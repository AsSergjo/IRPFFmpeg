#include "compact_mode.h"
#include "resource.h"
#include "util.h"

#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "dwmapi.lib")

static constexpr int kCompactClientWidth = 420;
static constexpr int kCompactClientHeight = 150;
static constexpr int kCompactBottomBarHeight = 30;
static constexpr int kCompactMinClientWidth = 330;
static constexpr int kCompactMinClientHeight = 118;
static constexpr UINT kTitleScrollTimerMs = 30;
static constexpr ULONGLONG kTitleScrollPauseMs = 3000;
static constexpr int kTitleScrollEndPaddingPx = 2;
static constexpr int kTitleScrollStartOvershootPx = -3;
static constexpr DWORD kDwmWindowCornerPreferenceAttribute = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
static constexpr int kDwmCornerDefault = 0; // DWMWCP_DEFAULT
static constexpr int kDwmCornerDoNotRound = 1; // DWMWCP_DONOTROUND

enum class CompactViewMode {
    Normal,
    Compact
};

enum class CompactTitleScrollState {
    ShowFull,
    Scrolling,
    WaitEnd
};

struct ControlLayoutSnapshot {
    int id = 0;
    RECT rect = {};
    bool visible = false;
};

static CompactViewMode g_viewMode = CompactViewMode::Normal;
static WINDOWPLACEMENT g_normalWindowPlacement = { sizeof(WINDOWPLACEMENT) };
static LONG_PTR g_normalWindowStyle = 0;
static LONG_PTR g_normalWindowExStyle = 0;
static bool g_normalWindowPlacementSaved = false;
static bool g_normalWindowWasTopmost = false;
static bool g_compactAlwaysOnTop = true;
static HWND g_hSpectrumHost = nullptr;
static HWND g_hCompactTitle = nullptr;
static HWND g_hCompactPreviousButton = nullptr;
static HWND g_hCompactRestoreButton = nullptr;
static HFONT g_hTitleFont = nullptr;
static HFONT g_hIconFont = nullptr;
static CompactModeCallbacks g_callbacks;
static std::vector<ControlLayoutSnapshot> g_normalControlLayout;
static int g_compactTitleScrollPosPx = 0;
static int g_compactTitleMaxScrollPosPx = 0;
static ULONGLONG g_compactTitleStateStartTick = 0;
static CompactTitleScrollState g_compactTitleScrollState = CompactTitleScrollState::ShowFull;
static std::wstring g_compactLastTitle;
static DWORD g_lastCoverClickTick = 0;
static POINT g_lastCoverClickPoint = {};
static DWORD g_lastTitleClickTick = 0;
static POINT g_lastTitleClickPoint = {};
static bool g_previousButtonHot = false;
static bool g_restoreButtonHot = false;
static bool g_dragging = false;
static POINT g_dragStartCursor = {};
static RECT g_dragStartWindow = {};

static HFONT GetCompactTitleFont()
{
    return g_hTitleFont
        ? g_hTitleFont
        : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

static void SetCompactDwmShadow(HWND hDlg, bool enable)
{
    if (!hDlg) {
        return;
    }

    BOOL compositionEnabled = FALSE;
    if (FAILED(DwmIsCompositionEnabled(&compositionEnabled)) || !compositionEnabled) {
        return;
    }

    DWMNCRENDERINGPOLICY policy = enable ? DWMNCRP_ENABLED : DWMNCRP_USEWINDOWSTYLE;
    DwmSetWindowAttribute(
        hDlg,
        DWMWA_NCRENDERING_POLICY,
        &policy,
        sizeof(policy));

    const int cornerPreference = enable ? kDwmCornerDoNotRound : kDwmCornerDefault;
    DwmSetWindowAttribute(
        hDlg,
        kDwmWindowCornerPreferenceAttribute,
        &cornerPreference,
        sizeof(cornerPreference));

    MARGINS margins = enable
        ? MARGINS{ 1, 1, 1, 1 }
        : MARGINS{ 0, 0, 0, 0 };
    DwmExtendFrameIntoClientArea(hDlg, &margins);
}

static void PostCompactRestoreCommand(HWND hDlg)
{
    if (!hDlg) {
        return;
    }

    PostMessageW(hDlg,
        WM_COMMAND,
        MAKEWPARAM(IDC_BUTTON_COMPACT_RESTORE, BN_CLICKED),
        reinterpret_cast<LPARAM>(g_hCompactRestoreButton));
}

static bool* GetCompactButtonHotFlag(HWND hWnd)
{
    switch (GetDlgCtrlID(hWnd))
    {
    case IDC_BUTTON_COMPACT_PREVIOUS:
        return &g_previousButtonHot;
    case IDC_BUTTON_COMPACT_RESTORE:
        return &g_restoreButtonHot;
    default:
        return nullptr;
    }
}

static bool IsCompactTitleDoubleClick(HWND hWnd, LPARAM lParam)
{
    DWORD now = GetTickCount();
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    ClientToScreen(hWnd, &pt);

    const int maxDx = GetSystemMetrics(SM_CXDOUBLECLK);
    const int maxDy = GetSystemMetrics(SM_CYDOUBLECLK);
    const bool isDoubleClick =
        g_lastTitleClickTick != 0 &&
        now - g_lastTitleClickTick <= GetDoubleClickTime() &&
        std::abs(pt.x - g_lastTitleClickPoint.x) <= maxDx &&
        std::abs(pt.y - g_lastTitleClickPoint.y) <= maxDy;

    g_lastTitleClickTick = now;
    g_lastTitleClickPoint = pt;

    if (isDoubleClick) {
        g_lastTitleClickTick = 0;
    }

    return isDoubleClick;
}

static POINT GetCompactContextMenuPoint(HWND hWnd, LPARAM lParam)
{
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (pt.x == -1 && pt.y == -1) {
        RECT rc = {};
        GetWindowRect(hWnd, &rc);
        pt.x = rc.left + (rc.right - rc.left) / 2;
        pt.y = rc.top + (rc.bottom - rc.top) / 2;
    }
    return pt;
}

static LRESULT CALLBACK CompactTitleSubclassProc(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDBLCLK:
        PostCompactRestoreCommand(GetParent(hWnd));
        return 0;

    case WM_LBUTTONDOWN:
        if (IsCompactTitleDoubleClick(hWnd, lParam)) {
            PostCompactRestoreCommand(GetParent(hWnd));
            return 0;
        }
        CompactModeBeginDrag(GetParent(hWnd));
        return 0;

    case WM_CONTEXTMENU:
        if (g_callbacks.showContextMenu) {
            g_callbacks.showContextMenu(GetParent(hWnd), GetCompactContextMenuPoint(hWnd, lParam));
            return 0;
        }
        break;

    case WM_NCDESTROY:
        RemoveWindowSubclass(hWnd, CompactTitleSubclassProc, subclassId);
        break;
    }

    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

static LRESULT CALLBACK CompactButtonSubclassProc(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (bool* hotFlag = GetCompactButtonHotFlag(hWnd); hotFlag && !*hotFlag) {
            *hotFlag = true;
            TRACKMOUSEEVENT tme = {};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hWnd;
            TrackMouseEvent(&tme);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;

    case WM_MOUSELEAVE:
        if (bool* hotFlag = GetCompactButtonHotFlag(hWnd)) {
            *hotFlag = false;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        break;

    case WM_NCDESTROY:
        if (bool* hotFlag = GetCompactButtonHotFlag(hWnd)) {
            *hotFlag = false;
        }
        RemoveWindowSubclass(hWnd, CompactButtonSubclassProc, subclassId);
        break;
    }

    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

static int MeasureTextWidthPx(HDC hdc, const std::wstring& text)
{
    if (!hdc || text.empty()) {
        return 0;
    }

    SIZE size = {};
    if (!GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size)) {
        return 0;
    }

    return size.cx;
}

static RECT GetChildClientRect(HWND hDlg, HWND hChild)
{
    RECT rc = {};
    if (!hDlg || !hChild || !GetWindowRect(hChild, &rc)) {
        return rc;
    }
    MapWindowPoints(HWND_DESKTOP, hDlg, reinterpret_cast<LPPOINT>(&rc), 2);
    return rc;
}

static std::wstring GetCompactTitleText()
{
    if (g_callbacks.getTitleText) {
        return g_callbacks.getTitleText();
    }
    return std::wstring();
}

static void ChangeCompactTitleScrollState(CompactTitleScrollState state)
{
    g_compactTitleScrollState = state;
    g_compactTitleStateStartTick = GetTickCount64();
    if (state == CompactTitleScrollState::ShowFull) {
        g_compactTitleScrollPosPx = 0;
    }
}

static void ResetCompactTitleScroll(const std::wstring& title)
{
    g_compactLastTitle = title;
    g_compactTitleScrollPosPx = 0;
    g_compactTitleMaxScrollPosPx = 0;
    ChangeCompactTitleScrollState(CompactTitleScrollState::ShowFull);
}

static void SaveNormalControlLayout(HWND hDlg)
{
    if (!hDlg || !g_normalControlLayout.empty()) {
        return;
    }

    static const int kControlIds[] = {
        IDC_STATIC_IMG,
        IDC_STATIC_SDL,
        IDC_LIST_URL,
        IDC_STATIC_NOW_PLAYING_BAR,
        IDC_LIST2,
        IDC_BUTTON_PP,
        IDC_BUTTON_REV,
        IDC_BUTTON_FORV,
        IDC_BUTTON_PREVIOUS_STATION,
        IDC_BUTTON_VOLUME,
        IDC_BUTTON_EQ,
        IDC_BUTTON_REC,
        IDC_ST_SETTING
    };

    for (int id : kControlIds) {
        HWND hControl = GetDlgItem(hDlg, id);
        if (!hControl) {
            continue;
        }

        ControlLayoutSnapshot snapshot;
        snapshot.id = id;
        snapshot.rect = GetChildClientRect(hDlg, hControl);
        snapshot.visible = IsWindowVisible(hControl) != FALSE;
        g_normalControlLayout.push_back(snapshot);
    }
}

static void EnsureCompactControls(HWND hDlg)
{
    if (!hDlg) {
        return;
    }

    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    if (!g_hCompactTitle || !IsWindow(g_hCompactTitle)) {
        g_hCompactTitle = CreateWindowExW(
            0,
            L"STATIC",
            nullptr,
            WS_CHILD | SS_OWNERDRAW | SS_NOTIFY,
            0, 0, 0, 0,
            hDlg,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATIC_COMPACT_TITLE)),
            hInstance,
            nullptr);
        if (g_hCompactTitle && g_hTitleFont) {
            SendMessageW(g_hCompactTitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_hTitleFont), FALSE);
        }
        if (g_hCompactTitle) {
            SetWindowSubclass(g_hCompactTitle, CompactTitleSubclassProc, 1, 0);
        }
    }

    if (!g_hCompactRestoreButton || !IsWindow(g_hCompactRestoreButton)) {
        g_hCompactRestoreButton = CreateWindowExW(
            0,
            L"BUTTON",
            nullptr,
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0,
            hDlg,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BUTTON_COMPACT_RESTORE)),
            hInstance,
            nullptr);
        if (g_hCompactRestoreButton && g_hIconFont) {
            SendMessageW(g_hCompactRestoreButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_hIconFont), FALSE);
        }
        if (g_hCompactRestoreButton) {
            SetWindowSubclass(g_hCompactRestoreButton, CompactButtonSubclassProc, 1, 0);
        }
    }

    if (!g_hCompactPreviousButton || !IsWindow(g_hCompactPreviousButton)) {
        g_hCompactPreviousButton = CreateWindowExW(
            0,
            L"BUTTON",
            nullptr,
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0,
            hDlg,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BUTTON_COMPACT_PREVIOUS)),
            hInstance,
            nullptr);
        if (g_hCompactPreviousButton && g_hIconFont) {
            SendMessageW(g_hCompactPreviousButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_hIconFont), FALSE);
        }
        if (g_hCompactPreviousButton) {
            SetWindowSubclass(g_hCompactPreviousButton, CompactButtonSubclassProc, 1, 0);
        }
    }

    if (g_callbacks.refreshTooltips) {
        g_callbacks.refreshTooltips(hDlg);
    }
}

static void ClampWindowToWorkArea(int& x, int& y, int width, int height)
{
    RECT work = {};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        return;
    }

    if (x + width > work.right) {
        x = work.right - width;
    }
    if (y + height > work.bottom) {
        y = work.bottom - height;
    }
    if (x < work.left) {
        x = work.left;
    }
    if (y < work.top) {
        y = work.top;
    }
}

static SIZE GetWindowSizeForClient(HWND hDlg, int clientWidth, int clientHeight)
{
    RECT rc = { 0, 0, clientWidth, clientHeight };
    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hDlg, GWL_STYLE));
    DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hDlg, GWL_EXSTYLE));
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);

    SIZE size = {};
    size.cx = rc.right - rc.left;
    size.cy = rc.bottom - rc.top;
    return size;
}

static HWND GetCompactZOrderAfterEnter()
{
    return g_compactAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
}

static void ApplyCompactTopmostStyle(HWND hDlg)
{
    if (!hDlg || !CompactModeIsActive()) {
        return;
    }

    LONG_PTR exStyle = GetWindowLongPtrW(hDlg, GWL_EXSTYLE);
    const LONG_PTR desiredExStyle = g_compactAlwaysOnTop
        ? (exStyle | WS_EX_TOPMOST)
        : (exStyle & ~WS_EX_TOPMOST);
    if (desiredExStyle != exStyle) {
        SetWindowLongPtrW(hDlg, GWL_EXSTYLE, desiredExStyle);
    }
}

void CompactModeApplyZOrder(HWND hDlg)
{
    if (!hDlg || !CompactModeIsActive()) {
        return;
    }

    ApplyCompactTopmostStyle(hDlg);
    SetWindowPos(hDlg,
        g_compactAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static void RestoreNormalZOrder(HWND hDlg)
{
    if (!hDlg) {
        return;
    }

    SetWindowPos(hDlg,
        g_normalWindowWasTopmost ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static void DrawCompactTitle(HWND hWnd, HDC hdc)
{
    RECT rc = {};
    GetClientRect(hWnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, width, height);
    if (!memDC || !memBmp) {
        if (memBmp) {
            DeleteObject(memBmp);
        }
        if (memDC) {
            DeleteDC(memDC);
        }
        return;
    }

    HGDIOBJ oldBmp = SelectObject(memDC, memBmp);
    RECT localRc = { 0, 0, width, height };

    HBRUSH hBack = CreateSolidBrush(RGB(246, 247, 250));
    FillRect(memDC, &localRc, hBack);
    DeleteObject(hBack);

    std::wstring title = GetCompactTitleText();
    if (title != g_compactLastTitle) {
        ResetCompactTitleScroll(title);
    }

    RECT textRc = localRc;
    textRc.left += 2;
    textRc.right -= 4;

    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(10, 18, 32));
    HFONT hOldFont = reinterpret_cast<HFONT>(SelectObject(
        memDC,
        GetCompactTitleFont()));

    const int textWidth = MeasureTextWidthPx(memDC, title);
    const int availableWidth = (std::max)(1, static_cast<int>(textRc.right - textRc.left));

    const int savedDc = SaveDC(memDC);
    IntersectClipRect(memDC, textRc.left, textRc.top, textRc.right, textRc.bottom);

    if (textWidth <= availableWidth) {
        RECT drawRc = textRc;
        DrawTextW(memDC, title.c_str(), -1, &drawRc,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    else {
        RECT drawRc = textRc;
        if (g_compactTitleScrollState != CompactTitleScrollState::ShowFull) {
            drawRc.left -= g_compactTitleScrollPosPx;
        }
        drawRc.right = drawRc.left + textWidth + 8;
        DrawTextW(memDC, title.c_str(), -1, &drawRc,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    RestoreDC(memDC, savedDc);
    SelectObject(memDC, hOldFont);
    BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
}

static const wchar_t* GetCompactButtonGlyph(UINT controlId)
{
    switch (controlId)
    {
    case IDC_BUTTON_COMPACT_PREVIOUS:
        return L"\uE8EE";
    case IDC_BUTTON_COMPACT_RESTORE:
        return L"\uE8A7";
    default:
        return L"";
    }
}

static bool IsCompactButtonHot(UINT controlId)
{
    switch (controlId)
    {
    case IDC_BUTTON_COMPACT_PREVIOUS:
        return g_previousButtonHot;
    case IDC_BUTTON_COMPACT_RESTORE:
        return g_restoreButtonHot;
    default:
        return false;
    }
}

static void DrawCompactIconButton(LPDRAWITEMSTRUCT pDIS)
{
    const bool pressed = (pDIS->itemState & ODS_SELECTED) != 0;
    const bool hot = IsCompactButtonHot(pDIS->CtlID) || ((pDIS->itemState & ODS_HOTLIGHT) != 0);

    HBRUSH hBack = CreateSolidBrush(RGB(246, 247, 250));
    FillRect(pDIS->hDC, &pDIS->rcItem, hBack);
    DeleteObject(hBack);

    RECT rcButton = pDIS->rcItem;
    InflateRect(&rcButton, -2, -2);

    if (hot || pressed) {
        COLORREF fill = pressed ? RGB(218, 224, 232) : RGB(232, 236, 242);
        COLORREF border = pressed ? RGB(162, 171, 184) : RGB(197, 204, 216);

        HPEN hPen = CreatePen(PS_SOLID, 1, border);
        HBRUSH hBrush = CreateSolidBrush(fill);
        HPEN hOldPen = reinterpret_cast<HPEN>(SelectObject(pDIS->hDC, hPen));
        HBRUSH hOldBrush = reinterpret_cast<HBRUSH>(SelectObject(pDIS->hDC, hBrush));
        RoundRect(pDIS->hDC, rcButton.left, rcButton.top, rcButton.right, rcButton.bottom, 5, 5);
        SelectObject(pDIS->hDC, hOldBrush);
        SelectObject(pDIS->hDC, hOldPen);
        DeleteObject(hBrush);
        DeleteObject(hPen);
    }

    HFONT hDrawFont = g_hIconFont;
    HFONT hSmallIconFont = nullptr;
    if (g_hIconFont) {
        LOGFONTW lf = {};
        if (GetObjectW(g_hIconFont, sizeof(lf), &lf)) {
            lf.lfHeight = -MulDiv(13, GetDeviceCaps(pDIS->hDC, LOGPIXELSY), 72);
            hSmallIconFont = CreateFontIndirectW(&lf);
            if (hSmallIconFont) {
                hDrawFont = hSmallIconFont;
            }
        }
    }

    HFONT hOldFont = reinterpret_cast<HFONT>(SelectObject(
        pDIS->hDC,
        hDrawFont ? hDrawFont : GetStockObject(DEFAULT_GUI_FONT)));
    SetTextColor(pDIS->hDC, pressed ? RGB(18, 24, 32) : RGB(40, 47, 58));
    SetBkMode(pDIS->hDC, TRANSPARENT);
    DrawTextW(pDIS->hDC, GetCompactButtonGlyph(pDIS->CtlID), -1, &rcButton,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(pDIS->hDC, hOldFont);
    if (hSmallIconFont) {
        DeleteObject(hSmallIconFont);
    }

}

void CompactModeConfigure(
    HWND,
    HWND hSpectrumHost,
    HFONT hTitleFont,
    HFONT hIconFont,
    const CompactModeCallbacks& callbacks)
{
    g_hSpectrumHost = hSpectrumHost;
    g_hTitleFont = hTitleFont;
    g_hIconFont = hIconFont;
    g_callbacks = callbacks;
}

bool CompactModeIsActive()
{
    return g_viewMode == CompactViewMode::Compact;
}

void CompactModeLayout(HWND hDlg)
{
    if (!hDlg || !CompactModeIsActive()) {
        return;
    }

    RECT rcClient = {};
    GetClientRect(hDlg, &rcClient);

    const int clientWidth = (std::max)(1, static_cast<int>(rcClient.right - rcClient.left));
    const int clientHeight = (std::max)(1, static_cast<int>(rcClient.bottom - rcClient.top));
    const int bottomHeight = (std::min)(kCompactBottomBarHeight, (std::max)(24, clientHeight / 3));
    const int spectrumHeight = (std::max)(48, clientHeight - bottomHeight);
    const int buttonSize = (std::min)(34, (std::max)(26, bottomHeight - 4));
    const int buttonMargin = 3;
    const int buttonGap = 2;
    const int restoreButtonX = clientWidth - buttonSize - buttonMargin;
    const int previousButtonX = restoreButtonX - buttonGap - buttonSize;
    const int titleLeft = 7;
    const int titleTop = spectrumHeight + 1;
    const int titleHeight = (std::max)(1, bottomHeight - 2);
    const int titleRight = previousButtonX - 4;

    if (g_hSpectrumHost) {
        MoveWindow(g_hSpectrumHost, 0, 0, clientWidth, spectrumHeight, TRUE);
        ShowWindow(g_hSpectrumHost, SW_SHOW);
    }

    if (g_hCompactTitle) {
        const int titleWidth = (std::max)(1, titleRight - titleLeft);
        MoveWindow(g_hCompactTitle, titleLeft, titleTop, titleWidth, titleHeight, TRUE);
        ShowWindow(g_hCompactTitle, SW_SHOW);
    }

    if (g_hCompactRestoreButton) {
        const int buttonY = spectrumHeight + (bottomHeight - buttonSize) / 2;
        MoveWindow(g_hCompactRestoreButton, restoreButtonX, buttonY, buttonSize, buttonSize, TRUE);
        ShowWindow(g_hCompactRestoreButton, SW_SHOW);
    }

    if (g_hCompactPreviousButton) {
        const int buttonY = spectrumHeight + (bottomHeight - buttonSize) / 2;
        MoveWindow(g_hCompactPreviousButton, previousButtonX, buttonY, buttonSize, buttonSize, TRUE);
        ShowWindow(g_hCompactPreviousButton, SW_SHOW);
    }

    if (HWND hCover = getCoverRendererWindow()) {
        ShowWindow(hCover, SW_HIDE);
    }
}

void CompactModeDrawChrome(HWND hDlg, HDC hdc)
{
    RECT rcClient = {};
    GetClientRect(hDlg, &rcClient);
    const int clientHeight = rcClient.bottom - rcClient.top;
    const int bottomTop = (std::max)(0, clientHeight - kCompactBottomBarHeight);

    RECT rcSpectrum = rcClient;
    rcSpectrum.bottom = bottomTop;
    HBRUSH hBlack = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rcSpectrum, hBlack);
    DeleteObject(hBlack);

    RECT rcBar = rcClient;
    rcBar.top = bottomTop;
    HBRUSH hBar = CreateSolidBrush(RGB(246, 247, 250));
    FillRect(hdc, &rcBar, hBar);
    DeleteObject(hBar);

    HPEN hLinePen = CreatePen(PS_SOLID, 1, RGB(224, 228, 234));
    HPEN hOldPen = reinterpret_cast<HPEN>(SelectObject(hdc, hLinePen));
    MoveToEx(hdc, rcClient.left, bottomTop, nullptr);
    LineTo(hdc, rcClient.right, bottomTop);
    SelectObject(hdc, hOldPen);
    DeleteObject(hLinePen);
}

void CompactModeAdvanceTitleScroll(HWND)
{
    if (!CompactModeIsActive() || !g_hCompactTitle || !IsWindow(g_hCompactTitle)) {
        return;
    }

    RECT rc = {};
    GetClientRect(g_hCompactTitle, &rc);
    const int availableWidth = (std::max)(1, static_cast<int>(rc.right - rc.left - 6));

    HDC hdc = GetDC(g_hCompactTitle);
    if (!hdc) {
        return;
    }

    HFONT hOldFont = reinterpret_cast<HFONT>(SelectObject(
        hdc,
        GetCompactTitleFont()));
    std::wstring title = GetCompactTitleText();
    const int textWidth = MeasureTextWidthPx(hdc, title);
    const int maxScrollPos = (std::max)(0, textWidth - availableWidth + kTitleScrollEndPaddingPx);
    SelectObject(hdc, hOldFont);
    ReleaseDC(g_hCompactTitle, hdc);

    if (title != g_compactLastTitle) {
        ResetCompactTitleScroll(title);
        InvalidateRect(g_hCompactTitle, nullptr, FALSE);
        return;
    }

    if (maxScrollPos <= 0) {
        if (g_compactTitleScrollPosPx != 0 || g_compactTitleMaxScrollPosPx != 0 ||
            g_compactTitleScrollState != CompactTitleScrollState::ShowFull) {
            ResetCompactTitleScroll(title);
            InvalidateRect(g_hCompactTitle, nullptr, FALSE);
        }
        return;
    }

    g_compactTitleMaxScrollPosPx = maxScrollPos;
    if (g_compactTitleScrollPosPx > maxScrollPos) {
        g_compactTitleScrollPosPx = maxScrollPos;
        InvalidateRect(g_hCompactTitle, nullptr, FALSE);
        return;
    }

    ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsedMs = now - g_compactTitleStateStartTick;
    const int oldScrollPos = g_compactTitleScrollPosPx;
    CompactTitleScrollState oldState = g_compactTitleScrollState;

    switch (g_compactTitleScrollState)
    {
    case CompactTitleScrollState::ShowFull:
        g_compactTitleScrollPosPx = 0;
        if (elapsedMs >= kTitleScrollPauseMs) {
            ChangeCompactTitleScrollState(CompactTitleScrollState::Scrolling);
        }
        break;

    case CompactTitleScrollState::Scrolling:
        ++g_compactTitleScrollPosPx;
        if (g_compactTitleScrollPosPx >= maxScrollPos) {
            g_compactTitleScrollPosPx = maxScrollPos;
            ChangeCompactTitleScrollState(CompactTitleScrollState::WaitEnd);
        }
        break;

    case CompactTitleScrollState::WaitEnd:
        if (elapsedMs >= kTitleScrollPauseMs) {
            --g_compactTitleScrollPosPx;
            if (g_compactTitleScrollPosPx < kTitleScrollStartOvershootPx) {
                ChangeCompactTitleScrollState(CompactTitleScrollState::ShowFull);
            }
        }
        break;
    }

    if (g_compactTitleScrollPosPx != oldScrollPos ||
        g_compactTitleScrollState != oldState) {
        InvalidateRect(g_hCompactTitle, nullptr, FALSE);
    }
}

void CompactModeInvalidateTitle()
{
    if (CompactModeIsActive() && g_hCompactTitle) {
        InvalidateRect(g_hCompactTitle, nullptr, FALSE);
    }
}

bool CompactModeHandleGetMinMaxInfo(HWND hDlg, LPARAM lParam)
{
    if (!CompactModeIsActive()) {
        return false;
    }

    MINMAXINFO* minMax = reinterpret_cast<MINMAXINFO*>(lParam);
    if (minMax) {
        SIZE minSize = GetWindowSizeForClient(hDlg, kCompactMinClientWidth, kCompactMinClientHeight);
        minMax->ptMinTrackSize.x = minSize.cx;
        minMax->ptMinTrackSize.y = minSize.cy;
    }
    return true;
}

bool CompactModeHandleNcHitTest(HWND hDlg, LPARAM lParam, INT_PTR& result)
{
    if (!CompactModeIsActive()) {
        return false;
    }

    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (g_hCompactRestoreButton) {
        RECT rcButton = {};
        GetWindowRect(g_hCompactRestoreButton, &rcButton);
        if (PtInRect(&rcButton, pt)) {
            result = HTCLIENT;
            return true;
        }
    }
    if (g_hCompactPreviousButton) {
        RECT rcButton = {};
        GetWindowRect(g_hCompactPreviousButton, &rcButton);
        if (PtInRect(&rcButton, pt)) {
            result = HTCLIENT;
            return true;
        }
    }

    RECT rcWindow = {};
    GetWindowRect(hDlg, &rcWindow);
    if (PtInRect(&rcWindow, pt)) {
        result = HTCLIENT;
        return true;
    }

    return false;
}

bool CompactModeDrawItem(LPDRAWITEMSTRUCT pDIS)
{
    if (!pDIS) {
        return false;
    }

    if (pDIS->CtlType == ODT_STATIC && pDIS->CtlID == IDC_STATIC_COMPACT_TITLE) {
        DrawCompactTitle(pDIS->hwndItem, pDIS->hDC);
        return true;
    }

    if (pDIS->CtlType == ODT_BUTTON &&
        (pDIS->CtlID == IDC_BUTTON_COMPACT_RESTORE ||
            pDIS->CtlID == IDC_BUTTON_COMPACT_PREVIOUS)) {
        DrawCompactIconButton(pDIS);
        return true;
    }

    return false;
}

void CompactModeEnter(HWND hDlg)
{
    if (!hDlg || CompactModeIsActive()) {
        return;
    }

    SaveNormalControlLayout(hDlg);
    EnsureCompactControls(hDlg);

    g_normalWindowPlacement.length = sizeof(g_normalWindowPlacement);
    g_normalWindowPlacementSaved = GetWindowPlacement(hDlg, &g_normalWindowPlacement) != FALSE;
    g_normalWindowStyle = GetWindowLongPtrW(hDlg, GWL_STYLE);
    g_normalWindowExStyle = GetWindowLongPtrW(hDlg, GWL_EXSTYLE);
    g_normalWindowWasTopmost = (g_normalWindowExStyle & WS_EX_TOPMOST) != 0;

    for (const ControlLayoutSnapshot& snapshot : g_normalControlLayout) {
        HWND hControl = GetDlgItem(hDlg, snapshot.id);
        if (!hControl || snapshot.id == IDC_STATIC_SDL) {
            continue;
        }
        ShowWindow(hControl, SW_HIDE);
    }

    if (HWND hCover = getCoverRendererWindow()) {
        ShowWindow(hCover, SW_HIDE);
    }

    LONG_PTR compactStyle = g_normalWindowStyle;
    compactStyle &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX |
        WS_MAXIMIZEBOX | WS_DLGFRAME);
    compactStyle |= WS_POPUP | WS_BORDER | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    LONG_PTR compactExStyle = g_normalWindowExStyle;
    compactExStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE);
    compactExStyle = g_compactAlwaysOnTop
        ? (compactExStyle | WS_EX_TOPMOST)
        : (compactExStyle & ~WS_EX_TOPMOST);

    SetWindowLongPtrW(hDlg, GWL_STYLE, compactStyle);
    SetWindowLongPtrW(hDlg, GWL_EXSTYLE, compactExStyle);
    SetCompactDwmShadow(hDlg, true);

    RECT rcWindow = {};
    GetWindowRect(hDlg, &rcWindow);
    SIZE compactWindowSize = GetWindowSizeForClient(hDlg, kCompactClientWidth, kCompactClientHeight);
    int x = rcWindow.left;
    int y = rcWindow.top;
    ClampWindowToWorkArea(x, y, compactWindowSize.cx, compactWindowSize.cy);

    g_viewMode = CompactViewMode::Compact;
    ResetCompactTitleScroll(std::wstring());

    SetWindowPos(hDlg, GetCompactZOrderAfterEnter(), x, y, compactWindowSize.cx, compactWindowSize.cy,
        SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    CompactModeLayout(hDlg);
    CompactModeApplyZOrder(hDlg);
    SetTimer(hDlg, IDT_COMPACT_TITLE_SCROLL, kTitleScrollTimerMs, nullptr);
    SetFocus(hDlg);
    InvalidateRect(hDlg, nullptr, TRUE);
}

void CompactModeExit(HWND hDlg)
{
    if (!hDlg || !CompactModeIsActive()) {
        return;
    }

    CompactModeEndDrag(hDlg);
    KillTimer(hDlg, IDT_COMPACT_TITLE_SCROLL);

    if (g_hCompactTitle) {
        ShowWindow(g_hCompactTitle, SW_HIDE);
    }
    if (g_hCompactPreviousButton) {
        ShowWindow(g_hCompactPreviousButton, SW_HIDE);
    }
    if (g_hCompactRestoreButton) {
        ShowWindow(g_hCompactRestoreButton, SW_HIDE);
    }

    g_viewMode = CompactViewMode::Normal;
    SetCompactDwmShadow(hDlg, false);

    if (g_normalWindowStyle != 0) {
        SetWindowLongPtrW(hDlg, GWL_STYLE, g_normalWindowStyle);
        SetWindowLongPtrW(hDlg, GWL_EXSTYLE, g_normalWindowExStyle);
    }

    const bool hasCoverRenderer = getCoverRendererWindow() != nullptr;
    for (const ControlLayoutSnapshot& snapshot : g_normalControlLayout) {
        HWND hControl = GetDlgItem(hDlg, snapshot.id);
        if (!hControl) {
            continue;
        }
        const int width = snapshot.rect.right - snapshot.rect.left;
        const int height = snapshot.rect.bottom - snapshot.rect.top;
        MoveWindow(hControl, snapshot.rect.left, snapshot.rect.top, width, height, FALSE);

        bool shouldShow = snapshot.visible;
        if (snapshot.id == IDC_STATIC_IMG && hasCoverRenderer) {
            shouldShow = false;
        }
        ShowWindow(hControl, shouldShow ? SW_SHOW : SW_HIDE);
    }

    if (g_normalWindowPlacementSaved) {
        g_normalWindowPlacement.length = sizeof(g_normalWindowPlacement);
        SetWindowPlacement(hDlg, &g_normalWindowPlacement);
        RestoreNormalZOrder(hDlg);
    }
    else {
        SetWindowPos(hDlg, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        RestoreNormalZOrder(hDlg);
    }

    if (HWND hCover = getCoverRendererWindow()) {
        if (HWND hStatic = GetDlgItem(hDlg, IDC_STATIC_IMG)) {
            ShowWindow(hStatic, SW_HIDE);
        }
        ShowWindow(hCover, SW_SHOW);
        SetWindowPos(hCover, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    redrawCoverImage(hDlg);
    if (g_callbacks.invalidateNormalText) {
        g_callbacks.invalidateNormalText(hDlg);
    }
    InvalidateRect(hDlg, nullptr, TRUE);
}

void CompactModeSetAlwaysOnTop(HWND hDlg, bool enabled)
{
    g_compactAlwaysOnTop = enabled;
    CompactModeApplyZOrder(hDlg);
}

void CompactModeBeginDrag(HWND hDlg)
{
    if (!hDlg || !CompactModeIsActive()) {
        return;
    }

    if (!GetCursorPos(&g_dragStartCursor) || !GetWindowRect(hDlg, &g_dragStartWindow)) {
        return;
    }

    g_dragging = true;
    SetCapture(hDlg);
}

bool CompactModeHandleMouseMove(HWND hDlg)
{
    if (!hDlg || !g_dragging) {
        return false;
    }

    if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
        CompactModeEndDrag(hDlg);
        return true;
    }

    POINT pt = {};
    if (!GetCursorPos(&pt)) {
        return true;
    }

    const int dx = pt.x - g_dragStartCursor.x;
    const int dy = pt.y - g_dragStartCursor.y;
    const UINT flags = g_compactAlwaysOnTop
        ? (SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)
        : (SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SetWindowPos(hDlg,
        g_compactAlwaysOnTop ? HWND_TOPMOST : nullptr,
        g_dragStartWindow.left + dx,
        g_dragStartWindow.top + dy,
        0, 0,
        flags);
    return true;
}

void CompactModeEndDrag(HWND hDlg)
{
    if (!g_dragging) {
        return;
    }

    g_dragging = false;
    if (hDlg && GetCapture() == hDlg) {
        ReleaseCapture();
    }
}

static LRESULT CALLBACK CoverRendererSubclassProc(
    HWND hWnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR refData)
{
    switch (msg)
    {
    case WM_LBUTTONDBLCLK:
        CompactModeEnter(reinterpret_cast<HWND>(refData));
        return 0;

    case WM_LBUTTONDOWN:
    {
        DWORD now = GetTickCount();
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hWnd, &pt);

        const int maxDx = GetSystemMetrics(SM_CXDOUBLECLK);
        const int maxDy = GetSystemMetrics(SM_CYDOUBLECLK);
        const bool isDoubleClick =
            g_lastCoverClickTick != 0 &&
            now - g_lastCoverClickTick <= GetDoubleClickTime() &&
            std::abs(pt.x - g_lastCoverClickPoint.x) <= maxDx &&
            std::abs(pt.y - g_lastCoverClickPoint.y) <= maxDy;

        g_lastCoverClickTick = now;
        g_lastCoverClickPoint = pt;

        if (isDoubleClick) {
            g_lastCoverClickTick = 0;
            CompactModeEnter(reinterpret_cast<HWND>(refData));
            return 0;
        }
        break;
    }

    case WM_NCDESTROY:
        RemoveWindowSubclass(hWnd, CoverRendererSubclassProc, subclassId);
        break;
    }

    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

void CompactModeInstallCoverSubclass(HWND hDlg)
{
    HWND hCoverRenderer = getCoverRendererWindow();
    if (!hDlg || !hCoverRenderer) {
        return;
    }

    SetWindowSubclass(hCoverRenderer, CoverRendererSubclassProc, 1, reinterpret_cast<DWORD_PTR>(hDlg));
}
