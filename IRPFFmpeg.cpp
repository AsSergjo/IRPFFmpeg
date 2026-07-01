#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#include "framework.h"
#include "IRPFFmpeg.h"
#include "cover_art.h"
#include "file_recording.h"
#include "language_manager.h"
#include "resource.h"

#include <windowsx.h> 
#include <commctrl.h> 
#include <vector>
#include <string>
#include <memory>
#include <map>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <cstdarg>
#include <cctype>
#include <cwctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <wininet.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <shlwapi.h> // Для PathCombine
#include <shellapi.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "shell32.lib")

const int MAX_RECONNECT_ATTEMPTS = 3;
const int RECONNECT_DELAY_MS = 2000;
const wchar_t CLASS_NAME[] = L"IRP+ffmpeg";

#define ID_TIMER_IMAGE_URL 3
#define ID_TIMER_METADATA 4
#define IDT_COVER_RESTORE 5
#define IDT_TRACK_TOAST_HIDE 6
static constexpr UINT kTrayIconId = 1;
static constexpr int kTrackToastSize = 300;
static constexpr int kTrackToastMargin = 18;
static const wchar_t TRACK_TOAST_CLASS[] = L"IRPFFmpegTrackToast";
static const wchar_t TRACK_TOAST_TEXT_CLASS[] = L"IRPFFmpegTrackToastText";
// ------------------------------- 
// Global UI Handles
// ------------------------------- 
HWND g_hMainWnd = NULL;
HWND g_hCoverArt = NULL;
HWND g_hPlaylist = NULL;
HWND g_hHistory = NULL;
HWND g_hStatic = NULL;
HBRUSH g_hbrBlack = nullptr;
HWND g_hBtnPlayPause, g_hBtnStop, g_hBtnOpen, g_hBtnPrev, g_hBtnNext;
HWND g_hSliderVolume, g_hSliderTreble, g_hSliderBass;
HWND g_hLabelVolume, g_hLabelTreble, g_hLabelBass;
HWND g_hNowPlayingBar = NULL;

// ------------------------------- 
// Global variables for Audio Logic
// ------------------------------- 
std::atomic<bool> running(false);
std::atomic_bool g_suppressFfmpegDecoderLog(false);
std::atomic_bool g_audioStreamInfoAllowed(false);
std::atomic_bool g_enableDebugLogFile(false);// true - включить логирование в файл debug.log, false - отключить
std::string current_track;
std::string current_metadata;
std::mutex metadata_mutex;
std::mutex g_coverFileMutex;
std::vector<std::string> track_history;
std::atomic<bool> g_stopImageThread(false);

std::thread g_playbackControlThread;

int reconnect_attempts = 0;
AVFormatContext* formatCtx = nullptr;
AVCodecContext* codecCtx = nullptr;
AVFilterGraph* filterGraph = nullptr;
AVFilterContext* filter_abuf = nullptr;
AVFilterContext* filter_aeq = nullptr;
AVFilterContext* filter_avol = nullptr;
AVFilterContext* filter_asink = nullptr;
std::atomic<float> current_volume(0.5f);
std::atomic<float> current_eq_gain(10.0f);
std::atomic<float> current_eq_gain_bass(2.0f);
std::atomic<unsigned long> g_playbackGeneration(0);
bool g_enableStereoWidth = true;
bool g_enableDynamicAutoVolume = false;
bool g_enableLufsGainNormalizer = false;
bool g_enableExciter = false;
bool g_enableDeepBass = false;
bool g_enableLimiterGainRider = true;
bool g_enableIcyStationNameUpdates = true;
bool g_minimizeToTray = true;
bool g_showTrackToastInTray = true;
bool g_trackToastPositionSaved = false;
int g_trackToastX = 0;
int g_trackToastY = 0;
int g_stereoWidthPercent = 30;
std::thread g_playbackThread;
std::string g_currentUrl;

std::atomic_bool g_quit_flag(false);

std::thread g_imageUrlThread;
std::atomic<bool> g_bIsImageUrlThreadRunning(false);
std::atomic<bool> g_playbackThreadRunning(false);
CRITICAL_SECTION g_url_vec_cs;

int g_currentlyPlayingIndex = -1;
static int g_previousStationIndex = -1;
// очередь запросов для единственного control-потока
static std::vector<std::string> g_controlVector;
static std::mutex ControlVectorMutex;
static std::atomic<bool> g_controlThreadRunning(false);
static std::mutex g_stopPlaybackMutex;

// -------------------------------
static HFONT hButtonFont = NULL; // Font for button icons
static HFONT hListboxFont = NULL; // Font for listbox items
static HFONT hNowPlayingTitleFont = NULL;
const int TITLE_HEIGHT = 0;
extern const int alwaysVisibleExtent = 1024; // pixels
extern const int maxVisibleExtent = 2048;    // pixels

static HWND g_hSettingStatic = NULL;
bool rec_is_flac = false;

// Глобальный вектор для хранения плейлиста
static std::vector<PlaylistItem> playlist;
struct PlaylistNameResolvedPayload {
    unsigned long generation = 0;
    int index = -1;
    std::wstring url;
    std::wstring name;
};
static std::thread g_playlistNameResolveThread;
static std::atomic<bool> g_stopPlaylistNameResolveThread(false);
static std::atomic<unsigned long> g_playlistNameResolveGeneration(0);
static std::mutex g_ffmpegStatusMutex;
static std::string g_lastFfmpegStatusRaw;
static std::string g_lastFfmpegLogRaw;
static std::wstring g_nowPlayingTitle;
static std::wstring g_nowPlayingStatus = L"Остановлено";
static std::wstring g_nowPlayingStreamInfo;
static std::wstring g_nowPlayingElapsed;
static std::wstring g_limiterRiderStatus;
static std::wstring g_lufsNormalizerStatus;
static bool g_isReallyExiting = false;
static bool g_trayIconAdded = false;
static bool g_isInTray = false;
static bool g_trayHideBalloonShown = false;
static bool g_restoringFromTray = false;
static bool g_minimizeToTrayFromCaptionButton = false;
static HWND g_hTrackToast = nullptr;
static HWND g_hTrackToastText = nullptr;
static std::wstring g_trackToastTitle;
static SDL_Window* g_trackToastSdlWindow = nullptr;
static SDL_Renderer* g_trackToastRenderer = nullptr;
static bool g_trackToastDragging = false;
static POINT g_trackToastDragOffset = {};
static UINT g_wmTaskbarCreated = 0;
#define WM_RENDER_COVER (WM_USER + 100)
// формат: "Global\\{GUID}")
#define SINGLE_INSTANCE_MUTEX_NAME L"Global\\IRPffmpegInstanceMutex_1"

static void SanitizeM3ULine(std::string& text)
{
    for (char& ch : text) {
        if (ch == '\r' || ch == '\n') {
            ch = ' ';
        }
    }
}

static bool SavePlaylistToM3U(const std::wstring& filename, const std::vector<PlaylistItem>& items)
{
    std::ofstream file(filename, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::wstring msg = L"Could not write playlist file: " + filename;
        MessageBoxW(g_hMainWnd, msg.c_str(), L"File Error", MB_OK | MB_ICONERROR);
        return false;
    }

    file << "#EXTM3U\n";
    for (const PlaylistItem& item : items) {
        std::string name = wstring_to_utf8(item.name);
        std::string url = wstring_to_utf8(item.url);
        SanitizeM3ULine(name);
        SanitizeM3ULine(url);

        file << "#EXTINF:-1," << name << "\n";
        file << url << "\n";
    }

    file.flush();
    if (!file.good()) {
        std::wstring msg = L"Failed to save playlist file: " + filename;
        MessageBoxW(g_hMainWnd, msg.c_str(), L"File Error", MB_OK | MB_ICONERROR);
        return false;
    }

    return true;
}
// -------------------------------
// Callbacks and Helpers
// -------------------------------

// FFmpeg interrupt callback
static int interrupt_callback(void* ctx) {
    std::atomic_bool* q = static_cast<std::atomic_bool*>(ctx);
    return q->load();
}

// Logging function
std::mutex log_mutex;
void LogToUI(const std::string& message) {
	// Log to debug log file if enabled
    if (!g_enableDebugLogFile.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(log_mutex);
    std::ofstream log_file("debug_log.txt", std::ios_base::app);
    if (log_file.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
        localtime_s(&tm, &t);

        std::stringstream ss;
        ss << "[" << std::this_thread::get_id() << "] "
            << std::put_time(&tm, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count()
            << ": " << message << std::endl;

        log_file << ss.str();
        // Also write to debug output, as it's still useful if not deadlocking
        //OutputDebugStringA(ss.str().c_str());
    }
}

void PostFfmpegStatus(const std::wstring& status)
{
    if (!g_hMainWnd) return;

    auto* text = new std::wstring(status);
    if (!PostMessageW(g_hMainWnd, WM_APP_FFMPEG_STATUS, 0, (LPARAM)text)) {
        delete text;
    }
}

static void InvalidateNowPlayingBar(HWND hDlg)
{
    HWND hBar = GetDlgItem(hDlg, IDC_STATIC_NOW_PLAYING_BAR);
    if (hBar) {
        InvalidateRect(hBar, NULL, FALSE);
    }
}

static void UpdatePlayPauseButtonIcon(HWND hDlg)
{
    HWND hButton = GetDlgItem(hDlg, IDC_BUTTON_PP);
    if (hButton) {
        InvalidateRect(hButton, NULL, TRUE);
        UpdateWindow(hButton);
    }
}

static void ForceForegroundWindow(HWND hWnd)
{
    if (!hWnd || !IsWindow(hWnd)) {
        return;
    }

    if (IsIconic(hWnd)) {
        ShowWindow(hWnd, SW_RESTORE);
    }
    else {
        ShowWindow(hWnd, SW_SHOWNORMAL);
    }

    HWND hForeground = GetForegroundWindow();
    DWORD foregroundThreadId = GetWindowThreadProcessId(hForeground, nullptr);
    DWORD currentThreadId = GetCurrentThreadId();

    bool attached = false;
    if (foregroundThreadId != 0 && foregroundThreadId != currentThreadId) {
        attached = AttachThreadInput(foregroundThreadId, currentThreadId, TRUE) != FALSE;
    }

    BringWindowToTop(hWnd);
    SetActiveWindow(hWnd);
    SetForegroundWindow(hWnd);
    SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    if (attached) {
        AttachThreadInput(foregroundThreadId, currentThreadId, FALSE);
    }
}

static std::wstring GetPlaylistDisplayName(int index)
{
    if (index < 0 || index >= static_cast<int>(playlist.size())) {
        return std::wstring();
    }

    if (g_enableIcyStationNameUpdates &&
        !playlist[index].disable_name_icy &&
        !playlist[index].name_icy.empty()) {
        return playlist[index].name_icy;
    }

    return playlist[index].name;
}

static std::wstring GetNowPlayingTitleText()
{
    if (!g_nowPlayingTitle.empty()) {
        return g_nowPlayingTitle;
    }

    if (g_currentlyPlayingIndex >= 0 &&
        g_currentlyPlayingIndex < static_cast<int>(playlist.size())) {
        return GetPlaylistDisplayName(g_currentlyPlayingIndex);
    }

    return Tr("nowplaying.no_data", L"Нет данных о треке");
}

static void CleanupTrackToastSdl()
{
    if (g_trackToastRenderer) {
        SDL_DestroyRenderer(g_trackToastRenderer);
        g_trackToastRenderer = nullptr;
    }
    if (g_trackToastSdlWindow) {
        SDL_DestroyWindow(g_trackToastSdlWindow);
        g_trackToastSdlWindow = nullptr;
    }
}

static bool EnsureTrackToastSdl(HWND hWnd)
{
    if (g_trackToastRenderer) {
        return true;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2");
    g_trackToastSdlWindow = SDL_CreateWindowFrom(hWnd);
    if (!g_trackToastSdlWindow) {
        return false;
    }

    g_trackToastRenderer = SDL_CreateRenderer(g_trackToastSdlWindow, -1, SDL_RENDERER_SOFTWARE);
    if (!g_trackToastRenderer) {
        g_trackToastRenderer = SDL_CreateRenderer(g_trackToastSdlWindow, -1, SDL_RENDERER_ACCELERATED);
    }

    if (!g_trackToastRenderer) {
        CleanupTrackToastSdl();
        return false;
    }

    SDL_SetRenderDrawBlendMode(g_trackToastRenderer, SDL_BLENDMODE_BLEND);
    return true;
}

static constexpr int kTrackToastTextPaddingX = 12;
static constexpr int kTrackToastTextPaddingY = 4;
static constexpr int kTrackToastMaxTextLines = 3;

static std::wstring NormalizeTrackToastTitleText(const std::wstring& text)
{
    std::wstring normalized;
    normalized.reserve(text.size());

    bool previousWasSpace = false;
    for (wchar_t ch : text) {
        if (std::iswspace(ch)) {
            if (!previousWasSpace) {
                normalized.push_back(L' ');
                previousWasSpace = true;
            }
        }
        else {
            normalized.push_back(ch);
            previousWasSpace = false;
        }
    }

    while (!normalized.empty() && normalized.front() == L' ') {
        normalized.erase(normalized.begin());
    }
    while (!normalized.empty() && normalized.back() == L' ') {
        normalized.pop_back();
    }

    return normalized;
}

static int MeasureTrackToastTextWidth(HDC hdc, const std::wstring& text)
{
    SIZE size = {};
    if (text.empty()) {
        return 0;
    }
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size);
    return size.cx;
}

static std::wstring EllipsizeTrackToastLine(HDC hdc, const std::wstring& text, int maxWidth)
{
    static const std::wstring ellipsis = L"...";
    if (MeasureTrackToastTextWidth(hdc, text) <= maxWidth) {
        return text;
    }
    if (MeasureTrackToastTextWidth(hdc, ellipsis) > maxWidth) {
        return std::wstring();
    }

    std::wstring result = text;
    while (!result.empty()) {
        result.pop_back();
        while (!result.empty() && result.back() == L' ') {
            result.pop_back();
        }

        std::wstring candidate = result + ellipsis;
        if (MeasureTrackToastTextWidth(hdc, candidate) <= maxWidth) {
            return candidate;
        }
    }

    return ellipsis;
}

static std::vector<std::wstring> BuildTrackToastTextLines(HDC hdc, int width)
{
    const int maxTextWidth = (std::max)(1, width - kTrackToastTextPaddingX * 2);
    const std::wstring text = NormalizeTrackToastTitleText(g_trackToastTitle);

    std::vector<std::wstring> words;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t next = text.find(L' ', pos);
        if (next == std::wstring::npos) {
            words.push_back(text.substr(pos));
            break;
        }
        if (next > pos) {
            words.push_back(text.substr(pos, next - pos));
        }
        pos = next + 1;
    }

    std::vector<std::wstring> lines;
    std::wstring current;
    size_t wordIndex = 0;

    while (wordIndex < words.size() && lines.size() < kTrackToastMaxTextLines) {
        const std::wstring& word = words[wordIndex];
        std::wstring candidate = current.empty() ? word : current + L" " + word;

        if (MeasureTrackToastTextWidth(hdc, candidate) <= maxTextWidth) {
            current = std::move(candidate);
            ++wordIndex;
            continue;
        }

        if (current.empty()) {
            lines.push_back(EllipsizeTrackToastLine(hdc, word, maxTextWidth));
            ++wordIndex;
        }
        else {
            lines.push_back(current);
            current.clear();
        }
    }

    if (!current.empty() && lines.size() < kTrackToastMaxTextLines) {
        lines.push_back(current);
    }

    if (wordIndex < words.size() && !lines.empty()) {
        std::wstring tail = lines.back();
        for (size_t i = wordIndex; i < words.size(); ++i) {
            if (!tail.empty()) {
                tail += L" ";
            }
            tail += words[i];
        }
        lines.back() = EllipsizeTrackToastLine(hdc, tail, maxTextWidth);
    }

    if (lines.empty()) {
        lines.push_back(L"");
    }

    return lines;
}

static int GetTrackToastLineHeight(HDC hdc)
{
    TEXTMETRICW tm = {};
    if (GetTextMetricsW(hdc, &tm)) {
        return tm.tmHeight + tm.tmExternalLeading;
    }
    return 18;
}

static int CalculateTrackToastOverlayHeight(HDC hdc, int width, HFONT hFont)
{
    HFONT hOldFont = hFont ? (HFONT)SelectObject(hdc, hFont) : nullptr;

    const int lineHeight = GetTrackToastLineHeight(hdc);
    const auto lines = BuildTrackToastTextLines(hdc, width);
    const int textHeight = lineHeight * static_cast<int>(lines.size());

    if (hOldFont) {
        SelectObject(hdc, hOldFont);
    }

    return (std::max)(lineHeight + kTrackToastTextPaddingY * 2, textHeight + kTrackToastTextPaddingY * 2);
}

static HFONT CreateTrackToastTitleFont()
{
    LOGFONTW lf = {};
    lf.lfHeight = -15;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");

    return CreateFontIndirectW(&lf);
}

static void LayoutTrackToastText(HWND hWnd)
{
    if (!g_hTrackToastText) {
        return;
    }

    RECT rc = {};
    GetClientRect(hWnd, &rc);
    int overlayHeight = 22;
    HDC overlayDc = GetDC(hWnd);
    if (overlayDc) {
        HFONT hTitleFont = CreateTrackToastTitleFont();
        overlayHeight = CalculateTrackToastOverlayHeight(overlayDc, rc.right - rc.left, hTitleFont);
        if (hTitleFont) {
            DeleteObject(hTitleFont);
        }
        ReleaseDC(hWnd, overlayDc);
    }
    SetWindowPos(g_hTrackToastText, HWND_TOP, 0, 0, rc.right - rc.left, overlayHeight,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_hTrackToastText, nullptr, TRUE);
}

static void DrawTrackToastText(HWND hWnd, HDC hdc)
{
    RECT rc = {};
    GetClientRect(hWnd, &rc);

    HFONT hTitleFont = CreateTrackToastTitleFont();
    HFONT hOldFont = hTitleFont ? (HFONT)SelectObject(hdc, hTitleFont) : nullptr;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(245, 245, 245));

    const auto lines = BuildTrackToastTextLines(hdc, rc.right - rc.left);
    const int lineHeight = GetTrackToastLineHeight(hdc);
    int y = kTrackToastTextPaddingY;

    for (const std::wstring& line : lines) {
        RECT textRect = {
            kTrackToastTextPaddingX,
            y,
            rc.right - kTrackToastTextPaddingX,
            y + lineHeight
        };
        DrawTextW(hdc, line.c_str(), -1, &textRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += lineHeight;
    }

    if (hOldFont) {
        SelectObject(hdc, hOldFont);
    }
    if (hTitleFont) {
        DeleteObject(hTitleFont);
    }
}

static void DrawTrackToast(HWND hWnd, HDC hdc)
{
    RECT rc = {};
    GetClientRect(hWnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    int overlayHeight = 22;
    HDC overlayDc = GetDC(hWnd);
    if (overlayDc) {
        HFONT hTitleFont = CreateTrackToastTitleFont();
        overlayHeight = CalculateTrackToastOverlayHeight(overlayDc, width, hTitleFont);
        if (hTitleFont) {
            DeleteObject(hTitleFont);
        }
        ReleaseDC(hWnd, overlayDc);
    }

    if (EnsureTrackToastSdl(hWnd)) {
        SDL_SetRenderDrawColor(g_trackToastRenderer, 35, 38, 44, 255);
        SDL_RenderClear(g_trackToastRenderer);

        SDL_Surface* loadedSurface = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_coverFileMutex);
            loadedSurface = IMG_Load("cover_cache\\cover.jpg");
        }

        if (loadedSurface) {
            SDL_Texture* coverTexture = SDL_CreateTextureFromSurface(g_trackToastRenderer, loadedSurface);
            SDL_FreeSurface(loadedSurface);

            if (coverTexture) {
                SDL_Rect dst = { 0, 0, width, height };
                SDL_RenderCopy(g_trackToastRenderer, coverTexture, nullptr, &dst);
                SDL_DestroyTexture(coverTexture);
            }
        }

        SDL_SetRenderDrawBlendMode(g_trackToastRenderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_trackToastRenderer, 0, 0, 0, 190);
        SDL_Rect overlayRect = { 0, 0, width, overlayHeight };
        SDL_RenderFillRect(g_trackToastRenderer, &overlayRect);

        SDL_SetRenderDrawBlendMode(g_trackToastRenderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(g_trackToastRenderer, 190, 190, 190, 255);
        SDL_Rect borderRect = { 0, 0, width - 1, height - 1 };
        SDL_RenderDrawRect(g_trackToastRenderer, &borderRect);

        SDL_RenderPresent(g_trackToastRenderer);
    }
    else {
        HBRUSH fallbackBrush = CreateSolidBrush(RGB(35, 38, 44));
        FillRect(hdc, &rc, fallbackBrush);
        DeleteObject(fallbackBrush);

        RECT overlayRect = { 0, 0, width, overlayHeight };
        HBRUSH overlayBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &overlayRect, overlayBrush);
        DeleteObject(overlayBrush);

        HBRUSH borderBrush = CreateSolidBrush(RGB(190, 190, 190));
        FrameRect(hdc, &rc, borderBrush);
        DeleteObject(borderBrush);
    }

    LayoutTrackToastText(hWnd);
}

static LRESULT CALLBACK TrackToastTextProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        DrawTrackToastText(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        HWND hParent = GetParent(hWnd);
        if (hParent) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            MapWindowPoints(hWnd, hParent, &pt, 1);
            SendMessageW(hParent, WM_LBUTTONDOWN, wParam, MAKELPARAM(pt.x, pt.y));
            return 0;
        }
        break;
    }
    case WM_NCDESTROY:
        if (g_hTrackToastText == hWnd) {
            g_hTrackToastText = nullptr;
        }
        break;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static LRESULT CALLBACK TrackToastProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        DrawTrackToast(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wParam == IDT_TRACK_TOAST_HIDE) {
            KillTimer(hWnd, IDT_TRACK_TOAST_HIDE);
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        KillTimer(hWnd, IDT_TRACK_TOAST_HIDE);
        g_trackToastDragging = true;
        g_trackToastDragOffset.x = GET_X_LPARAM(lParam);
        g_trackToastDragOffset.y = GET_Y_LPARAM(lParam);
        SetCapture(hWnd);
        return 0;
    case WM_MOUSEMOVE:
        if (g_trackToastDragging) {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ClientToScreen(hWnd, &pt);
            int x = pt.x - g_trackToastDragOffset.x;
            int y = pt.y - g_trackToastDragOffset.y;
            SetWindowPos(hWnd, HWND_TOPMOST, x, y, 0, 0,
                SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g_trackToastDragging) {
            g_trackToastDragging = false;
            ReleaseCapture();

            RECT toastRc = {};
            GetWindowRect(hWnd, &toastRc);
            g_trackToastPositionSaved = true;
            g_trackToastX = toastRc.left;
            g_trackToastY = toastRc.top;

            SetTimer(hWnd, IDT_TRACK_TOAST_HIDE, 6500, nullptr);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        g_trackToastDragging = false;
        break;
    case WM_NCDESTROY:
        CleanupTrackToastSdl();
        if (g_hTrackToastText && GetParent(g_hTrackToastText) == hWnd) {
            g_hTrackToastText = nullptr;
        }
        if (g_hTrackToast == hWnd) {
            g_hTrackToast = nullptr;
        }
        break;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void RegisterTrackToastClass()
{
    static bool registered = false;
    static bool textRegistered = false;
    if (registered && textRegistered) {
        return;
    }

    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    HCURSOR hCursor = LoadCursor(nullptr, IDC_ARROW);

    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = TrackToastProc;
        wc.hInstance = hInstance;
        wc.hCursor = hCursor;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = TRACK_TOAST_CLASS;

        if (RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            registered = true;
        }
    }

    if (!textRegistered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = TrackToastTextProc;
        wc.hInstance = hInstance;
        wc.hCursor = hCursor;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = TRACK_TOAST_TEXT_CLASS;

        if (RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            textRegistered = true;
        }
    }
}

static void ShowTrackToastIfNeeded(HWND hOwner)
{
    if (!g_showTrackToastInTray || !g_isInTray) {
        return;
    }

    g_trackToastTitle = GetNowPlayingTitleText();
    if (g_trackToastTitle.empty() || g_trackToastTitle == Tr("nowplaying.no_data", L"Нет данных о треке")) {
        return;
    }

    RegisterTrackToastClass();

    if (!g_hTrackToast) {
        g_hTrackToast = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            TRACK_TOAST_CLASS,
            L"",
            WS_POPUP,
            CW_USEDEFAULT, CW_USEDEFAULT,
            kTrackToastSize, kTrackToastSize,
            hOwner,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
    }

    if (!g_hTrackToast) {
        return;
    }

    if (!g_hTrackToastText) {
        g_hTrackToastText = CreateWindowExW(
            WS_EX_TRANSPARENT,
            TRACK_TOAST_TEXT_CLASS,
            L"",
            WS_CHILD | WS_VISIBLE,
            0, 0,
            kTrackToastSize, 24,
            g_hTrackToast,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
    }

    CleanupTrackToastSdl();

    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    HMONITOR hMon = MonitorFromWindow(hOwner, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(hMon, &mi)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
    }

    int x = mi.rcWork.right - kTrackToastSize - kTrackToastMargin;
    int y = mi.rcWork.bottom - kTrackToastSize - kTrackToastMargin;
    if (g_trackToastPositionSaved) {
        x = g_trackToastX;
        y = g_trackToastY;
    }

    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    if (x + kTrackToastSize > mi.rcWork.right) x = mi.rcWork.right - kTrackToastSize;
    if (y + kTrackToastSize > mi.rcWork.bottom) y = mi.rcWork.bottom - kTrackToastSize;

    SetWindowPos(g_hTrackToast, HWND_TOPMOST, x, y, kTrackToastSize, kTrackToastSize,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    LayoutTrackToastText(g_hTrackToast);
    InvalidateRect(g_hTrackToast, nullptr, TRUE);
    UpdateWindow(g_hTrackToast);
    if (g_hTrackToastText) {
        InvalidateRect(g_hTrackToastText, nullptr, TRUE);
        UpdateWindow(g_hTrackToastText);
    }
    SetTimer(g_hTrackToast, IDT_TRACK_TOAST_HIDE, 6500, nullptr);
}

static bool IsTransientPlaybackStatus(const std::wstring& status)
{
    if (status.empty() || status == TrString("status.stopped", L"Остановлено")) {
        return false;
    }

    if (status.rfind(L"\u25B7", 0) == 0) {
        return false;
    }

    static const wchar_t* tokens[] = {
        L"FFmpeg",
        L"Подключение",
        L"Чтение",
        L"Анализ",
        L"Используемый",
        L"Ошибка",
        L"Поток",
        L"таймаут",
        L"Переподключение",
        L"Попытка",
        L"Аудиоустройство",
        L"Пропускаем",
        L"Connecting",
        L"Reading",
        L"Analyzing",
        L"Active",
        L"Error",
        L"Failed",
        L"Stream",
        L"timeout",
        L"Reconnecting",
        L"Attempt",
        L"Audio device",
        L"Skipping"
    };

    for (const wchar_t* token : tokens) {
        if (status.find(token) != std::wstring::npos) {
            return true;
        }
    }

    return false;
}

static std::wstring GetNowPlayingBarText()
{
    const bool appendBadServerDataStatus =
        g_nowPlayingStatus == TrString("status.skipping_bad_server_data", L"Пропускаем битые данные от сервера...") ||
        g_nowPlayingStatus == L"Пропускаем битые данные от сервера..." ||
        g_nowPlayingStatus == L"Skipping corrupted data from server...";
    const bool showStreamInfo =
        !appendBadServerDataStatus &&
        g_audioStreamInfoAllowed.load() &&
        !g_nowPlayingStreamInfo.empty();
    const bool showLimiterStatus =
        g_enableLimiterGainRider && running.load() && !g_limiterRiderStatus.empty();
    const bool showLufsStatus =
        g_enableLufsGainNormalizer && running.load() && !g_lufsNormalizerStatus.empty();

    if (!appendBadServerDataStatus && IsTransientPlaybackStatus(g_nowPlayingStatus)) {
        return g_nowPlayingStatus;
    }

    std::wstring barText;

    if ((showStreamInfo || appendBadServerDataStatus || showLimiterStatus || showLufsStatus) &&
        running.load() &&
        !g_nowPlayingElapsed.empty()) {
        barText = L"▷ ";
        barText += g_nowPlayingElapsed;
    }

    if (showLimiterStatus) {
        if (!barText.empty()) {
            barText += L"   ";
        }
        barText += g_limiterRiderStatus;
    }

    if (showLufsStatus) {
        if (!barText.empty()) {
            barText += L"   ";
        }
        barText += g_lufsNormalizerStatus;
    }

    if (appendBadServerDataStatus) {
        if (!barText.empty()) {
            barText += L"   ";
        }
        barText += g_nowPlayingStatus;
    }

    if (showStreamInfo) {
        if (!barText.empty()) {
            barText += L"   ";
        }
        barText += g_nowPlayingStreamInfo;
    }

    if (barText.empty()) {
        barText = g_nowPlayingStatus.empty() ? TrString("status.stopped", L"Остановлено") : g_nowPlayingStatus;
    }

    return barText;
}

static std::string TrimAsciiCopy(std::string value)
{
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };

    value.erase(value.begin(),
        std::find_if(value.begin(), value.end(), notSpace));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), notSpace).base(),
        value.end());

    return value;
}

static std::string ToLowerAsciiCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static bool IsInterestingFfmpegStatusLine(const std::string& lower)
{
    return lower.find("http") != std::string::npos ||
           lower.find("icy") != std::string::npos ||
           lower.find("metadata") != std::string::npos ||
           lower.find("reconnect") != std::string::npos ||
           lower.find("retry") != std::string::npos ||
           lower.find("timeout") != std::string::npos ||
           lower.find("timed out") != std::string::npos ||
           lower.find("error") != std::string::npos ||
           lower.find("failed") != std::string::npos ||
           lower.find("disconnect") != std::string::npos ||
           lower.find("network") != std::string::npos;
}

static bool IsNoisyFfmpegAdvisoryLine(const std::string& lower)
{
    return lower.find("if you want to help") != std::string::npos ||
           lower.find("upload a sample") != std::string::npos ||
           lower.find("sample of this file") != std::string::npos ||
           lower.find("ffmpeg-devel") != std::string::npos ||
           lower.find("ffmpeg.org/bugreports") != std::string::npos ||
           lower.find("is not implemented") != std::string::npos ||
           lower.find("update your ffmpeg version") != std::string::npos;
}

static bool IsNoisyFfmpegDecoderLine(const std::string& lower)
{
    return lower.find("header missing") != std::string::npos ||
           lower.find("big_values too big") != std::string::npos ||
           lower.find("error while decoding mpeg audio frame") != std::string::npos ||
           lower.find("invalid new backstep") != std::string::npos ||
           lower.find("invalid block type") != std::string::npos ||
           lower.find("switch point in") != std::string::npos;
}

static const char* FfmpegLogLevelName(int level)
{
    if (level <= AV_LOG_PANIC) return "panic";
    if (level <= AV_LOG_FATAL) return "fatal";
    if (level <= AV_LOG_ERROR) return "error";
    if (level <= AV_LOG_WARNING) return "warning";
    if (level <= AV_LOG_INFO) return "info";
    return "debug";
}

static HWND CreateTooltipWindow(HWND hParent)
{
    static const wchar_t kTooltipProp[] = L"IRPFFmpegTooltipWindow";
    if (HWND hOldTooltip = reinterpret_cast<HWND>(GetPropW(hParent, kTooltipProp))) {
        DestroyWindow(hOldTooltip);
        RemovePropW(hParent, kTooltipProp);
    }

    HWND hTooltip = CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASSW,
        nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        hParent,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);

    if (hTooltip) {
        SetWindowPos(hTooltip, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SendMessageW(hTooltip, TTM_SETMAXTIPWIDTH, 0, 360);
        SendMessageW(hTooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 450);
        SendMessageW(hTooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 8000);
        SetPropW(hParent, kTooltipProp, reinterpret_cast<HANDLE>(hTooltip));
    }

    return hTooltip;
}

static void AddTooltip(HWND hTooltip, HWND hParent, HWND hControl, const wchar_t* text)
{
    if (!hTooltip || !hParent || !hControl || !text || !*text) {
        return;
    }

    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = hParent;
    ti.uId = reinterpret_cast<UINT_PTR>(hControl);
    ti.lpszText = const_cast<LPWSTR>(text);

    SendMessageW(hTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

static void SetupMainDialogTooltips(HWND hDlg);

static void PopulateLanguageCombo(HWND hDlg)
{
    HWND hCombo = GetDlgItem(hDlg, IDC_COMBO_LANGUAGE);
    if (!hCombo) {
        return;
    }

    LoadAvailableLanguages();
    const auto& languageOptions = GetAvailableLanguages();
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);

    int selectedIndex = 0;
    for (size_t i = 0; i < languageOptions.size(); ++i) {
        int idx = static_cast<int>(SendMessageW(hCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(languageOptions[i].displayName.c_str())));
        SendMessageW(hCombo, CB_SETITEMDATA, idx, static_cast<LPARAM>(i));
        if (languageOptions[i].id == g_languageId) {
            selectedIndex = idx;
        }
    }

    if (!languageOptions.empty()) {
        SendMessageW(hCombo, CB_SETCURSEL, selectedIndex, 0);
    }
}

static void ApplySettingsDialogLanguage(HWND hDlg)
{
    SetWindowTextW(hDlg, Tr("settings.title", L"Настройки"));
    SetDlgItemTextW(hDlg, IDC_SETTINGWND_TITLE, Tr("settings.title", L"Настройки"));
    SetDlgItemTextW(hDlg, IDOK, Tr("settings.ok", L"OK"));
    SetDlgItemTextW(hDlg, IDC_GROUP_RECORDING, Tr("settings.group.recording", L" Режимы записи "));
    SetDlgItemTextW(hDlg, IDC_GROUP_EFFECTS, Tr("settings.group.effects", L" Эффекты воспроизведения "));
    SetDlgItemTextW(hDlg, IDC_GROUP_PROGRAM, Tr("settings.group.program", L" Настройки программы "));
    SetDlgItemTextW(hDlg, IDC_STATIC_LANGUAGE, Tr("settings.language", L"Язык:"));
    SetDlgItemTextW(hDlg, IDC_CHECK_MP3, Tr("settings.recording.mp3", L" MP3, LAME, 320 kbit/sec"));
    SetDlgItemTextW(hDlg, IDC_CHECK_FLAC, Tr("settings.recording.flac", L" FLAC, s16, ~1000 kbit/sec"));
    SetDlgItemTextW(hDlg, IDC_CHECK_STEREO_WIDTH, Tr("settings.effect.stereo_width", L" Расширение Стерео"));
    SetDlgItemTextW(hDlg, IDC_CHECK_EXCITER, Tr("settings.effect.exciter", L" Exciter / Яркость"));
    SetDlgItemTextW(hDlg, IDC_CHECK_DEEP_BASS, Tr("settings.effect.deep_bass", L" DeepBass / Глубокий Бас"));
    SetDlgItemTextW(hDlg, IDC_CHECK_DYNAMIC_AUTO_VOLUME, Tr("settings.effect.dynamic_auto_volume", L" Динамическая Регулировка Усиления"));
    SetDlgItemTextW(hDlg, IDC_CHECK_LUFS_GAIN_NORMALIZER, Tr("settings.effect.lufs_gain_normalizer", L" LUFS-нормализация станций"));
    SetDlgItemTextW(hDlg, IDC_CHECK_LIMITER_GAIN_RIDER, Tr("settings.effect.gain_rider", L" GainRider / Контроль Пиков"));
    SetDlgItemTextW(hDlg, IDC_CHECK_MINIMIZE_TO_TRAY, Tr("settings.program.minimize_to_tray", L" При минимизации отправлять в трей"));
    SetDlgItemTextW(hDlg, IDC_CHECK_SHOW_TRACK_TOAST, Tr("settings.program.show_track_toast", L" В трее показывать обложку при смене трека"));
}

static void ApplyMainDialogLanguage(HWND hDlg)
{
    if (hDlg) {
        SetWindowTextW(hDlg, L"IRP+ffmpeG");
        if (g_nowPlayingStatus == L"Остановлено" || g_nowPlayingStatus == L"Stopped") {
            g_nowPlayingStatus = TrString("status.stopped", L"Остановлено");
            InvalidateNowPlayingBar(hDlg);
        }
        SetupMainDialogTooltips(hDlg);
    }
}

static void SetupMainDialogTooltips(HWND hDlg)
{
    HWND hTooltip = CreateTooltipWindow(hDlg);
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_BUTTON_PP), Tr("tooltip.play_stop", L"Воспроизвести или остановить текущую станцию"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_BUTTON_REV), Tr("tooltip.prev_station", L"Перейти к предыдущей станции в списке"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_BUTTON_FORV), Tr("tooltip.next_station", L"Перейти к следующей станции в списке"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_BUTTON_PREVIOUS_STATION), Tr("tooltip.previous_station", L"Вернуться к ранее звучавшей станции"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_BUTTON_REC), Tr("tooltip.record", L"Начать или остановить запись текущего потока"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_ST_SETTING), Tr("tooltip.settings", L"Открыть настройки программы"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_SLIDER_BASS), Tr("tooltip.slider.bass", L"Низкие Частоты"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_SLIDER_HI), Tr("tooltip.slider.treble", L"Высокие Частоты"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_SLIDER_VOL), Tr("tooltip.slider.volume", L"Громкость"));
}

static void SetupSettingsDialogTooltips(HWND hDlg)
{
    HWND hTooltip = CreateTooltipWindow(hDlg);
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_CHECK_MP3), Tr("tooltip.recording.mp3", L"Записывать поток в MP3 320 kbit/sec"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_CHECK_FLAC), Tr("tooltip.recording.flac", L"Записывать поток в FLAC без потерь"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_CHECK_STEREO_WIDTH), Tr("tooltip.effect.stereo_width", L"Включить расширение стереобазы"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_CHECK_EXCITER), Tr("tooltip.effect.exciter", L"Добавить яркость и выразительность верхним частотам"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_CHECK_DEEP_BASS), Tr("tooltip.effect.deep_bass", L"Усилить глубину низких частот"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_CHECK_DYNAMIC_AUTO_VOLUME), Tr("tooltip.effect.dynamic_auto_volume", L"Автоматически выравнивать громкость воспроизведения"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_CHECK_LUFS_GAIN_NORMALIZER), Tr("tooltip.effect.lufs_gain_normalizer", L"Медленно приводить уровень разных станций к эталону -8.10 LUFS"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_CHECK_LIMITER_GAIN_RIDER), Tr("tooltip.effect.gain_rider", L"Контролировать пики и удерживать комфортный уровень сигнала"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_CHECK_MINIMIZE_TO_TRAY), Tr("tooltip.program.minimize_to_tray", L"При нажатии кнопки свернуть прятать программу в трей"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDC_CHECK_SHOW_TRACK_TOAST), Tr("tooltip.program.show_track_toast", L"Когда программа в трее, показывать обложку при смене трека"));
    AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDOK), Tr("tooltip.settings.ok", L"Сохранить настройки и закрыть окно"));
}

static std::wstring TranslateFfmpegStatusLine(const std::string& line)
{
    const std::string lower = ToLowerAsciiCopy(line);

    if (lower.find("http/1.1 200 ok") != std::string::npos) {
        return TrString("status.http_ok", L"Чтение заголовков (HTTP/1.1 200 OK)");
    }
    if (lower.find("reconnect") != std::string::npos || lower.find("retry") != std::string::npos) {
        return TrString("status.ffmpeg_reconnect", L"FFmpeg: переподключение к потоку...");
    }
    if (lower.find("timed out") != std::string::npos || lower.find("timeout") != std::string::npos) {
        return TrString("status.ffmpeg_timeout", L"FFmpeg: таймаут сети / ожидание переподключения");
    }
    if (lower.find("metadata") != std::string::npos || lower.find("icy") != std::string::npos) {
        return TrString("status.ffmpeg_icy_metadata", L"FFmpeg: ICY / metadata");
    }
    if (lower.find("http") != std::string::npos && lower.find("request") != std::string::npos) {
        return TrString("status.http_request", L"Подключение к URL (HTTP request...)");
    }
    if (lower.find("error") != std::string::npos || lower.find("failed") != std::string::npos) {
        return TrString("status.ffmpeg_prefix", L"FFmpeg: ") + utf8_to_wstring(line);
    }

    return TrString("status.ffmpeg_prefix", L"FFmpeg: ") + utf8_to_wstring(line);
}

static void FfmpegLogCallback(void* avcl, int level, const char* fmt, va_list vl)
{
    UNREFERENCED_PARAMETER(avcl);

    if (!running.load() && !g_playbackThreadRunning.load()) {
        return;
    }
    if (level > AV_LOG_INFO) {
        return;
    }

    char line[1024] = {};
    int printPrefix = 1;
    av_log_format_line(avcl, level, fmt, vl, line, sizeof(line), &printPrefix);

    std::string text = TrimAsciiCopy(line);
    if (text.empty()) {
        return;
    }

    std::string lower = ToLowerAsciiCopy(text);
    if (IsNoisyFfmpegAdvisoryLine(lower)) {
        return;
    }
    if (g_suppressFfmpegDecoderLog.load() && IsNoisyFfmpegDecoderLine(lower)) {
        return;
    }

    if (level <= AV_LOG_WARNING) {
        std::lock_guard<std::mutex> lock(g_ffmpegStatusMutex);
        if (text != g_lastFfmpegLogRaw) {
            g_lastFfmpegLogRaw = text;
            LogToUI(std::string("FFmpeg[") + FfmpegLogLevelName(level) + "]: " + text);
        }
    }

    if (!IsInterestingFfmpegStatusLine(lower)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_ffmpegStatusMutex);
        if (text == g_lastFfmpegStatusRaw) {
            return;
        }
        g_lastFfmpegStatusRaw = text;
    }

    PostFfmpegStatus(TranslateFfmpegStatusLine(text));
}

// Global callback for EnumChildWindows to set font
extern "C" BOOL CALLBACK SetChildFontProc(HWND hChild, LPARAM lParam) {
    SendMessage(hChild, WM_SETFONT, (WPARAM)lParam, TRUE);
    return TRUE;
}

// Function prototypes
void StartPlaybackThread(const char* url);
void StopPlayback(bool resetDisplayedBitrate);
static void StopPlaybackAsync();
void UpdatePlayingIndicator(int oldIndex, int newIndex);
void PlayAtIndex(int index, bool resetReconnect = true);
void StopPlaylistNameResolveThread();
void StartPlaylistNameResolveThread();
//для управления таймером метаданных
void StartMetadataTimer();
void StopMetadataTimer();

// Прототип функции обработки сообщений диалога
INT_PTR CALLBACK DialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK AboutDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

void UpdatePlayingIndicator(int oldIndex, int newIndex) {
    HWND hListView = GetDlgItem(g_hMainWnd, IDC_LIST_URL);
    const std::wstring playIcon = L"▶ ";

    // Remove icon from old item
    if (oldIndex != -1) {
        wchar_t text[256] = { 0 };
        ListView_GetItemText(hListView, oldIndex, 0, text, 256);
        text[_countof(text) - 1] = L'\0';
        std::wstring currentText(text, _countof(text) - 1);
        if (currentText.rfind(playIcon, 0) == 0) { // check if starts with icon
            std::wstring newText = currentText.substr(playIcon.length());
            ListView_SetItemText(hListView, oldIndex, 0, (LPWSTR)newText.c_str());
        }
    }

    // Add icon to new item
    if (newIndex != -1) {
        wchar_t text[256] = { 0 };   
        ListView_GetItemText(hListView, newIndex, 0, text, 256);
        text[_countof(text) - 1] = L'\0';
        std::wstring currentText(text, _countof(text) - 1);
        // check if icon is already there
        if (currentText.rfind(playIcon, 0) != 0) {
            std::wstring newText = playIcon + text;
            ListView_SetItemText(hListView, newIndex, 0, (LPWSTR)newText.c_str());
            ListView_EnsureVisible(hListView, newIndex, FALSE);
        }
    }
}

void PlayAtIndex(int index, bool resetReconnect) {
    HWND hListView = GetDlgItem(g_hMainWnd, IDC_LIST_URL);
    if (!hListView) {
        return;
    }

    if (index < 0 || index >= static_cast<int>(playlist.size())) {
        return;
    }

    const std::wstring& url = playlist[index].url;
    if (url.empty()) {
        return;
    }

    const int oldPlayingIndex = g_currentlyPlayingIndex;
    if (oldPlayingIndex != index &&
        oldPlayingIndex >= 0 &&
        oldPlayingIndex < static_cast<int>(playlist.size())) {
        g_previousStationIndex = oldPlayingIndex;
    }

    UpdatePlayingIndicator(g_currentlyPlayingIndex, -1);

    if (resetReconnect) {
        reconnect_attempts = 0;
    }

    // Convert URL to char* for playback function
    int required = WideCharToMultiByte(CP_UTF8, 0, url.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required > 0) {
        char* utf8 = new char[required];
        WideCharToMultiByte(CP_UTF8, 0, url.c_str(), -1, utf8, required, nullptr, nullptr);
        
        StartPlaybackThread(utf8);
        
        delete[] utf8;
    }
    // Update UI
    UpdatePlayingIndicator(-1, index);
    g_currentlyPlayingIndex = index;
    ListView_SetItemState(hListView, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);

    int itemCount = ListView_GetItemCount(hListView);
    int topVisible = ListView_GetTopIndex(hListView);
    int visibleCount = ListView_GetCountPerPage(hListView);
    int bottomVisible = min(topVisible + visibleCount - 1, itemCount - 1);

    if (index >= bottomVisible && index + 1 < itemCount) {
        // Активная строка — последняя видимая снизу: показываем следующую
        ListView_EnsureVisible(hListView, index + 1, FALSE);
    }
    else if (index <= topVisible && index - 1 >= 0) {
        // Активная строка — первая видимая сверху: показываем предыдущую
        ListView_EnsureVisible(hListView, index - 1, FALSE);
    }
    else {
        ListView_EnsureVisible(hListView, index, FALSE);
    }
   
}

static LRESULT CALLBACK SettingStaticSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
    {
        // Если ещё не в состоянии "hot", включаем трекинг и отмечаем состояние
        if (!GetProp(hwnd, L"hot"))
        {
            SetProp(hwnd, L"hot", (HANDLE)1);
            TRACKMOUSEEVENT tme = { sizeof(tme) };
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        break;
    }
    case WM_MOUSELEAVE:
    {
        // Убираем "hot" и перерисовываем
        RemoveProp(hwnd, L"hot");
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    }
    case WM_LBUTTONDOWN:
    {
        // Перенаправляем клик родителю, чтобы сработал существующий обработчик IDC_ST_SETTING
        SetFocus(hwnd);
        return DefSubclassProc(hwnd, msg, wParam, lParam);
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        // Фон: подсвечиваем при hover
        bool isHot = (GetProp(hwnd, L"hot") != nullptr);
        //всегда перерисовываем фон
        HBRUSH hBrush = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
        FillRect(hdc, &rc, hBrush);
        DeleteObject(hBrush);

        if (hButtonFont)
        {
            HFONT hOld = (HFONT)SelectObject(hdc, hButtonFont);
            COLORREF iconColor = isHot ? RGB(70, 130, 220) : RGB(100, 100, 100);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, iconColor);

            if (isHot) {

                LOGFONT lf;
                GetObject(hButtonFont, sizeof(LOGFONT), &lf);
                lf.lfHeight = -MulDiv(22, GetDeviceCaps(hdc, LOGPIXELSY), 72); // Увеличиваем на 2 пункта
                HFONT hBigFont = CreateFontIndirect(&lf);
                SelectObject(hdc, hBigFont);
                DrawTextW(hdc, L"\ue713", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                DeleteObject(hBigFont);

            }
            else {

                DrawTextW(hdc, L"\ue713", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            SelectObject(hdc, hOld);
        }
        else
        {
            // fallback
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(100, 100, 100));
            DrawTextW(hdc, L"⚙", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
    {
        RemoveProp(hwnd, L"hot");
        break;
    }
    default:
        break;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void SetStationNameColumnWidth(HWND hListView)
{
    if (!hListView) return;

    ListView_SetColumnWidth(hListView, 0, LVSCW_AUTOSIZE);
    int width = ListView_GetColumnWidth(hListView, 0) + 56;
    if (width < 204) width = 204;
    if (width > 274) width = 274;
    ListView_SetColumnWidth(hListView, 0, width);
}

static HBITMAP CreateMenuGlyphBitmap(HWND hWnd, const wchar_t* glyph)
{
    if (!glyph || !hButtonFont) {
        return nullptr;
    }

    HDC hdc = GetDC(hWnd);
    if (!hdc) {
        return nullptr;
    }

    const int size = (std::max)(18, GetSystemMetrics(SM_CXMENUCHECK));
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP hBitmap = memDC ? CreateCompatibleBitmap(hdc, size, size) : nullptr;
    HGDIOBJ hOldBitmap = hBitmap ? SelectObject(memDC, hBitmap) : nullptr;

    if (hBitmap) {
        RECT rc = { 0, 0, size, size };
        HBRUSH hBack = CreateSolidBrush(GetSysColor(COLOR_MENU));
        FillRect(memDC, &rc, hBack);
        DeleteObject(hBack);

        LOGFONTW lf = {};
        HFONT hIconFont = hButtonFont;
        if (GetObjectW(hButtonFont, sizeof(lf), &lf)) {
            lf.lfHeight = -MulDiv(12, GetDeviceCaps(hdc, LOGPIXELSY), 72);
            HFONT hSmallFont = CreateFontIndirectW(&lf);
            if (hSmallFont) {
                hIconFont = hSmallFont;
            }
        }

        HFONT hOldFont = (HFONT)SelectObject(memDC, hIconFont);
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(35, 35, 35));
        DrawTextW(memDC, glyph, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(memDC, hOldFont);

        if (hIconFont != hButtonFont) {
            DeleteObject(hIconFont);
        }
    }

    if (hOldBitmap) {
        SelectObject(memDC, hOldBitmap);
    }
    if (memDC) {
        DeleteDC(memDC);
    }
    ReleaseDC(hWnd, hdc);

    return hBitmap;
}

static void SetMenuItemBitmapByCommand(HMENU hMenu, UINT commandId, HBITMAP hBitmap)
{
    if (!hMenu || !hBitmap) {
        return;
    }

    MENUITEMINFOW mii = {};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_BITMAP;
    mii.hbmpItem = hBitmap;
    SetMenuItemInfoW(hMenu, commandId, FALSE, &mii);
}

static void PaintRoundedRect(HDC hdc, const RECT& rc, int radius, COLORREF fill, COLORREF outline)
{
    HBRUSH hBrush = CreateSolidBrush(fill);
    HPEN hPen = CreatePen(PS_SOLID, 1, outline);
    HGDIOBJ oldBrush = SelectObject(hdc, hBrush);
    HGDIOBJ oldPen = SelectObject(hdc, hPen);

    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(hPen);
    DeleteObject(hBrush);
}

static void DrawStyledSliderChannel(LPNMCUSTOMDRAW lpcd)
{
    HWND hTrackBar = lpcd->hdr.hwndFrom;
    HDC hdc = lpcd->hdc;

    RECT rcClient = {};
    GetClientRect(hTrackBar, &rcClient);

    HBRUSH hBackBrush = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
    FillRect(hdc, &rcClient, hBackBrush);
    DeleteObject(hBackBrush);

    RECT buttonRect = rcClient;
    InflateRect(&buttonRect, -1, -1);

    RECT shadowRect = buttonRect;
    OffsetRect(&shadowRect, 0, 1);
    PaintRoundedRect(hdc, shadowRect, 10, RGB(226, 230, 235), RGB(226, 230, 235));
    PaintRoundedRect(hdc, buttonRect, 10, RGB(247, 248, 250), RGB(218, 224, 232));

    const int buttonHeight = static_cast<int>(buttonRect.bottom - buttonRect.top);
    const int trackHeight = (std::max)(2, buttonHeight / 4 - 2);
    const int trackCenterY = buttonRect.top + (buttonRect.bottom - buttonRect.top) / 2 + 4;
    RECT trackRect = {
        buttonRect.left + 6,
        trackCenterY - trackHeight / 2,
        buttonRect.right - 6,
        trackCenterY + (trackHeight + 1) / 2
    };

    PaintRoundedRect(hdc, trackRect, 4, RGB(223, 227, 233), RGB(205, 211, 219));

    int minVal = (int)SendMessage(hTrackBar, TBM_GETRANGEMIN, 0, 0);
    int maxVal = (int)SendMessage(hTrackBar, TBM_GETRANGEMAX, 0, 0);
    int curVal = (int)SendMessage(hTrackBar, TBM_GETPOS, 0, 0);
    if (maxVal <= minVal) {
        return;
    }

    const int trackWidth = trackRect.right - trackRect.left;
    const COLORREF accent = RGB(120, 205, 240);

    if (minVal < 0 && maxVal > 0) {
        int zeroX = trackRect.left + MulDiv(0 - minVal, trackWidth, maxVal - minVal);
        int curX = trackRect.left + MulDiv(curVal - minVal, trackWidth, maxVal - minVal);

        RECT centerLine = { zeroX - 1, trackRect.top + 1, zeroX + 1, trackRect.bottom - 1 };
        HBRUSH hCenterBrush = CreateSolidBrush(RGB(145, 153, 165));
        FillRect(hdc, &centerLine, hCenterBrush);
        DeleteObject(hCenterBrush);

        if (curX != zeroX) {
            RECT progressRect = trackRect;
            progressRect.left = (std::min)(zeroX, curX);
            progressRect.right = (std::max)(zeroX, curX);
            if (progressRect.right - progressRect.left < 2) {
                progressRect.right = progressRect.left + 2;
            }

            PaintRoundedRect(hdc, progressRect, 4, accent, accent);
        }
    }
    else if (curVal > minVal) {
        int activeWidth = MulDiv(curVal - minVal, trackWidth, maxVal - minVal);
        RECT progressRect = trackRect;
        progressRect.right = trackRect.left + activeWidth;

        if (progressRect.right - progressRect.left >= 2) {
            PaintRoundedRect(hdc, progressRect, 4, accent, accent);
        }
    }
}

static void FillTrayIconData(HWND hWnd, NOTIFYICONDATAW& nid)
{
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hWnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY_ICON;
    nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_IRPFFMPEG));
    wcscpy_s(nid.szTip, L"IRP+ffmpeG");
}

static void AddTrayIcon(HWND hWnd)
{
    NOTIFYICONDATAW nid = {};
    FillTrayIconData(hWnd, nid);

    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        g_trayIconAdded = true;
    }
}

static void RemoveTrayIcon(HWND hWnd)
{
    if (!g_trayIconAdded) {
        return;
    }

    NOTIFYICONDATAW nid = {};
    FillTrayIconData(hWnd, nid);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayIconAdded = false;
}

static void ShowTrayBalloon(HWND hWnd)
{
    if (!g_trayIconAdded) {
        AddTrayIcon(hWnd);
    }

    NOTIFYICONDATAW nid = {};
    FillTrayIconData(hWnd, nid);
    nid.uFlags |= NIF_INFO;
    wcscpy_s(nid.szInfoTitle, Tr("tray.balloon.title", L"IRP+ffmpeG работает в трее"));
    wcscpy_s(nid.szInfo, Tr("tray.balloon.text", L"Дважды щелкните значок, чтобы вернуть окно. Для выхода используйте меню трея."));
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void RestoreMainWindow(HWND hWnd)
{
    if (!hWnd) {
        return;
    }
    g_restoringFromTray = true;
    if (g_isInTray) {
        RemoveTrayIcon(hWnd);
        g_isInTray = false;
    }
    if (g_hTrackToast) {
        ShowWindow(g_hTrackToast, SW_HIDE);
    }
    ShowWindow(hWnd, SW_RESTORE);
    ShowWindow(hWnd, SW_SHOWNORMAL);
    SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hWnd);
    SetActiveWindow(hWnd);
    SetForegroundWindow(hWnd);

    g_restoringFromTray = false;
}

static void RestoreMainWindowFromTaskbar(HWND hWnd)
{
    if (!hWnd) {
        return;
    }
    g_minimizeToTrayFromCaptionButton = false;
    if (g_isInTray) {
        RestoreMainWindow(hWnd);
        return;
    }

    ShowWindow(hWnd, SW_RESTORE);
    ShowWindow(hWnd, SW_SHOW);
    BringWindowToTop(hWnd);
    SetForegroundWindow(hWnd);
}

static void HideMainWindowToTray(HWND hWnd)
{
    if(!hWnd) {
        return;
    }

    if (IsWindow(hWnd)) {
        SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }

    if (!g_trayIconAdded) {
        AddTrayIcon(hWnd);
    }

    ShowWindow(hWnd, SW_HIDE);

    g_isInTray = true;

    if (!g_trayHideBalloonShown) {
        ShowTrayBalloon(hWnd);
        g_trayHideBalloonShown = true;
    }
}

static void RequestApplicationExit(HWND hWnd)
{
    g_isReallyExiting = true;
    g_isInTray = false;
    RemoveTrayIcon(hWnd);
    if (g_hTrackToast) {
        DestroyWindow(g_hTrackToast);
        g_hTrackToast = nullptr;
    }
    SendMessageW(hWnd, WM_DESTROY, 0, 0);
}

static void ShowTrayContextMenu(HWND hWnd)
{
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) {
        return;
    }

    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_RESTORE, Tr("tray.restore", L"Открыть IRP+ffmpeG"));
    HBITMAP hOpenIcon = CreateMenuGlyphBitmap(hWnd, L"\uE8A7");
    SetMenuItemBitmapByCommand(hMenu, IDM_TRAY_RESTORE, hOpenIcon);

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(hMenu, MF_STRING, IDC_BUTTON_PP, running.load() ? Tr("tray.stop", L"Остановить") : Tr("tray.play", L"Воспроизвести"));
    HBITMAP hPlayIcon = CreateMenuGlyphBitmap(hWnd, running.load() ? L"\uE769" : L"\uE768");
    SetMenuItemBitmapByCommand(hMenu, IDC_BUTTON_PP, hPlayIcon);

    AppendMenuW(hMenu, MF_STRING, IDC_BUTTON_REV, Tr("tray.prev", L"Предыдущая станция"));
    HBITMAP hPrevIcon = CreateMenuGlyphBitmap(hWnd, L"\uE892");
    SetMenuItemBitmapByCommand(hMenu, IDC_BUTTON_REV, hPrevIcon);

    AppendMenuW(hMenu, MF_STRING, IDC_BUTTON_FORV, Tr("tray.next", L"Следующая станция"));
    HBITMAP hNextIcon = CreateMenuGlyphBitmap(hWnd, L"\uE893");
    SetMenuItemBitmapByCommand(hMenu, IDC_BUTTON_FORV, hNextIcon);

    AppendMenuW(hMenu, MF_STRING, IDC_BUTTON_PREVIOUS_STATION, Tr("tray.previous", L"Вернуться к прошлой станции"));
    HBITMAP hBackIcon = CreateMenuGlyphBitmap(hWnd, L"\uE72B");
    SetMenuItemBitmapByCommand(hMenu, IDC_BUTTON_PREVIOUS_STATION, hBackIcon);

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(hMenu, MF_STRING, IDC_ST_SETTING, Tr("tray.settings", L"Настройки"));
    HBITMAP hSettingsIcon = CreateMenuGlyphBitmap(hWnd, L"\uE713");
    SetMenuItemBitmapByCommand(hMenu, IDC_ST_SETTING, hSettingsIcon);

    AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, Tr("tray.about", L"О программе"));
    HBITMAP hAboutIcon = CreateMenuGlyphBitmap(hWnd, L"\uE946");
    SetMenuItemBitmapByCommand(hMenu, IDM_ABOUT, hAboutIcon);

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, Tr("tray.exit", L"Выход"));
    HBITMAP hExitIcon = CreateMenuGlyphBitmap(hWnd, L"\uE8BB");
    SetMenuItemBitmapByCommand(hMenu, IDM_EXIT, hExitIcon);

    SetMenuDefaultItem(hMenu, IDM_TRAY_RESTORE, FALSE);

    POINT pt = {};
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    UINT command = TrackPopupMenu(hMenu,
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RETURNCMD,
        pt.x, pt.y, 0, hWnd, nullptr);
    if (command != 0) {
        SendMessageW(hWnd, WM_COMMAND, MAKEWPARAM(command, 0), 0);
    }
    PostMessageW(hWnd, WM_NULL, 0, 0);

    DestroyMenu(hMenu);

    if (hOpenIcon) DeleteObject(hOpenIcon);
    if (hPlayIcon) DeleteObject(hPlayIcon);
    if (hPrevIcon) DeleteObject(hPrevIcon);
    if (hNextIcon) DeleteObject(hNextIcon);
    if (hBackIcon) DeleteObject(hBackIcon);
    if (hSettingsIcon) DeleteObject(hSettingsIcon);
    if (hAboutIcon) DeleteObject(hAboutIcon);
    if (hExitIcon) DeleteObject(hExitIcon);
}

INT_PTR CALLBACK AboutDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    switch (message)
    {
    case WM_INITDIALOG:
    {
        HWND hTooltip = CreateTooltipWindow(hDlg);
        AddTooltip(hTooltip, hDlg, GetDlgItem(hDlg, IDOK), Tr("tooltip.about.ok", L"Закрыть окно информации о программе"));
        return (INT_PTR)TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }

    return (INT_PTR)FALSE;
}

static std::wstring TrimWide(std::wstring value)
{
    value.erase(value.begin(),
        std::find_if(value.begin(), value.end(), [](wchar_t ch) { return !std::iswspace(ch); }));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), [](wchar_t ch) { return !std::iswspace(ch); }).base(),
        value.end());
    return value;
}

static std::wstring GetWindowTextString(HWND hWnd)
{
    const int len = GetWindowTextLengthW(hWnd);
    if (len <= 0) {
        return std::wstring();
    }

    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hWnd, &text[0], len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

static bool IsSupportedPlaylistUrlW(const std::wstring& url)
{
    if (url.empty()) return false;

    const size_t colon = url.find(L':');
    if (colon == std::wstring::npos || colon == 0) return false;

    std::wstring scheme = url.substr(0, colon);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });

    static const wchar_t* kSchemes[] = {
        L"http", L"https", L"mms", L"mmsh", L"mmst", L"rtsp", L"icy"
    };

    bool supported = false;
    for (const wchar_t* allowed : kSchemes) {
        if (scheme == allowed) {
            supported = true;
            break;
        }
    }
    if (!supported) return false;

    if (url.size() <= colon + 3) return false;
    if (url.compare(colon + 1, 2, L"//") != 0) return false;

    const std::wstring remainder = url.substr(colon + 3);
    if (remainder.empty()) return false;
    if (remainder[0] == L'/' || remainder[0] == L'.') return false;

    for (wchar_t ch : url) {
        if (std::iswcntrl(ch) || std::iswspace(ch)) {
            return false;
        }
    }

    return true;
}

static bool ValidateNewStationInput(HWND hDlg, HWND hNameEdit, HWND hUrlEdit, PlaylistItem& item)
{
    std::wstring name = TrimWide(GetWindowTextString(hNameEdit));
    std::wstring url = TrimWide(GetWindowTextString(hUrlEdit));

    if (name.empty()) {
        MessageBoxW(hDlg, Tr("add.msg.enter_name", L"Введите название станции."), Tr("add.title", L"Добавить станцию"), MB_OK | MB_ICONWARNING);
        SetFocus(hNameEdit);
        return false;
    }
    if (name.find_first_of(L"\r\n") != std::wstring::npos ||
        url.find_first_of(L"\r\n") != std::wstring::npos) {
        MessageBoxW(hDlg, Tr("add.msg.one_line", L"Название и URL должны быть записаны в одну строку."), Tr("add.title", L"Добавить станцию"), MB_OK | MB_ICONWARNING);
        return false;
    }
    if (url.empty()) {
        MessageBoxW(hDlg, Tr("add.msg.enter_url", L"Введите интернет адрес станции."), Tr("add.title", L"Добавить станцию"), MB_OK | MB_ICONWARNING);
        SetFocus(hUrlEdit);
        return false;
    }
    if (!IsSupportedPlaylistUrlW(url)) {
        MessageBoxW(hDlg,
            Tr("add.msg.invalid_supported_url", L"Некорректный URL. Поддерживаются адреса вида http://, https://, icy://, mms:// или rtsp:// без пробелов."),
            Tr("add.title", L"Добавить станцию"), MB_OK | MB_ICONWARNING);
        SetFocus(hUrlEdit);
        return false;
    }

    for (const PlaylistItem& existing : playlist) {
        if (TrimWide(existing.url) == url) {
            MessageBoxW(hDlg, Tr("add.msg.duplicate_url", L"Станция с таким URL уже есть в плейлисте."), Tr("add.title", L"Добавить станцию"), MB_OK | MB_ICONWARNING);
            SetFocus(hUrlEdit);
            return false;
        }
    }

    item.name = std::move(name);
    item.url = std::move(url);
    return true;
}

struct AddStationDialogState {
    bool accepted = false;
    bool done = false;
    PlaylistItem item;
    HWND hNameEdit = nullptr;
    HWND hUrlEdit = nullptr;
    HFONT hFont = nullptr;
    int fontPointSize = 10;
};

static constexpr int kAddStationDialogFontPt = 10;

static LRESULT CALLBACK AddStationDialogProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<AddStationDialogState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE:
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<AddStationDialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        HDC fontDc = GetDC(hWnd);
        const int dpiY = fontDc ? GetDeviceCaps(fontDc, LOGPIXELSY) : 96;
        if (fontDc) {
            ReleaseDC(hWnd, fontDc);
        }
        const int fontHeight = -MulDiv(state->fontPointSize, dpiY, 72);
        state->hFont = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT hFont = state->hFont ? state->hFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND hLabelName = CreateWindowExW(0, L"STATIC", Tr("add.name_label", L"Название станции:"),
            WS_CHILD | WS_VISIBLE, 16, 18, 128, 20, hWnd, nullptr, GetModuleHandle(NULL), nullptr);
        HWND hLabelUrl = CreateWindowExW(0, L"STATIC", Tr("add.url_label", L"URL - адрес:"),
            WS_CHILD | WS_VISIBLE, 16, 56, 128, 20, hWnd, nullptr, GetModuleHandle(NULL), nullptr);

        state->hNameEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            150, 16, 292, 24, hWnd, (HMENU)IDC_EDIT_ADD_STATION_NAME, GetModuleHandle(NULL), nullptr);
        state->hUrlEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            150, 54, 292, 24, hWnd, (HMENU)IDC_EDIT_ADD_STATION_URL, GetModuleHandle(NULL), nullptr);

        HWND hOk = CreateWindowExW(0, L"BUTTON", Tr("common.ok", L"OK"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            262, 104, 82, 28, hWnd, (HMENU)IDOK, GetModuleHandle(NULL), nullptr);
        HWND hCancel = CreateWindowExW(0, L"BUTTON", Tr("common.cancel", L"Отмена"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            360, 104, 82, 28, hWnd, (HMENU)IDCANCEL, GetModuleHandle(NULL), nullptr);

        HWND controls[] = { hLabelName, hLabelUrl, state->hNameEdit, state->hUrlEdit, hOk, hCancel };
        for (HWND hCtl : controls) {
            if (hCtl) SendMessageW(hCtl, WM_SETFONT, (WPARAM)hFont, TRUE);
        }
        HWND hTooltip = CreateTooltipWindow(hWnd);
        AddTooltip(hTooltip, hWnd, hOk, Tr("tooltip.add.ok", L"Добавить станцию в список"));
        AddTooltip(hTooltip, hWnd, hCancel, Tr("tooltip.add.cancel", L"Закрыть окно без добавления станции"));

        SendMessageW(state->hNameEdit, EM_LIMITTEXT, 256, 0);
        SendMessageW(state->hUrlEdit, EM_LIMITTEXT, 2048, 0);
        SetFocus(state->hNameEdit);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            if (state && ValidateNewStationInput(hWnd, state->hNameEdit, state->hUrlEdit, state->item)) {
                state->accepted = true;
                state->done = true;
                ShowWindow(hWnd, SW_HIDE);
            }
            return 0;
        case IDCANCEL:
            if (state) {
                state->done = true;
            }
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (state) {
            state->done = true;
        }
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        if (state && state->hFont) {
            DeleteObject(state->hFont);
            state->hFont = nullptr;
        }
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool ShowAddStationDialog(HWND hOwner, PlaylistItem& item)
{
    const wchar_t kClassName[] = L"IRPFFmpegAddStationDialog";
    static bool registered = false;

    HINSTANCE hInst = GetModuleHandle(NULL);
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = AddStationDialogProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClassName;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            MessageBoxW(hOwner, Tr("add.msg.create_failed", L"Не удалось создать форму добавления станции."), Tr("add.title", L"Добавить станцию"), MB_OK | MB_ICONERROR);
            return false;
        }
        registered = true;
    }

    RECT ownerRc = {};
    GetWindowRect(hOwner, &ownerRc);
    const int width = 480;
    const int height = 175;
    const int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - width) / 2;
    const int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - height) / 2;

    AddStationDialogState state;
    state.fontPointSize = kAddStationDialogFontPt;
    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kClassName,
        Tr("add.title", L"Добавить станцию"),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, width, height,
        hOwner,
        nullptr,
        hInst,
        &state);

    if (!hDlg) {
        MessageBoxW(hOwner, Tr("add.msg.open_failed", L"Не удалось открыть форму добавления станции."), Tr("add.title", L"Добавить станцию"), MB_OK | MB_ICONERROR);
        return false;
    }

    EnableWindow(hOwner, FALSE);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);

    MSG msg = {};
    while (!state.done && IsWindow(hDlg)) {
        BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            if (result == 0) {
                PostQuitMessage((int)msg.wParam);
            }
            break;
        }

        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(hOwner, TRUE);
    SetActiveWindow(hOwner);
    if (IsWindow(hDlg)) {
        DestroyWindow(hDlg);
    }

    if (state.accepted) {
        item = std::move(state.item);
        return true;
    }
    return false;
}

struct EditStationNameDialogState {
    bool accepted = false;
    bool done = false;
    std::wstring name;
    HWND hNameEdit = nullptr;
    HFONT hFont = nullptr;
    int fontPointSize = 10;
};

static bool ValidateStationNameInput(HWND hDlg, HWND hNameEdit, std::wstring& outName)
{
    std::wstring name = TrimWide(GetWindowTextString(hNameEdit));
    if (name.empty()) {
        MessageBoxW(hDlg, Tr("edit_station.msg.enter_name", L"Введите название станции."), Tr("edit_station.title", L"Изменить название станции"), MB_OK | MB_ICONWARNING);
        SetFocus(hNameEdit);
        return false;
    }
    if (name.find_first_of(L"\r\n") != std::wstring::npos) {
        MessageBoxW(hDlg, Tr("edit_station.msg.one_line", L"Название станции должно быть записано в одну строку."), Tr("edit_station.title", L"Изменить название станции"), MB_OK | MB_ICONWARNING);
        SetFocus(hNameEdit);
        return false;
    }
    if (name.size() > 128) {
        MessageBoxW(hDlg, Tr("edit_station.msg.too_long", L"Название станции не должно быть длиннее 128 символов."), Tr("edit_station.title", L"Изменить название станции"), MB_OK | MB_ICONWARNING);
        SetFocus(hNameEdit);
        return false;
    }

    outName = std::move(name);
    return true;
}

static LRESULT CALLBACK EditStationNameDialogProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<EditStationNameDialogState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE:
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<EditStationNameDialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        HDC fontDc = GetDC(hWnd);
        const int dpiY = fontDc ? GetDeviceCaps(fontDc, LOGPIXELSY) : 96;
        if (fontDc) {
            ReleaseDC(hWnd, fontDc);
        }
        const int fontHeight = -MulDiv(state->fontPointSize, dpiY, 72);
        state->hFont = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT hFont = state->hFont ? state->hFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND hLabelName = CreateWindowExW(0, L"STATIC", Tr("add.name_label", L"Название станции:"),
            WS_CHILD | WS_VISIBLE, 16, 22, 128, 20, hWnd, nullptr, GetModuleHandle(NULL), nullptr);

        state->hNameEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", state->name.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            150, 20, 292, 24, hWnd, (HMENU)IDC_EDIT_ADD_STATION_NAME, GetModuleHandle(NULL), nullptr);

        HWND hOk = CreateWindowExW(0, L"BUTTON", Tr("common.ok", L"OK"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            262, 92, 82, 28, hWnd, (HMENU)IDOK, GetModuleHandle(NULL), nullptr);
        HWND hCancel = CreateWindowExW(0, L"BUTTON", Tr("common.cancel", L"Отмена"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            360, 92, 82, 28, hWnd, (HMENU)IDCANCEL, GetModuleHandle(NULL), nullptr);

        HWND controls[] = { hLabelName, state->hNameEdit, hOk, hCancel };
        for (HWND hCtl : controls) {
            if (hCtl) SendMessageW(hCtl, WM_SETFONT, (WPARAM)hFont, TRUE);
        }
        HWND hTooltip = CreateTooltipWindow(hWnd);
        AddTooltip(hTooltip, hWnd, hOk, Tr("tooltip.add.ok", L"Добавить станцию в список"));
        AddTooltip(hTooltip, hWnd, hCancel, Tr("tooltip.add.cancel", L"Закрыть окно без добавления станции"));

        SendMessageW(state->hNameEdit, EM_LIMITTEXT, 128, 0);
        SendMessageW(state->hNameEdit, EM_SETSEL, 0, -1);
        SetFocus(state->hNameEdit);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            if (state && ValidateStationNameInput(hWnd, state->hNameEdit, state->name)) {
                state->accepted = true;
                state->done = true;
                ShowWindow(hWnd, SW_HIDE);
            }
            return 0;
        case IDCANCEL:
            if (state) {
                state->done = true;
            }
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (state) {
            state->done = true;
        }
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        if (state && state->hFont) {
            DeleteObject(state->hFont);
            state->hFont = nullptr;
        }
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool ShowEditStationNameDialog(HWND hOwner, std::wstring& name)
{
    const wchar_t kClassName[] = L"IRPFFmpegEditStationNameDialog";
    static bool registered = false;

    HINSTANCE hInst = GetModuleHandle(NULL);
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = EditStationNameDialogProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClassName;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            MessageBoxW(hOwner, Tr("edit_station.msg.create_failed", L"Не удалось создать форму изменения станции."), Tr("edit_station.title", L"Изменить название станции"), MB_OK | MB_ICONERROR);
            return false;
        }
        registered = true;
    }

    RECT ownerRc = {};
    GetWindowRect(hOwner, &ownerRc);
    const int width = 480;
    const int height = 165;
    const int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - width) / 2;
    const int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - height) / 2;

    EditStationNameDialogState state;
    state.fontPointSize = kAddStationDialogFontPt;
    state.name = name;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kClassName,
        Tr("edit_station.title", L"Изменить название станции"),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, width, height,
        hOwner,
        nullptr,
        hInst,
        &state);

    if (!hDlg) {
        MessageBoxW(hOwner, Tr("edit_station.msg.open_failed", L"Не удалось открыть форму изменения станции."), Tr("edit_station.title", L"Изменить название станции"), MB_OK | MB_ICONERROR);
        return false;
    }

    EnableWindow(hOwner, FALSE);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);

    MSG msg = {};
    while (!state.done && IsWindow(hDlg)) {
        BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            if (result == 0) {
                PostQuitMessage((int)msg.wParam);
            }
            break;
        }

        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(hOwner, TRUE);
    SetActiveWindow(hOwner);
    if (IsWindow(hDlg)) {
        DestroyWindow(hDlg);
    }

    if (state.accepted) {
        name = std::move(state.name);
        return true;
    }
    return false;
}

static bool AddStationToPlaylist(HWND hDlg)
{
    PlaylistItem item;
    if (!ShowAddStationDialog(hDlg, item)) {
        return false;
    }

    HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);
    if (!hListView) {
        return false;
    }

    std::vector<PlaylistItem> updatedPlaylist = playlist;
    updatedPlaylist.push_back(item);

    if (!SavePlaylistToM3U(L"playlist.m3u", updatedPlaylist)) {
        return false;
    }

    playlist.swap(updatedPlaylist);

    const int index = ListView_GetItemCount(hListView);
    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = index;
    std::wstring displayName = GetPlaylistDisplayName(index);
    lvi.pszText = const_cast<LPWSTR>(displayName.c_str());
    ListView_InsertItem(hListView, &lvi);
    ListView_SetItemText(hListView, index, 1, const_cast<LPWSTR>(playlist[index].url.c_str()));

    SetStationNameColumnWidth(hListView);
    ListView_SetColumnWidth(hListView, 1, LVSCW_AUTOSIZE);
    int cur = ListView_GetColumnWidth(hListView, 1);
    ListView_SetColumnWidth(hListView, 1, cur + 128);

    ListView_SetItemState(hListView, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetSelectionMark(hListView, index);
    ListView_EnsureVisible(hListView, index, FALSE);

    savePlaylistToDat(L"app.dat", playlist, g_currentlyPlayingIndex);
    return true;
}

static bool DeleteStationFromPlaylist(HWND hDlg, int index)
{
    if (index < 0 || index >= static_cast<int>(playlist.size())) {
        return false;
    }

    HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);
    if (!hListView) {
        return false;
    }

    const bool deletingCurrentStation = (index == g_currentlyPlayingIndex);
    if (deletingCurrentStation && running.load()) {
        if (g_is_recording.load()) {
            g_rec_semafor.store(false);
            // находится в file_recording.cpp
            StopRecording();

            HWND hRec = GetDlgItem(hDlg, IDC_BUTTON_REC);
            if (hRec) {
                InvalidateRect(hRec, NULL, TRUE);
                UpdateWindow(hRec);
            }
        }

        StopPlaybackAsync();
    }

    std::vector<PlaylistItem> updatedPlaylist = playlist;
    updatedPlaylist.erase(updatedPlaylist.begin() + index);

    if (!SavePlaylistToM3U(L"playlist.m3u", updatedPlaylist)) {
        return false;
    }

    if (index == g_currentlyPlayingIndex) {
        UpdatePlayingIndicator(index, -1);
        g_currentlyPlayingIndex = -1;
    }
    else if (index < g_currentlyPlayingIndex) {
        --g_currentlyPlayingIndex;
    }

    if (index == g_previousStationIndex) {
        g_previousStationIndex = -1;
    }
    else if (index < g_previousStationIndex) {
        --g_previousStationIndex;
    }

    playlist.swap(updatedPlaylist);
    ListView_DeleteItem(hListView, index);
    SetStationNameColumnWidth(hListView);

    const int count = ListView_GetItemCount(hListView);
    int selectIndex = -1;
    if (count > 0) {
        selectIndex = (index < count) ? index : (count - 1);
        ListView_SetItemState(hListView, selectIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetSelectionMark(hListView, selectIndex);
    }

    if (deletingCurrentStation && selectIndex >= 0) {
        PlayAtIndex(selectIndex);
    }

    savePlaylistToDat(L"app.dat", playlist, g_currentlyPlayingIndex);

    return true;
}

static bool SaveStationFromPlaylist(HWND hDlg, int index)
{
    if (index < 0 || index >= static_cast<int>(playlist.size())) {
        return false;
    }

    std::wstring stationName = GetPlaylistDisplayName(index);
    stationName = TrimWide(stationName);
    if (stationName.empty()) {
        return false;
    }

    PlaylistItem oldItem = playlist[index];
    playlist[index].name = stationName;
    playlist[index].name_icy.clear();
    playlist[index].disable_name_icy = true;

    if (!SavePlaylistToM3U(L"playlist.m3u", playlist)) {
        playlist[index] = std::move(oldItem);
        return false;
    }

    savePlaylistToDat(L"app.dat", playlist, g_currentlyPlayingIndex);

    HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);
    if (hListView) {
        std::wstring displayName = stationName;
        if (index == g_currentlyPlayingIndex) {
            displayName = L"▶ " + displayName;
        }
        ListView_SetItemText(hListView, index, 0, const_cast<LPWSTR>(displayName.c_str()));
        ListView_SetItemText(hListView, index, 1, const_cast<LPWSTR>(playlist[index].url.c_str()));
        SetStationNameColumnWidth(hListView);
    }

    InvalidateNowPlayingBar(hDlg);
    return true;
}

static bool EditStationNameInPlaylist(HWND hDlg, int index)
{
    if (index < 0 || index >= static_cast<int>(playlist.size())) {
        return false;
    }

    std::wstring stationName = GetPlaylistDisplayName(index);
    if (!ShowEditStationNameDialog(hDlg, stationName)) {
        return false;
    }

    PlaylistItem oldItem = playlist[index];
    playlist[index].name = stationName;
    playlist[index].name_icy.clear();
    playlist[index].disable_name_icy = true;

    if (!SavePlaylistToM3U(L"playlist.m3u", playlist)) {
        playlist[index] = std::move(oldItem);
        return false;
    }

    savePlaylistToDat(L"app.dat", playlist, g_currentlyPlayingIndex);

    HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);
    if (hListView) {
        std::wstring displayName = stationName;
        if (index == g_currentlyPlayingIndex) {
            displayName = L"▶ " + displayName;
        }
        ListView_SetItemText(hListView, index, 0, const_cast<LPWSTR>(displayName.c_str()));
        ListView_SetItemText(hListView, index, 1, const_cast<LPWSTR>(playlist[index].url.c_str()));
        SetStationNameColumnWidth(hListView);
    }

    InvalidateNowPlayingBar(hDlg);
    return true;
}

static void FillListViewFromPlaylist(HWND hListView)
{
    if (!hListView) return;

    // Отключаем перерисовку, чтобы избежать мерцания при вставке множества элементов
    SendMessage(hListView, WM_SETREDRAW, FALSE, 0);

    // Очистим и заполним
    ListView_DeleteAllItems(hListView);

    LVITEM lvi = {};
    lvi.mask = LVIF_TEXT;

    for (int i = 0; i < static_cast<int>(playlist.size()); ++i) {
        lvi.iItem = i;
        std::wstring displayName = GetPlaylistDisplayName(i);
        lvi.pszText = const_cast<LPWSTR>(displayName.c_str());
        ListView_InsertItem(hListView, &lvi);
        ListView_SetItemText(hListView, i, 1, const_cast<LPWSTR>(playlist[i].url.c_str()));
    }

    // Настроим ширину колонок
    SetStationNameColumnWidth(hListView);
    ListView_SetColumnWidth(hListView, 1, LVSCW_AUTOSIZE);
    int cur = ListView_GetColumnWidth(hListView, 1);
    ListView_SetColumnWidth(hListView, 1, cur + 128);

    // Включаем перерисовку и принудительно обновляем контрол
    SendMessage(hListView, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hListView, NULL, TRUE);
    UpdateWindow(hListView);
}

static bool IsPlaylistOutdatedAgainstM3U(const std::vector<PlaylistItem>& currentPlaylist)
{
    if (!PathFileExistsW(L"playlist.m3u")) {
        return false;
    }

    std::vector<PlaylistItem> m3uPlaylist;
    loadPlaylist(L"playlist.m3u", m3uPlaylist);

    if (m3uPlaylist.empty()) {
        return false;
    }
    if (currentPlaylist.size() != m3uPlaylist.size()) {
        return true;
    }

    for (size_t i = 0; i < currentPlaylist.size(); ++i) {
        if (currentPlaylist[i].url != m3uPlaylist[i].url) {
            return true;
        }
    }

    return false;
}

static void PreserveIcyNameDisableFlagsByUrl(const std::vector<PlaylistItem>& sourcePlaylist,
    std::vector<PlaylistItem>& targetPlaylist)
{
    for (PlaylistItem& target : targetPlaylist) {
        if (target.url.empty()) {
            continue;
        }
        for (const PlaylistItem& source : sourcePlaylist) {
            if (source.url == target.url) {
                target.disable_name_icy = source.disable_name_icy;
                break;
            }
        }
    }
}

void StopPlaylistNameResolveThread()
{
    g_stopPlaylistNameResolveThread.store(true);
    if (g_playlistNameResolveThread.joinable()) {
        g_playlistNameResolveThread.join();
    }
}

void StartPlaylistNameResolveThread()
{
    StopPlaylistNameResolveThread();
    
    std::vector<std::pair<int, std::wstring>> pending;
    pending.reserve(playlist.size());

    for (int i = 0; i < static_cast<int>(playlist.size()); ++i) {
        if (playlist[i].url.empty()) continue;
        if (playlist[i].name.empty() || playlist[i].name == playlist[i].url) {
            pending.emplace_back(i, playlist[i].url);
        }
    }

    if (pending.empty()) {
        return;
    }

    g_stopPlaylistNameResolveThread.store(false);
    const unsigned long generation = g_playlistNameResolveGeneration.fetch_add(1) + 1;

    g_playlistNameResolveThread = std::thread([pending = std::move(pending), generation]() {
        for (const auto& entry : pending) {
            if (g_stopPlaylistNameResolveThread.load()) {
                break;
            }

            std::wstring stationName = ResolveStationNameFromUrl(entry.second);
            if (g_stopPlaylistNameResolveThread.load()) {
                break;
            }
            if (stationName.empty()) {
                continue;
            }

            auto* payload = new PlaylistNameResolvedPayload();
            payload->generation = generation;
            payload->index = entry.first;
            payload->url = entry.second;
            payload->name = std::move(stationName);

            if (!g_hMainWnd || !PostMessageW(g_hMainWnd, WM_APP_PLAYLIST_NAME_RESOLVED, 0, (LPARAM)payload)) {
                delete payload;
                break;
            }
        }
    });
}

static void ReloadPlaylistFromM3U()
{
    StopPlaylistNameResolveThread();

    // Сохраняем URL текущей воспроизводимой станции (если есть)
    std::wstring savedUrl;
    int oldIndex = g_currentlyPlayingIndex;
    if (oldIndex != -1 && oldIndex < static_cast<int>(playlist.size())) {
        savedUrl = playlist[oldIndex].url;
    }

    std::wstring savedPreviousUrl;
    if (g_previousStationIndex != -1 &&
        g_previousStationIndex < static_cast<int>(playlist.size())) {
        savedPreviousUrl = playlist[g_previousStationIndex].url;
    }

    // Перезагружаем плейлист
    std::vector<PlaylistItem> previousPlaylist = playlist;
    std::vector<PlaylistItem> tmp;
    loadPlaylist(L"playlist.m3u", tmp);
    PreserveIcyNameDisableFlagsByUrl(previousPlaylist, tmp);
    playlist = std::move(tmp);

    if (g_hMainWnd) {
        HWND hListView = GetDlgItem(g_hMainWnd, IDC_LIST_URL);
        if (hListView) {
            FillListViewFromPlaylist(hListView);

            // После перезагрузки пытаемся найти станцию с тем же URL
            int foundIndex = -1;
            if (!savedUrl.empty()) {
                for (int i = 0; i < static_cast<int>(playlist.size()); ++i) {
                    if (playlist[i].url == savedUrl) {
                        foundIndex = i;
                        break;
                    }
                }
            }

            int foundPreviousIndex = -1;
            if (!savedPreviousUrl.empty()) {
                for (int i = 0; i < static_cast<int>(playlist.size()); ++i) {
                    if (playlist[i].url == savedPreviousUrl) {
                        foundPreviousIndex = i;
                        break;
                    }
                }
            }

            // Перед удалением/установкой индикатора убедимся, что старый индекс валидный в новом списке
            int prevIndexToRemove = (oldIndex != -1 && oldIndex < ListView_GetItemCount(hListView)) ? oldIndex : -1;

            if (foundIndex != -1) {
                UpdatePlayingIndicator(prevIndexToRemove, foundIndex);
                g_currentlyPlayingIndex = foundIndex;
            }
            else {
                UpdatePlayingIndicator(prevIndexToRemove, -1);
                g_currentlyPlayingIndex = -1;
            }

            g_previousStationIndex = (foundPreviousIndex != foundIndex) ? foundPreviousIndex : -1;

            StartPlaylistNameResolveThread();
        }
    }
}

static void SyncStereoWidthPercentFromDialog(HWND hDlg, bool normalizeText)
{
    BOOL translated = FALSE;
    UINT rawValue = GetDlgItemInt(hDlg, IDC_EDIT_STEREO_WIDTH, &translated, FALSE);

    if (translated) {
        g_stereoWidthPercent = (std::max)(0, (std::min)(100, static_cast<int>(rawValue)));
    }

    if (normalizeText) {
        SetDlgItemInt(hDlg, IDC_EDIT_STEREO_WIDTH, static_cast<UINT>(g_stereoWidthPercent), FALSE);
    }
}

INT_PTR CALLBACK SettingsDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    switch (message)
    {
    case WM_INITDIALOG:
    {
        HWND hParent = GetParent(hDlg);

        if (hParent != NULL)
        {
            // Получаем размеры родительского окна
            RECT rcParent, rcDlg;
            GetWindowRect(hParent, &rcParent);
            GetWindowRect(hDlg, &rcDlg);

            // Вычисляем координаты для центрирования
            int x = rcParent.left + (rcParent.right - rcParent.left) / 2 -
                (rcDlg.right - rcDlg.left) / 2;
            int y = rcParent.top + (rcParent.bottom - rcParent.top) / 2 -
                (rcDlg.bottom - rcDlg.top) / 2;

            // Перемещаем диалог
            SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }

        LOGFONT lf = { 0 };
        lf.lfHeight = -MulDiv(12, GetDeviceCaps(GetDC(hDlg), LOGPIXELSY), 72); // ~12pt
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");

        HFONT hTitleFont = CreateFontIndirectW(&lf);
        if (hTitleFont)
        {
            HWND hTitle = GetDlgItem(hDlg, IDC_SETTINGWND_TITLE);
            if (hTitle)
            {
                SendMessageW(hTitle, WM_SETFONT, (WPARAM)hTitleFont, MAKELPARAM(TRUE, 0));
                // Сохраняем хендл шрифта для удаления при закрытии
                SetPropW(hDlg, L"CustomTitleFont", (HANDLE)hTitleFont);
            }
        }

        ApplySettingsDialogLanguage(hDlg);
        PopulateLanguageCombo(hDlg);

        //начальные состояния чекбоксов
        SendDlgItemMessageW(hDlg, IDC_CHECK_FLAC, BM_SETCHECK, rec_is_flac ? BST_CHECKED : BST_UNCHECKED, 0);
        SendDlgItemMessageW(hDlg, IDC_CHECK_MP3, BM_SETCHECK, rec_is_flac ? BST_UNCHECKED : BST_CHECKED, 0);
        SendDlgItemMessageW(hDlg, IDC_CHECK_STEREO_WIDTH, BM_SETCHECK, g_enableStereoWidth ? BST_CHECKED : BST_UNCHECKED, 0);
        SendDlgItemMessageW(hDlg, IDC_CHECK_DYNAMIC_AUTO_VOLUME, BM_SETCHECK, g_enableDynamicAutoVolume ? BST_CHECKED : BST_UNCHECKED, 0);
        SendDlgItemMessageW(hDlg, IDC_CHECK_LUFS_GAIN_NORMALIZER, BM_SETCHECK, g_enableLufsGainNormalizer ? BST_CHECKED : BST_UNCHECKED, 0);
        SendDlgItemMessageW(hDlg, IDC_CHECK_EXCITER, BM_SETCHECK, g_enableExciter ? BST_CHECKED : BST_UNCHECKED, 0);
        SendDlgItemMessageW(hDlg, IDC_CHECK_DEEP_BASS, BM_SETCHECK, g_enableDeepBass ? BST_CHECKED : BST_UNCHECKED, 0);
        SendDlgItemMessageW(hDlg, IDC_CHECK_LIMITER_GAIN_RIDER, BM_SETCHECK, g_enableLimiterGainRider ? BST_CHECKED : BST_UNCHECKED, 0);
        SendDlgItemMessageW(hDlg, IDC_CHECK_MINIMIZE_TO_TRAY, BM_SETCHECK, g_minimizeToTray ? BST_CHECKED : BST_UNCHECKED, 0);
        SendDlgItemMessageW(hDlg, IDC_CHECK_SHOW_TRACK_TOAST, BM_SETCHECK, g_showTrackToastInTray ? BST_CHECKED : BST_UNCHECKED, 0);
        SetDlgItemInt(hDlg, IDC_EDIT_STEREO_WIDTH, static_cast<UINT>((std::max)(0, (std::min)(100, g_stereoWidthPercent))), FALSE);

        if (HWND hEditStereo = GetDlgItem(hDlg, IDC_EDIT_STEREO_WIDTH)) {
            // установка стиля ES_NUMBER
            LONG_PTR style = GetWindowLongPtr(hEditStereo, GWL_STYLE);
            SetWindowLongPtr(hEditStereo, GWL_STYLE, style | ES_NUMBER);
            SendMessage(hEditStereo, EM_SETLIMITTEXT, (WPARAM)3, 0); // максимум 3 символа (0..100)
            EnableWindow(hEditStereo, g_enableStereoWidth ? TRUE : FALSE);
        }

        SetupSettingsDialogTooltips(hDlg);

        return (INT_PTR)TRUE;
    }
    case WM_LBUTTONDOWN:
    {
        // Проверяем, кликнули ли в область заголовка (первые 20 пикселей по Y)
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (pt.y < 20)
        {
            ReleaseCapture();
            SendMessage(hDlg, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
    {
        WORD id = LOWORD(wParam);
        WORD notif = HIWORD(wParam);

        switch (id)
        {
        case IDC_CHECK_FLAC:
            if (notif == BN_CLICKED)
            {
                // Если чекнули FLAC — ставим rec_is_flac = true и снимаем MP3
                BOOL checked = (SendDlgItemMessageW(hDlg, IDC_CHECK_FLAC, BM_GETCHECK, 0, 0) == BST_CHECKED);
                rec_is_flac = (checked != FALSE);
                SendDlgItemMessageW(hDlg, IDC_CHECK_MP3, BM_SETCHECK, rec_is_flac ? BST_UNCHECKED : BST_CHECKED, 0);
            }
            return (INT_PTR)TRUE;

        case IDC_CHECK_MP3:
            if (notif == BN_CLICKED)
            {
                // Если чекнули MP3 — ставим rec_is_flac = false и снимаем FLAC
                BOOL checked = (SendDlgItemMessageW(hDlg, IDC_CHECK_MP3, BM_GETCHECK, 0, 0) == BST_CHECKED);
                rec_is_flac = (checked == FALSE); // если MP3 checked => rec_is_flac = false
                SendDlgItemMessageW(hDlg, IDC_CHECK_FLAC, BM_SETCHECK, rec_is_flac ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            return (INT_PTR)TRUE;

        case IDC_CHECK_STEREO_WIDTH:
            if (notif == BN_CLICKED)
            {
                g_enableStereoWidth =
                    (SendDlgItemMessageW(hDlg, IDC_CHECK_STEREO_WIDTH, BM_GETCHECK, 0, 0) == BST_CHECKED);
                if (HWND hEditStereo = GetDlgItem(hDlg, IDC_EDIT_STEREO_WIDTH)) {
                    EnableWindow(hEditStereo, g_enableStereoWidth ? TRUE : FALSE);
                }
                SyncStereoWidthPercentFromDialog(hDlg, true);
            }
            return (INT_PTR)TRUE;

        case IDC_CHECK_DYNAMIC_AUTO_VOLUME:
            if (notif == BN_CLICKED)
            {
                g_enableDynamicAutoVolume =
                    (SendDlgItemMessageW(hDlg, IDC_CHECK_DYNAMIC_AUTO_VOLUME, BM_GETCHECK, 0, 0) == BST_CHECKED);
            }
            return (INT_PTR)TRUE;

        case IDC_CHECK_LUFS_GAIN_NORMALIZER:
            if (notif == BN_CLICKED)
            {
                g_enableLufsGainNormalizer =
                    (SendDlgItemMessageW(hDlg, IDC_CHECK_LUFS_GAIN_NORMALIZER, BM_GETCHECK, 0, 0) == BST_CHECKED);
                if (!g_enableLufsGainNormalizer) {
                    g_lufsNormalizerStatus.clear();
                    InvalidateNowPlayingBar(g_hMainWnd);
                }
            }
            return (INT_PTR)TRUE;

        case IDC_CHECK_EXCITER:
            if (notif == BN_CLICKED)
            {
                g_enableExciter =
                    (SendDlgItemMessageW(hDlg, IDC_CHECK_EXCITER, BM_GETCHECK, 0, 0) == BST_CHECKED);
            }
            return (INT_PTR)TRUE;

        case IDC_CHECK_DEEP_BASS:
            if (notif == BN_CLICKED)
            {
                g_enableDeepBass =
                    (SendDlgItemMessageW(hDlg, IDC_CHECK_DEEP_BASS, BM_GETCHECK, 0, 0) == BST_CHECKED);
            }
            return (INT_PTR)TRUE;

        case IDC_CHECK_LIMITER_GAIN_RIDER:
            if (notif == BN_CLICKED)
            {
                g_enableLimiterGainRider =
                    (SendDlgItemMessageW(hDlg, IDC_CHECK_LIMITER_GAIN_RIDER, BM_GETCHECK, 0, 0) == BST_CHECKED);
                if (!g_enableLimiterGainRider) {
                    g_limiterRiderStatus.clear();
                    InvalidateNowPlayingBar(g_hMainWnd);
                }
            }
            return (INT_PTR)TRUE;

        case IDC_CHECK_MINIMIZE_TO_TRAY:
            if (notif == BN_CLICKED)
            {
                g_minimizeToTray =
                    (SendDlgItemMessageW(hDlg, IDC_CHECK_MINIMIZE_TO_TRAY, BM_GETCHECK, 0, 0) == BST_CHECKED);
            }
            return (INT_PTR)TRUE;

        case IDC_CHECK_SHOW_TRACK_TOAST:
            if (notif == BN_CLICKED)
            {
                g_showTrackToastInTray =
                    (SendDlgItemMessageW(hDlg, IDC_CHECK_SHOW_TRACK_TOAST, BM_GETCHECK, 0, 0) == BST_CHECKED);
            }
            return (INT_PTR)TRUE;

        case IDC_COMBO_LANGUAGE:
            if (notif == CBN_SELCHANGE)
            {
                HWND hCombo = GetDlgItem(hDlg, IDC_COMBO_LANGUAGE);
                int sel = hCombo ? static_cast<int>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0)) : CB_ERR;
                if (sel != CB_ERR) {
                    LRESULT itemData = SendMessageW(hCombo, CB_GETITEMDATA, sel, 0);
                    const auto& languageOptions = GetAvailableLanguages();
                    if (itemData >= 0 && static_cast<size_t>(itemData) < languageOptions.size()) {
                        if (LoadLanguageById(languageOptions[static_cast<size_t>(itemData)].id)) {
                            savePlaylistToDat(L"app.dat", playlist, g_currentlyPlayingIndex);
                            ApplySettingsDialogLanguage(hDlg);
                            SetupSettingsDialogTooltips(hDlg);
                            ApplyMainDialogLanguage(g_hMainWnd);
                        }
                    }
                }
            }
            return (INT_PTR)TRUE;

        case IDC_EDIT_STEREO_WIDTH:
            if (notif == EN_CHANGE) {
                SyncStereoWidthPercentFromDialog(hDlg, false);
            }
            else if (notif == EN_KILLFOCUS) {
                SyncStereoWidthPercentFromDialog(hDlg, true);
            }
            return (INT_PTR)TRUE;

        case IDOK:
            SyncStereoWidthPercentFromDialog(hDlg, true);
            EndDialog(hDlg, IDOK);
            return (INT_PTR)TRUE;
            
        case IDCANCEL:
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    case WM_DESTROY:
    {
        // Освобождаем шрифт
        HFONT hFont = (HFONT)GetPropW(hDlg, L"CustomTitleFont");
        if (hFont) DeleteObject(hFont);
        RemovePropW(hDlg, L"CustomTitleFont");
        break;
    }

    default:
        break;
    }

    return (INT_PTR)FALSE;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{ 
    HANDLE hMutex = CreateMutexW(NULL, FALSE, SINGLE_INSTANCE_MUTEX_NAME);

    if (hMutex == NULL) {
         return 1;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Экземпляр уже запущен — активируем его окно
        HWND hWndExisting = FindWindowW(NULL, L"IRP+ffmpeG");
        if (hWndExisting) {
            if (!PostMessageW(hWndExisting, WM_APP_RESTORE_FROM_SINGLE_INSTANCE, 0, 0)) {
                ShowWindow(hWndExisting, SW_RESTORE);
                SetForegroundWindow(hWndExisting);
            }
        }

        CloseHandle(hMutex);
        return 0;
    }

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
   
    // Инициализация common controls
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    DialogBox(hInstance, MAKEINTRESOURCE(IDD_MAIN_DIALOG), NULL, DialogProc);
       
    CloseHandle(hMutex);

    return 0;
}

INT_PTR CALLBACK DialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
     
    g_hMainWnd = hDlg;

    if (message == g_wmTaskbarCreated && g_wmTaskbarCreated != 0 && g_isInTray) {
        AddTrayIcon(hDlg);
        return (INT_PTR)TRUE;
    }

    switch (message)
    {
    case WM_INITDIALOG: {
        InitializeLanguageSystem();
        av_log_set_level(AV_LOG_INFO);
        av_log_set_callback(FfmpegLogCallback);

        // Init SDL
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            std::wstring sdlError = utf8_to_wstring(SDL_GetError());
            std::wstring errorMsg = L"SDL could not initialize! Error: " + sdlError;
            MessageBox(hDlg, errorMsg.c_str(), L"SDL Error", MB_OK);
            EndDialog(hDlg, -1);
            return (INT_PTR)FALSE;
        }
        int imgFlags = IMG_INIT_JPG;
        if (!(IMG_Init(imgFlags) & imgFlags)) {
            std::wstring imgError = utf8_to_wstring(IMG_GetError());
            std::wstring errorMsg = L"SDL_image could not initialize for JPG support! Error: " + imgError;
            MessageBox(hDlg, errorMsg.c_str(), L"SDL_image Error", MB_OK);
            SDL_Quit();
            EndDialog(hDlg, -1);
            return (INT_PTR)FALSE;
        }

        if (!initCoverRenderer(hDlg)) {
            //MessageBox(hDlg, L"Failed to init cover renderer", L"Error", MB_OK);
        }

        InitializeCriticalSection(&g_url_vec_cs);
      
        //для окна сдл
        g_hStatic = GetDlgItem(hDlg, IDC_STATIC_SDL);
        g_hbrBlack = CreateSolidBrush(RGB(0, 0, 0));
        // Create a font for the buttons
        hButtonFont = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe Fluent Icons");
        // Create a font for the listbox
        hListboxFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        hNowPlayingTitleFont = CreateFontW(19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        //принудительно включаем горизонтальную полосу для IDC_LIST2
        {
            HWND hHistory = GetDlgItem(hDlg, IDC_LIST2);
            if (hHistory) {
                // Добавим стиль горизонтального скролла, если его нет
                LONG_PTR style = GetWindowLongPtr(hHistory, GWL_STYLE);
                if (!(style & WS_HSCROLL)) {
                    SetWindowLongPtr(hHistory, GWL_STYLE, style | WS_HSCROLL);
                    SetWindowPos(hHistory, NULL, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
                }
                                
                SendMessage(hHistory, LB_SETHORIZONTALEXTENT, (WPARAM)alwaysVisibleExtent, 0);
                // Явно показать горизонтальную полосу
                ShowScrollBar(hHistory, SB_HORZ, TRUE);
                SendMessage(hHistory, WM_SETFONT, (WPARAM)hListboxFont, TRUE);

                const int newItemHeight = 23; // желаемая высота строки в пикселях
                SendMessageW(hHistory, LB_SETITEMHEIGHT, 0, (LPARAM)newItemHeight);
            }
        }

        //control настройки
        g_hSettingStatic = GetDlgItem(hDlg, IDC_ST_SETTING);
        if (g_hSettingStatic) {
            LONG_PTR style = GetWindowLongPtr(g_hSettingStatic, GWL_STYLE);
            style |= SS_NOTIFY;
            SetWindowLongPtr(g_hSettingStatic, GWL_STYLE, style);
            SetWindowPos(g_hSettingStatic, NULL, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            SetWindowSubclass(g_hSettingStatic, SettingStaticSubclassProc, 0, 0);
            InvalidateRect(g_hSettingStatic, NULL, TRUE);
            UpdateWindow(g_hSettingStatic);
        }
        // Получаем дескрипторы слайдеров
       //слайдеры громкости и тембра
        g_hSliderVolume = GetDlgItem(hDlg, IDC_SLIDER_VOL);
        g_hSliderTreble = GetDlgItem(hDlg, IDC_SLIDER_HI);
        g_hSliderBass = GetDlgItem(hDlg, IDC_SLIDER_BASS);
        g_hNowPlayingBar = GetDlgItem(hDlg, IDC_STATIC_NOW_PLAYING_BAR);
        SetupMainDialogTooltips(hDlg);

        // Load playlist from file
        int selectedIndex = -1;
        if (!loadPlaylistFromDat(L"app.dat", playlist, selectedIndex)) {
            // If loading from app.dat fails, load from m3u as a fallback
            loadPlaylist(L"playlist.m3u", playlist);
        }
        if (!LoadLanguageById(g_languageId)) {
            g_languageId = L"russian";
            LoadLanguageById(g_languageId);
        }
        ApplyMainDialogLanguage(hDlg);

        if (IsPlaylistOutdatedAgainstM3U(playlist)) {
            // обновляем данные плейлиста
            std::vector<PlaylistItem> previousPlaylist = playlist;
            std::vector<PlaylistItem> m3uPlaylist;
            loadPlaylist(L"playlist.m3u", m3uPlaylist);
            PreserveIcyNameDisableFlagsByUrl(previousPlaylist, m3uPlaylist);
            playlist = std::move(m3uPlaylist);
            selectedIndex = -1;
        }

        if (playlist.empty()) {
         
			//error loading playlist
			//show message box with error
        
        }

        SendMessage(g_hSliderVolume, TBM_SETRANGEMIN, TRUE, 0);
        SendMessage(g_hSliderVolume, TBM_SETRANGEMAX, TRUE, 100);
        float rawVol = current_volume.load();
        if (rawVol < 0.0f) rawVol = 0.0f;
        if (rawVol > 1.0f) rawVol = 1.0f;
        int vol = (int)std::round(rawVol * 100.0f);
        SendMessage(g_hSliderVolume, TBM_SETPOS, TRUE, vol);

        SendMessage(g_hSliderBass, TBM_SETRANGE, (WPARAM)TRUE, MAKELPARAM(-16, 16));
        int gain = (int)current_eq_gain_bass.load();
        SendMessage(g_hSliderBass, TBM_SETPOS, TRUE, gain);
        SendMessage(g_hSliderBass, TBM_SETPAGESIZE, TRUE, (LPARAM)1);

        SendMessage(g_hSliderTreble, TBM_SETRANGE, (WPARAM)TRUE, MAKELPARAM(-16, 16));
        gain = (int)current_eq_gain.load();
        SendMessage(g_hSliderTreble, TBM_SETPOS, TRUE, gain);
        SendMessage(g_hSliderTreble, TBM_SETPAGESIZE, 0, (LPARAM)1);
        // Setup ListView
        HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);
        SendMessage(hListView, WM_SETFONT, (WPARAM)hListboxFont, TRUE);
        ListView_SetExtendedListViewStyle(hListView, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
        // Add columns
        LVCOLUMN lvc = { 0 };
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCFMT_LEFT;

        lvc.pszText = (LPWSTR)L"Название станции";
        lvc.cx = 224;
        ListView_InsertColumn(hListView, 0, &lvc);

        lvc.pszText = (LPWSTR)L"URL - адрес радио";
        lvc.cx = 320;
        ListView_InsertColumn(hListView, 1, &lvc);
        
        // Add items from playlist vector
        LVITEM lvi = { 0 };
        lvi.mask = LVIF_TEXT;

        for (int i = 0; i < playlist.size(); ++i)
        {
            lvi.iItem = i;
            std::wstring displayName = GetPlaylistDisplayName(i);
            lvi.pszText = (LPWSTR)displayName.c_str();
            ListView_InsertItem(hListView, &lvi);
            ListView_SetItemText(hListView, i, 1, (LPWSTR)playlist[i].url.c_str());
            ListView_SetColumnWidth(hListView, 1, LVSCW_AUTOSIZE);
            int cur = ListView_GetColumnWidth(hListView, 1);
            ListView_SetColumnWidth(hListView, 1, cur + 128);
        }
		//ширина колонки с названием станции по содержимому
        SetStationNameColumnWidth(hListView);
             
        if (selectedIndex != -1) {
            // Make sure the index is valid
            if (selectedIndex < ListView_GetItemCount(hListView)) {
                // Set selection and focus
                ListView_SetItemState(hListView, selectedIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                // Ensure the selected item is visible
                if (selectedIndex + 1 < ListView_GetItemCount(hListView) && selectedIndex != 0) {
                    ListView_EnsureVisible(hListView, selectedIndex + 1, FALSE);
                }
                else {
                    ListView_EnsureVisible(hListView, selectedIndex, FALSE);
                }
                 // Optionally, update the global playing index if you auto-play
                g_currentlyPlayingIndex = selectedIndex;
                UpdatePlayingIndicator(-1, selectedIndex);
            }
        }

        // Если нет названия станции или название станции = url,
        // пытаемся получить имя по url уже после того, как ListView полностью создан.
        StartPlaylistNameResolveThread();

        RECT rc;
        GetWindowRect(hDlg, &rc);
        int winW = rc.right - rc.left;
        int winH = rc.bottom - rc.top;

        // рабочая область экрана (исключая панель задач)
        RECT work;
        SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
        int screenW = work.right - work.left;
        int screenH = work.bottom - work.top;

        int x = work.left + (screenW - winW) / 2;
        int y = work.top + (screenH - winH) / 2;

        SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        
        SetTimer(hDlg, ID_TIMER_IMAGE_URL, 1000, NULL);

		//загрузка и отображение изображения в отдельном потоке
        g_stopImageThread = false;

        return (INT_PTR)TRUE;
    }
    case WM_ACTIVATE:
    {
        WORD activationState = LOWORD(wParam);
        if (activationState == WA_INACTIVE) {
            HWND hFocus = GetFocus();
            if (hFocus && IsChild(hDlg, hFocus)) {
                const int controlId = GetDlgCtrlID(hFocus);
                switch (controlId) {
                case IDC_BUTTON_PP:
                case IDC_BUTTON_REV:
                case IDC_BUTTON_FORV:
                case IDC_BUTTON_PREVIOUS_STATION:
                case IDC_BUTTON_REC:
                    SetFocus(hDlg);
                    break;
                default:
                    break;
                }
            }
        }
        else if (HIWORD(wParam) == 0 && !g_isInTray && !g_minimizeToTrayFromCaptionButton) {
            ForceForegroundWindow(hDlg);
            PostMessageW(hDlg, WM_APP_ENSURE_FOREGROUND, 0, 0);
        }
        break;
    }

    case WM_MOUSEACTIVATE:
        if (LOWORD(lParam) == HTMINBUTTON) {
            g_minimizeToTrayFromCaptionButton = true;
            return MA_ACTIVATE;
        }
        break;

    case WM_NCLBUTTONDOWN:
        if (wParam == HTMINBUTTON) {
            g_minimizeToTrayFromCaptionButton = true;
        }
        break;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_RESTORE) {
            RestoreMainWindowFromTaskbar(hDlg);
            return (INT_PTR)TRUE;
        }
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            const bool sendToTray = g_minimizeToTrayFromCaptionButton &&
                g_minimizeToTray &&
                !g_isReallyExiting &&
                !g_restoringFromTray;
            g_minimizeToTrayFromCaptionButton = false;

            if (sendToTray) {
                HideMainWindowToTray(hDlg);
                return (INT_PTR)TRUE;
            }
        }
        break;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            if (IsWindow(hDlg)) {
                SetWindowPos(hDlg, HWND_NOTOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            }
        }
        g_minimizeToTrayFromCaptionButton = false;
        break;
    case WM_LBUTTONDOWN:
    {
        // Начинаем перетаскивание окна
        ReleaseCapture(); // важно для корректного drag
        SendMessage(hDlg, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        break;
    }
    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        HWND hwndCtl = (HWND)lParam;

        SetBkMode(hdc, TRANSPARENT);

        if (hwndCtl == g_hStatic) {
            return (INT_PTR)g_hbrBlack; // кисть для фона для sdl2 окна спектра
        }

        break;

    }
    case WM_TIMER:
    {
        if (wParam == 1)
        {
            KillTimer(hDlg, 1); // одноразовый

        }

        if (wParam == IDT_COVER_RESTORE) {

            if (!redrawCoverImage(hDlg)) {

				//LogToUI("Failed to redraw IDT_COVER_RESTORE cover image after UAC");

            }

            return TRUE;
        }

        if (wParam == 2) {

            KillTimer(hDlg, 2); // одноразовый
        }

        if (wParam == ID_TIMER_IMAGE_URL)
        {
            if (!g_bIsImageUrlThreadRunning && !vec_url.empty())
            {
                if (g_imageUrlThread.joinable()) {
                    g_imageUrlThread.join();
                }
                // находится в cover_art.cpp
                g_imageUrlThread = std::thread(GetImageUrlThread);
                //g_imageUrlThread.detach();
            }
        }
        else if (wParam == ID_TIMER_METADATA)
        {
            // таймер метаданных
            update_stream_metadata();
        }
        break;
    }
    case WM_APP_ENSURE_FOREGROUND:
        if (IsWindow(hDlg)) {
            ForceForegroundWindow(hDlg);
            SetWindowPos(hDlg, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        }
        return (INT_PTR)TRUE;

    case WM_APP_SET_VOLUME_SLIDER:
        SendMessage(g_hSliderVolume, TBM_SETPOS, TRUE, (LPARAM)wParam);
        InvalidateRect(g_hSliderVolume, nullptr, TRUE);
        UpdateWindow(g_hSliderVolume);
        break;
    case WM_APP_PLAYLIST_NAME_RESOLVED:
    {
        std::unique_ptr<PlaylistNameResolvedPayload> payload(reinterpret_cast<PlaylistNameResolvedPayload*>(lParam));
        if (!payload) {
            return 0;
        }
        if (payload->generation != g_playlistNameResolveGeneration.load()) {
            return 0;
        }
        if (payload->index < 0 || payload->index >= static_cast<int>(playlist.size())) {
            return 0;
        }
        if (playlist[payload->index].url != payload->url) {
            return 0;
        }
        if (!playlist[payload->index].name.empty() && playlist[payload->index].name != playlist[payload->index].url) {
            return 0;
        }

        playlist[payload->index].name = payload->name;

        HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);
        if (!hListView) {
            return 0;
        }

        std::wstring displayName = GetPlaylistDisplayName(payload->index);
        if (payload->index == g_currentlyPlayingIndex) {
            wchar_t currentText[256] = { 0 };
            ListView_GetItemText(hListView, payload->index, 0, currentText, _countof(currentText));
            if (std::wstring(currentText).rfind(L"▶ ", 0) == 0) {
                displayName = L"▶ " + displayName;
            }
        }

        ListView_SetItemText(hListView, payload->index, 0, const_cast<LPWSTR>(displayName.c_str()));
        // После обновления текста подстроим ширину колонки
        SetStationNameColumnWidth(hListView);
        return 0;
    }
    case WM_APP_STATION_NAME_FROM_ICY:
    {
        std::unique_ptr<StationNameFromIcyPayload> payload(reinterpret_cast<StationNameFromIcyPayload*>(lParam));
        if (!payload) {
            return 0;
        }
        if (!g_enableIcyStationNameUpdates) {
            return 0;
        }
        if (payload->generation != g_playbackGeneration.load()) {
            return 0;
        }
        const int index = g_currentlyPlayingIndex;
        if (index < 0 || index >= static_cast<int>(playlist.size())) {
            return 0;
        }
        if (playlist[index].disable_name_icy) {
            playlist[index].name_icy.clear();
            return 0;
        }
        std::wstring stationName = TrimWide(payload->name);
        if (stationName.empty()) {
            return 0;
        }
        if (stationName.size() > 128) {
            stationName.resize(128);
            stationName = TrimWide(stationName);
        }
        if (stationName.empty()) {
            return 0;
        }

        if (playlist[index].name_icy == stationName) {
            return 0;
        }

        playlist[index].name_icy = stationName;

        HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);
        if (!hListView) {
            return 0;
        }

        std::wstring displayName = L"▶ " + stationName;
        ListView_SetItemText(hListView, index, 0, const_cast<LPWSTR>(displayName.c_str()));
        SetStationNameColumnWidth(hListView);
        InvalidateNowPlayingBar(hDlg);
        return 0;
    }
    case WM_APP_METADATA_UPDATED:
    {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
        if (!text) {
            return 0;
        }

        g_nowPlayingTitle = *text;
        InvalidateNowPlayingBar(hDlg);
        return 0;
    }
    case WM_APP_FFMPEG_STATUS:
    {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
        if (!text) {
            return 0;
        }

        g_nowPlayingStatus = *text;
        InvalidateNowPlayingBar(hDlg);
        return 0;
    }
    case WM_APP_STREAM_INFO:
    {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
        if (!text) {
            return 0;
        }

        if (text->empty()) {
            g_nowPlayingStreamInfo.clear();
            InvalidateNowPlayingBar(hDlg);
            return 0;
        }

        if (!g_audioStreamInfoAllowed.load()) {
            return 0;
        }

        g_nowPlayingStreamInfo = *text;
        InvalidateNowPlayingBar(hDlg);
        return 0;
    }
    case WM_APP_LIMITER_RIDER_STATUS:
    {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
        if (!text) {
            return 0;
        }

        g_limiterRiderStatus = *text;
        InvalidateNowPlayingBar(hDlg);
        return 0;
    }
    case WM_APP_LUFS_NORMALIZER_STATUS:
    {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
        if (!text) {
            return 0;
        }

        g_lufsNormalizerStatus = *text;
        InvalidateNowPlayingBar(hDlg);
        return 0;
    }
    case WM_COMMAND:
    {
        switch (LOWORD(wParam))
        {
        case IDOK:
        case IDCANCEL:
            RequestApplicationExit(hDlg);
            return (INT_PTR)TRUE;

        case IDM_TRAY_RESTORE:
            RestoreMainWindow(hDlg);
            return (INT_PTR)TRUE;

        case IDM_ABOUT:
            RestoreMainWindow(hDlg);
            DialogBox(GetModuleHandle(NULL),
                MAKEINTRESOURCE(IDD_ABOUTBOX),
                hDlg,
                AboutDialogProc);
            return (INT_PTR)TRUE;

        case IDM_EXIT:
            RequestApplicationExit(hDlg);
            return (INT_PTR)TRUE;

        case ID_LISTBOX_COPY:
        {
            HWND hList = GetDlgItem(hDlg, IDC_LIST2);
            int idx = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);

            if (idx == LB_ERR || idx < 0 || idx >= (int)track_history.size()) {
                return (INT_PTR)TRUE;
            }

            const std::string& s = track_history[idx];
            std::wstring w = utf8_to_wstring(s);

            if (idx != LB_ERR)
            {
                if (w.size() > 0)
                {
                    if (OpenClipboard(hDlg))
                    {
                        EmptyClipboard();
                        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
                        if (hg)
                        {
                            LPWSTR lptstr = (LPWSTR)GlobalLock(hg);
                            if (lptstr)
                            {
                                memcpy(lptstr, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
                                GlobalUnlock(hg);
                                if (!SetClipboardData(CF_UNICODETEXT, hg))
                                {
                                    GlobalFree(hg);
                                }
                            }
                            else
                            {
                                GlobalFree(hg);
                            }
                        }
                        CloseClipboard();
                    }
                }
            }
            return (INT_PTR)TRUE;
        }
        case IDC_ST_SETTING:
        {
            RestoreMainWindow(hDlg);
            DialogBox(GetModuleHandle(NULL),
                MAKEINTRESOURCE(IDD_DIALOG_SETTING),
                hDlg,
                SettingsDialogProc);

            return (INT_PTR)TRUE;
        }
        case IDC_BUTTON_REC:
        {
            if (running.load()) {

                if (g_rec_semafor.load()) {
                    //StopRecording;
                    g_rec_semafor.store(false);
                }
                else {
                    // StartRecording;
                    g_rec_semafor.store(true);
                }

                if (g_is_recording.load()) {
                    // находится в file_recording.cpp
                    StopRecording();
                }
                if (g_rec_semafor.load()) {
                    // находится в file_recording.cpp
                    StartRecording();
                }
            }

            return (INT_PTR)TRUE;
        }
        case IDC_BUTTON_PP:
        {
            if (running.load()) {
                if (g_is_recording.load())
                {
                    g_rec_semafor.store(false);
                    // находится в file_recording.cpp
                    StopRecording();

                    HWND hRec = GetDlgItem(hDlg, IDC_BUTTON_REC);
                    if (hRec) {
                        InvalidateRect(hRec, NULL, TRUE);
                        UpdateWindow(hRec);
                    }
                }

                StopPlayback();
            }
            else {
                       
                if (g_currentlyPlayingIndex != -1) {
                    PlayAtIndex(g_currentlyPlayingIndex);
                }
                else {
                    PlayAtIndex(0);
                }

				SetFocus(GetDlgItem(hDlg, IDC_LIST_URL));
                
            }
            return (INT_PTR)TRUE;
        }
        case IDC_BUTTON_REV:
        {
            HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);
            int count = ListView_GetItemCount(hListView);
            if (count > 0) {
                int current = (g_currentlyPlayingIndex == -1) ? 0 : g_currentlyPlayingIndex;
                int prev = (current - 1 + count) % count;
                PlayAtIndex(prev);

            }
            return (INT_PTR)TRUE;
        }
        case IDC_BUTTON_FORV:
        {
            HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);
            int count = ListView_GetItemCount(hListView);
            if (count > 0) {
                int current = (g_currentlyPlayingIndex == -1) ? 0 : g_currentlyPlayingIndex;
                int next = (current + 1) % count;
                PlayAtIndex(next);

            }

            return (INT_PTR)TRUE;
        }
        case IDC_BUTTON_PREVIOUS_STATION:
        {
            if (g_previousStationIndex >= 0 &&
                g_previousStationIndex < static_cast<int>(playlist.size()) &&
                g_previousStationIndex != g_currentlyPlayingIndex) {
                PlayAtIndex(g_previousStationIndex);
            }
            return (INT_PTR)TRUE;
        }
        case IDC_BUTTON_RELOAD_M3U:
        {
            ReloadPlaylistFromM3U();
            return (INT_PTR)TRUE;
        }
        case ID_LIST_URL_ADD_STATION:
        {
            AddStationToPlaylist(hDlg);
            return (INT_PTR)TRUE;
        }
        case ID_LIST_URL_SAVE_STATION:
        {
            HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);
            int index = hListView ? ListView_GetSelectionMark(hListView) : -1;
            if (hListView && index < 0) {
                index = ListView_GetNextItem(hListView, -1, LVNI_SELECTED);
            }
            SaveStationFromPlaylist(hDlg, index);
            return (INT_PTR)TRUE;
        }
        case ID_LIST_URL_EDIT_STATION:
        {
            HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);
            int index = hListView ? ListView_GetSelectionMark(hListView) : -1;
            if (hListView && index < 0) {
                index = ListView_GetNextItem(hListView, -1, LVNI_SELECTED);
            }
            EditStationNameInPlaylist(hDlg, index);
            return (INT_PTR)TRUE;
        }
        case ID_LIST_URL_DELETE_STATION:
        {
            HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);
            int index = hListView ? ListView_GetSelectionMark(hListView) : -1;
            if (hListView && index < 0) {
                index = ListView_GetNextItem(hListView, -1, LVNI_SELECTED);
            }
            DeleteStationFromPlaylist(hDlg, index);
            return (INT_PTR)TRUE;
        }
        }
    }
    break;
    case WM_ACTIVATEAPP:
		if (!wParam) {//need redraw after UAC prompt
           SetTimer(hDlg, IDT_COVER_RESTORE, 100, NULL);
 	    }
        else
        {  KillTimer(hDlg, IDT_COVER_RESTORE);
        }
        break;

    case WM_APP_COVER_DOWNLOADED: {

        bool coverReady = false;
        if (initCoverRenderer(hDlg)) {
            if (reloadCoverTexture()) {
                if (!redrawCoverImage(hDlg)) {
					//LogToUI("Failed to redraw cover image after download");
                }
                else {
                    coverReady = true;
                }
            }
            else {
				//LogToUI("Failed to reload cover texture after download");
            }
        }
        else {

			//LogToUI("Failed to init cover renderer after download");
        }

        if (coverReady) {
            ShowTrackToastIfNeeded(hDlg);
        }
  
    }
    break;
    case WM_APP_UPDATE_ICON:
    {
        UpdatePlayPauseButtonIcon(hDlg);
    }
    break;
    case WM_CONTEXTMENU:
    {
        HWND hwndCtrl = (HWND)wParam;
        HWND hList = GetDlgItem(hDlg, IDC_LIST2);
        HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);

        if (hwndCtrl == hListView)
        {
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            int itemIndex = -1;

            if (pt.x == -1 && pt.y == -1) {
                RECT rc;
                GetWindowRect(hListView, &rc);
                pt.x = rc.left + 16;
                pt.y = rc.top + 16;
                itemIndex = ListView_GetSelectionMark(hListView);
            }
            else {
                POINT ptClient = pt;
                ScreenToClient(hListView, &ptClient);

                LVHITTESTINFO hit = {};
                hit.pt = ptClient;
                itemIndex = ListView_HitTest(hListView, &hit);
                if (itemIndex >= 0) {
                    SetFocus(hListView);
                    ListView_SetItemState(hListView, itemIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_SetSelectionMark(hListView, itemIndex);
                }
            }

            HMENU hPopup = CreatePopupMenu();
            if (hPopup) {
                AppendMenuW(hPopup, MF_STRING, IDC_BUTTON_RELOAD_M3U, Tr("context.reload_m3u", L"Обновить m3u"));
                HBITMAP hReloadIcon = CreateMenuGlyphBitmap(hDlg, L"\uE72C");
                SetMenuItemBitmapByCommand(hPopup, IDC_BUTTON_RELOAD_M3U, hReloadIcon);

                AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hPopup, MF_STRING, ID_LIST_URL_ADD_STATION, Tr("context.add_station", L"Добавить станцию"));
                HBITMAP hAddIcon = CreateMenuGlyphBitmap(hDlg, L"\uE710");
                SetMenuItemBitmapByCommand(hPopup, ID_LIST_URL_ADD_STATION, hAddIcon);

                HBITMAP hDeleteIcon = nullptr;
                HBITMAP hSaveIcon = nullptr;
                HBITMAP hEditIcon = nullptr;
                if (itemIndex >= 0) {
                    AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(hPopup, MF_STRING, ID_LIST_URL_EDIT_STATION, Tr("context.edit_station", L"Изменить название станции"));
                    hEditIcon = CreateMenuGlyphBitmap(hDlg, L"\uE70F");
                    SetMenuItemBitmapByCommand(hPopup, ID_LIST_URL_EDIT_STATION, hEditIcon);

                    AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(hPopup, MF_STRING, ID_LIST_URL_SAVE_STATION, Tr("context.save_station", L"Сохранить радиостанцию"));
                    hSaveIcon = CreateMenuGlyphBitmap(hDlg, L"\uE74E");
                    SetMenuItemBitmapByCommand(hPopup, ID_LIST_URL_SAVE_STATION, hSaveIcon);

                    AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(hPopup, MF_STRING, ID_LIST_URL_DELETE_STATION, Tr("context.delete_station", L"Удалить станцию"));
                    hDeleteIcon = CreateMenuGlyphBitmap(hDlg, L"\uE74D");
                    SetMenuItemBitmapByCommand(hPopup, ID_LIST_URL_DELETE_STATION, hDeleteIcon);
                }
                TrackPopupMenu(hPopup, TPM_RIGHTBUTTON | TPM_TOPALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hDlg, NULL);
                DestroyMenu(hPopup);

                if (hReloadIcon) DeleteObject(hReloadIcon);
                if (hAddIcon) DeleteObject(hAddIcon);
                if (hEditIcon) DeleteObject(hEditIcon);
                if (hSaveIcon) DeleteObject(hSaveIcon);
                if (hDeleteIcon) DeleteObject(hDeleteIcon);
            }

            return (INT_PTR)TRUE;
        }

        if (hwndCtrl == hList)
        {
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);

            POINT ptClient = pt;
            ScreenToClient(hList, &ptClient);

            int itemIndex = (int)SendMessage(hList, LB_ITEMFROMPOINT, 0, MAKELPARAM(ptClient.x, ptClient.y));

            if (HIWORD(itemIndex) == 0)
            {
                SendMessage(hList, LB_SETCURSEL, LOWORD(itemIndex), 0);

                HMENU hMenu = LoadMenu(GetModuleHandle(NULL), MAKEINTRESOURCE(IDR_CONTEXT_MENU));
                if (hMenu)
                {
                    HMENU hSubMenu = GetSubMenu(hMenu, 0);
                    HBITMAP hCopyIcon = CreateMenuGlyphBitmap(hDlg, L"\uE8C8");
                    SetMenuItemBitmapByCommand(hSubMenu, ID_LISTBOX_COPY, hCopyIcon);
                    TrackPopupMenu(hSubMenu, TPM_RIGHTBUTTON | TPM_TOPALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hDlg, NULL);
                    DestroyMenu(hMenu);
                    if (hCopyIcon) DeleteObject(hCopyIcon);
                }
            }
        }

        return (INT_PTR)TRUE;
    }
    break;
    case WM_HSCROLL:
    {
        HWND hSlider = (HWND)lParam;
        if (hSlider == g_hSliderVolume || hSlider == g_hSliderTreble || hSlider == g_hSliderBass) {
            if (hSlider == g_hSliderVolume) {

                int pos = static_cast<int>(SendMessage(hSlider, TBM_GETPOS, 0, 0));
                float new_vol = pos / 100.0f;
                current_volume.store(new_vol);

            }
            else if (hSlider == g_hSliderTreble) {

                int pos = static_cast<int>(SendMessage(hSlider, TBM_GETPOS, 0, 0));
                float new_gain = static_cast<float>(pos);
                current_eq_gain.store(new_gain);
            }
            else if (hSlider == g_hSliderBass) {

                int pos = static_cast<int>(SendMessage(hSlider, TBM_GETPOS, 0, 0));
                float new_gain = static_cast<float>(pos);
                current_eq_gain_bass.store(new_gain);
            }

            UpdateFilterSettings();

            InvalidateRect(hSlider, NULL, FALSE);
        }

    }
    break;
    case WM_MEASUREITEM:
    {
        LPMEASUREITEMSTRUCT pMIS = (LPMEASUREITEMSTRUCT)lParam;
        if (pMIS->CtlID == IDC_LIST2)
        {
            pMIS->itemHeight = 22;
            return TRUE;
        }
    }
    break;
    case WM_DRAWITEM:
    {
        LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;

        if (pDIS->CtlType == ODT_STATIC && pDIS->CtlID == IDC_STATIC_NOW_PLAYING_BAR)
        {
            RECT rc = pDIS->rcItem;
            const int width = rc.right - rc.left;
            const int height = rc.bottom - rc.top;
            HDC memDC = CreateCompatibleDC(pDIS->hDC);
            HBITMAP memBmp = CreateCompatibleBitmap(pDIS->hDC, width, height);
            HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

            RECT localRc = { 0, 0, width, height };
            HBRUSH hBack = CreateSolidBrush(RGB(245, 246, 248));
            FillRect(memDC, &localRc, hBack);
            DeleteObject(hBack);

            HPEN hLinePen = CreatePen(PS_SOLID, 1, RGB(216, 220, 226));
            HPEN hOldPen = (HPEN)SelectObject(memDC, hLinePen);
            MoveToEx(memDC, localRc.left, localRc.bottom - 1, nullptr);
            LineTo(memDC, localRc.right, localRc.bottom - 1);
            SelectObject(memDC, hOldPen);
            DeleteObject(hLinePen);

            RECT rcContent = localRc;
            InflateRect(&rcContent, -6, -1);

            std::wstring barText = GetNowPlayingBarText();

            SetBkMode(memDC, TRANSPARENT);
            HFONT hOldFont = (HFONT)SelectObject(
                memDC,
                hListboxFont ? hListboxFont : hNowPlayingTitleFont);
            SetTextColor(memDC, RGB(35, 35, 35));
            DrawTextW(memDC, barText.c_str(), -1, &rcContent,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(memDC, hOldFont);

            BitBlt(pDIS->hDC, rc.left, rc.top, width, height, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);

            return TRUE;
        }

        if (pDIS->CtlType == ODT_BUTTON)
        {
            COLORREF bgColor = (pDIS->itemState & ODS_SELECTED) ? RGB(200, 220, 255) : RGB(225, 235, 255);
            if (pDIS->CtlID == IDC_BUTTON_PP) {
                bgColor = (pDIS->itemState & ODS_SELECTED) ? RGB(180, 205, 240) : RGB(205, 220, 245);
            }
            HBRUSH hBrush = CreateSolidBrush(bgColor);
            FillRect(pDIS->hDC, &pDIS->rcItem, hBrush);
            DeleteObject(hBrush);

            HPEN hPen = CreatePen(PS_SOLID, 1, RGB(150, 170, 220));
            HPEN hOldPen = (HPEN)SelectObject(pDIS->hDC, hPen);
            HBRUSH hNullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH hOldBrush = (HBRUSH)SelectObject(pDIS->hDC, hNullBrush);

            // Закруглённый прямоугольник: радиус 6x6
            RoundRect(
                pDIS->hDC,
                pDIS->rcItem.left,
                pDIS->rcItem.top,
                pDIS->rcItem.right,
                pDIS->rcItem.bottom,
                6, 6
            );

            SelectObject(pDIS->hDC, hOldBrush);
            SelectObject(pDIS->hDC, hOldPen);
            DeleteObject(hPen);
      
            const wchar_t* icon = L"";
            
            switch (pDIS->CtlID)
            {
            case IDC_BUTTON_PP:
            {
                if (running.load())
                {  icon = L"\ue769";
                }
                else
                {  icon = L"\uE768";
                }
                break;
            }
            case IDC_BUTTON_REV: icon = L"\uE892"; break;
            case IDC_BUTTON_FORV: icon = L"\uE893"; break;
            case IDC_BUTTON_PREVIOUS_STATION: icon = L"\uE8EE"; break;
            case IDC_BUTTON_REC: {
                if (g_rec_semafor.load()) {
                    icon = L"\uea3b";
                }
                else {
                    icon = L"\uea3f";
                }
                break;
            }
            }

            HFONT hOldFont = (HFONT)SelectObject(pDIS->hDC, hButtonFont);
            SetTextColor(pDIS->hDC, RGB(0, 0, 0));
            SetBkMode(pDIS->hDC, TRANSPARENT);
            DrawTextW(pDIS->hDC, icon, -1, &pDIS->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(pDIS->hDC, hOldFont);

            if (pDIS->itemState & ODS_FOCUS)
            {
                RECT rcFocus = pDIS->rcItem;
                InflateRect(&rcFocus, -3, -3);

                HPEN hFocusPen = CreatePen(PS_DOT, 1, RGB(80, 100, 160));
                HPEN hOldFocusPen = (HPEN)SelectObject(pDIS->hDC, hFocusPen);
                HBRUSH hFocusBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
                HBRUSH hOldFocusBrush = (HBRUSH)SelectObject(pDIS->hDC, hFocusBrush);

                RoundRect(
                    pDIS->hDC,
                    rcFocus.left,
                    rcFocus.top,
                    rcFocus.right,
                    rcFocus.bottom,
                    6, 6
                );

                SelectObject(pDIS->hDC, hOldFocusBrush);
                SelectObject(pDIS->hDC, hOldFocusPen);
                DeleteObject(hFocusPen);
            }
            return TRUE;
        }
        else if (pDIS->CtlType == ODT_LISTBOX && pDIS->CtlID == IDC_LIST2)
        {
            if (pDIS->itemID == -1) return TRUE;
                        
            const bool isCurrent = (pDIS->itemID == 0);
            const bool isSelected = (pDIS->itemState & ODS_SELECTED) != 0;

            COLORREF clrText, clrBkg;
            if (isSelected)
            {
                clrBkg = isCurrent ? RGB(220, 228, 238) : RGB(233, 230, 233);
                clrText = RGB(20, 20, 20);
            }
            else
            {
                clrBkg = isCurrent ? RGB(242, 244, 247) : RGB(255, 255, 255);
                clrText = RGB(0, 0, 0);
            }

            HBRUSH hBrush = CreateSolidBrush(clrBkg);
            FillRect(pDIS->hDC, &pDIS->rcItem, hBrush);
            DeleteObject(hBrush);
                        
            if (pDIS->itemID < (int)track_history.size()) {
                if (!track_history.empty()) {
                    const std::string& s = track_history[pDIS->itemID];
                    std::wstring w = utf8_to_wstring(s);
                    //OutputDebugString(w.c_str());
                    // draw w.c_str()
                    SetTextColor(pDIS->hDC, clrText);
                    SetBkMode(pDIS->hDC, TRANSPARENT);
                    RECT rcText = pDIS->rcItem; rcText.left += 5;
                    HFONT hOldFont = (HFONT)SelectObject(pDIS->hDC, hListboxFont);
                    DrawTextW(pDIS->hDC, w.c_str(), -1, &rcText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    SelectObject(pDIS->hDC, hOldFont);
                }
            }

            if ((pDIS->itemState & ODS_FOCUS) && !(pDIS->itemState & ODS_SELECTED))
            {
                DrawFocusRect(pDIS->hDC, &pDIS->rcItem);
            }
            return TRUE;
        }

    }
    break;
    case WM_PAINT:
    {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hDlg, &ps);    

        if (!redrawCoverImage(hDlg)) {
            //LogToUI("Failed to redraw cover image after WM_PAINT");
        }

		EndPaint(hDlg, &ps);    
        
        return TRUE;
    }
    break;
    case WM_NOTIFY:
    {
        LPNMHDR lpnm = (LPNMHDR)lParam;
        if (lpnm->code == NM_CUSTOMDRAW)
        {
            LPNMCUSTOMDRAW lpcd = (LPNMCUSTOMDRAW)lParam;
            // Проверяем, что это нужный TrackBar
            if (lpcd->hdr.idFrom == IDC_SLIDER_VOL ||
                lpcd->hdr.idFrom == IDC_SLIDER_BASS ||
                lpcd->hdr.idFrom == IDC_SLIDER_HI)
            {
                switch (lpcd->dwDrawStage)
                {
                case CDDS_PREPAINT:
                {
                    SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                    return TRUE;
                }

                case CDDS_ITEMPREPAINT:
                {

                    if (lpcd->dwItemSpec == TBCD_THUMB)
                    {
                        HDC hdc = lpcd->hdc;
                        RECT rcThumb = lpcd->rc;

                        // Увеличиваем ширину бегунка
                        int extra = 5;
                        rcThumb.left -= extra;
                        rcThumb.right += extra;
                        rcThumb.top -= 5;

                        HWND hTrackBar = lpcd->hdr.hwndFrom;
                        int curVal = (int)SendMessage(hTrackBar, TBM_GETPOS, 0, 0);

                        //бегунок не рисуем - только значки из шрифта
                                            
                        LOGFONT lf; HFONT hVolumeIconFont = NULL; HFONT hOldFont = NULL;
                        if (GetObject(hButtonFont, sizeof(LOGFONT), &lf))
                        {
                            lf.lfHeight = -MulDiv(14, GetDeviceCaps(hdc, LOGPIXELSY), 72); // 16pt → пиксели
                            hVolumeIconFont = CreateFontIndirect(&lf);
                        }
                        if (hVolumeIconFont)
                        {
                            hOldFont = (HFONT)SelectObject(hdc, hVolumeIconFont);

                            SetBkMode(hdc, TRANSPARENT);

                            // ===== иконка =====
                            if (lpcd->hdr.idFrom == IDC_SLIDER_VOL)
                            {
                                DrawTextW(hdc, L" \ue994", -1, &rcThumb, DT_CENTER | DT_VCENTER | DT_SINGLELINE); //
                            }
                            else if (lpcd->hdr.idFrom == IDC_SLIDER_HI)
                            {
                                DrawTextW(hdc, L"\uf090", -1, &rcThumb, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                            }
                            else if (lpcd->hdr.idFrom == IDC_SLIDER_BASS) {
                                DrawTextW(hdc, L"\uf08e", -1, &rcThumb, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                            }

                            SelectObject(hdc, hOldFont);
                            DeleteObject(hVolumeIconFont);
                        }
                        // ===== значение =====
                        wchar_t buf[32];
                        wsprintf(buf, L"%d", curVal);

                        if (hTrackBar != g_hSliderVolume) {
                            if (curVal < 0)
                                wsprintf(buf, L"-%d", curVal);
                            else if (curVal == 0)
                                wsprintf(buf, L"%d", curVal);
                            else if (curVal > 0)
                                wsprintf(buf, L"+%d", curVal);
                        }
                      
                        SetBkMode(hdc, TRANSPARENT);
                        // Цвет текста под фон бегунка
                        SetTextColor(hdc, RGB(20, 20, 20));

                        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                        HFONT oldFont = (HFONT)SelectObject(hdc, hFont);

                        if (rcThumb.left > 40)
                            rcThumb.left -= 40;
                        else
                            rcThumb.left += 40;

                        DrawText(
                            hdc,
                            buf,
                            -1,
                            &rcThumb,
                            DT_CENTER | DT_VCENTER | DT_SINGLELINE
                        );

                        SelectObject(hdc, oldFont);

                        SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                        return TRUE;
                    }
                    // =======================
                    // РИСУЕМ ПОЛОСКУ
                    // =======================
                    if (lpcd->dwItemSpec == TBCD_CHANNEL)
                    {
                        DrawStyledSliderChannel(lpcd);

                        SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                        return TRUE;
                    }

                    // Всё остальное (tics) не даём системе рисовать
                    SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                    return TRUE;
                }
                }
            }
            if (lpcd->hdr.idFrom == IDC_LIST_URL)
            {
                LPNMLVCUSTOMDRAW lplv = (LPNMLVCUSTOMDRAW)lParam;

                switch (lplv->nmcd.dwDrawStage)
                {
                case CDDS_PREPAINT:
                {
                    // Сообщаем, что хотим получать уведомления для элементов
                    SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                    return TRUE;
                }
                case CDDS_ITEMPREPAINT:
                {
                    lplv->clrText = RGB(32, 32, 32);
                    // Set alternating row colors
                    if ((lplv->nmcd.dwItemSpec % 2) == 0) {
                        lplv->clrTextBk = RGB(255, 255, 255);
                    }
                    else {
                        lplv->clrTextBk = RGB(240, 240, 240);
                    }
                    
                    HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);
                    int itemIndex = (int)lplv->nmcd.dwItemSpec;

                    // Получаем состояние элемента напрямую из ListView
                    UINT itemState = ListView_GetItemState(
                        hListView,
                        itemIndex,
                        LVIS_SELECTED | LVIS_FOCUSED
                    );

                    BOOL isSelected = (itemState & LVIS_SELECTED) != 0;
                    BOOL isWindowFocused = (GetFocus() == hListView);

                    if (isSelected)
                    {
                        if (isWindowFocused)
                        {
                            // Активное выделение (окно в фокусе)
                            lplv->clrTextBk = RGB(200, 230, 245); 
                            lplv->clrText = RGB(33, 33, 33); 
                        }
                        else
                        {
                            // Неактивное выделение (окно без фокуса)
                            lplv->clrTextBk = GetSysColor(COLOR_BTNFACE); // Системный цвет неактивного выделения
                            lplv->clrText = GetSysColor(COLOR_WINDOWTEXT);
                        }

                        //отключаем стандартную заливку системным цветом
                        lplv->nmcd.uItemState &= ~CDIS_SELECTED;
                    }

                    SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NEWFONT);
                    return TRUE;
                }
                }
            }

        }
        if (lpnm->code == NM_DBLCLK && lpnm->idFrom == IDC_LIST_URL)
        {
            LPNMITEMACTIVATE lpnmia = (LPNMITEMACTIVATE)lParam;
            if (lpnmia->iItem != -1)
            {
                PlayAtIndex(lpnmia->iItem);
                
            }
        }

        SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_DODEFAULT);
        return TRUE;
       
    }
    break;
    case WM_APP_PLAYBACK_ERROR:
    {
        unsigned long messageGeneration = static_cast<unsigned long>(wParam);
        if (messageGeneration != g_playbackGeneration.load()) {
			//LogToUI("Received outdated playback error message, ignoring.");
            return (INT_PTR)TRUE;
        }

        if (g_is_recording.load()){ 
            g_rec_semafor.store(false);
            // находится в file_recording.cpp
            StopRecording();
            HWND hRec = GetDlgItem(hDlg, IDC_BUTTON_REC);
            if (hRec) {
                InvalidateRect(hRec, NULL, TRUE); 
                UpdateWindow(hRec);  
            }
        }
        //перезагрузка
        int current_trying = 0;
        if ((int)lParam > 0) {
            if (reconnect_attempts < MAX_RECONNECT_ATTEMPTS) {
                current_trying = ++reconnect_attempts;
            }
            else {
                current_trying = 0;
            }
        }

        StopPlayback(current_trying <= 0);
        //LogToUI("Playback error occurred. Attempting to reconnect... Try #" + std::to_string(current_trying));
        if (current_trying > 0) {
            PostFfmpegStatus(
                TrString("status.reconnect_attempt_prefix", L"Переподключение... попытка ") +
                std::to_wstring(current_trying) +
                TrString("status.reconnect_attempt_middle", L" из ") +
                std::to_wstring(MAX_RECONNECT_ATTEMPTS));

            HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);
            int selected = ListView_GetSelectionMark(hListView);
            if (g_currentlyPlayingIndex != -1) {
                selected = g_currentlyPlayingIndex;
            }
            else {
                selected = 0; // Default to first item
            }

            PlayAtIndex(selected, false);
        }
        else {
            reconnect_attempts = 0;
            PostFfmpegStatus(TrString("status.stream_unavailable_timeout", L"Поток недоступен / таймаут подключения"));
            UpdatePlayPauseButtonIcon(hDlg);
        }
        return (INT_PTR)TRUE;
    }
    break;
    case WM_APP_ELAPSED_TIME:
    {
        int total_sec = static_cast<int>(wParam);
        if (total_sec <= 0) {
            g_nowPlayingElapsed.clear();
            g_limiterRiderStatus.clear();
            g_lufsNormalizerStatus.clear();
            if (!running.load()) {
                g_nowPlayingStreamInfo.clear();
            }
        }
        else {
            int h = total_sec / 3600;
            int m = (total_sec % 3600) / 60;
            int s = total_sec % 60;

            wchar_t time_buf[16];
            if (h > 0) {
                swprintf_s(time_buf, L"%d:%02d:%02d", h, m, s);
            }
            else {
                swprintf_s(time_buf, L"%02d:%02d", m, s);
            }

            g_nowPlayingElapsed = time_buf;
            if (running.load() &&
                !g_suppressFfmpegDecoderLog.load() &&
                g_audioStreamInfoAllowed.load() &&
                !g_nowPlayingStreamInfo.empty() &&
                IsTransientPlaybackStatus(g_nowPlayingStatus)) {
                g_nowPlayingStatus.clear();
            }
        }

        InvalidateNowPlayingBar(hDlg);
        return 0;
    }

    case WM_APP_TRAY_ICON:
    {
        switch (LOWORD(lParam))
        {
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case WM_LBUTTONDBLCLK:
            RestoreMainWindow(hDlg);
            return (INT_PTR)TRUE;

        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayContextMenu(hDlg);
            return (INT_PTR)TRUE;
        }
        break;
    }

    case WM_APP_RESTORE_FROM_SINGLE_INSTANCE:
        RestoreMainWindow(hDlg);
        return (INT_PTR)TRUE;

    case WM_CLOSE:
        RequestApplicationExit(hDlg);
        return (INT_PTR)TRUE;

    case WM_DESTROY:
    {
        RemoveTrayIcon(hDlg);
        if (g_hTrackToast) {
            DestroyWindow(g_hTrackToast);
            g_hTrackToast = nullptr;
        }

        HWND hListView = GetDlgItem(hDlg, IDC_LIST_URL);
        int itemCount = ListView_GetItemCount(hListView);
        std::vector<PlaylistItem> currentPlaylist;
        currentPlaylist.reserve(itemCount);

        for (int i = 0; i < itemCount; ++i) {
            if (i < static_cast<int>(playlist.size())) {
                PlaylistItem item;
                item.name = playlist[i].name;
                item.url = playlist[i].url;
                item.disable_name_icy = playlist[i].disable_name_icy;
                currentPlaylist.push_back(item);
            }
        }

        savePlaylistToDat(L"app.dat", currentPlaylist, g_currentlyPlayingIndex);

        StopPlaylistNameResolveThread();

        if (hButtonFont) DeleteObject(hButtonFont);
        if (hListboxFont) DeleteObject(hListboxFont);
        if (hNowPlayingTitleFont) DeleteObject(hNowPlayingTitleFont);
        if (g_hbrBlack)  DeleteObject(g_hbrBlack);

        if (g_hSettingStatic) {
            RemoveWindowSubclass(g_hSettingStatic, SettingStaticSubclassProc, 0);
            g_hSettingStatic = NULL;
        }

        StopMetadataTimer();

        if (g_is_recording.load())
        {
            g_rec_semafor.store(false);
            // находится в file_recording.cpp
            StopRecording();
        }

        StopPlayback();

        if (g_playbackControlThread.joinable()) {
            g_playbackControlThread.join();
        }
         
        cleanupSDL();

        g_stopImageThread.store(true);
        if (g_imageUrlThread.joinable()) {
            g_imageUrlThread.join();
        }

        // находится в cover_art.cpp
        SaveCoverCacheIndex();
                          
        DeleteCriticalSection(&g_url_vec_cs);

        EndDialog(hDlg, 0);

        break;
    }
    }
    return (INT_PTR)FALSE;
}

void PlaybackControlFunction(std::string url) {
    // Новый запрос сразу делает старые ошибки/повторы неактуальными.
    unsigned long playbackGeneration = g_playbackGeneration.fetch_add(1) + 1;

    StopPlayback();

    if (playbackGeneration != g_playbackGeneration.load()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_ffmpegStatusMutex);
        g_lastFfmpegStatusRaw.clear();
        g_lastFfmpegLogRaw.clear();
    }
    g_audioStreamInfoAllowed.store(false);

    if (url.empty()) {
        return; // мало ли?
    }
	//глобальное url - на всякий случай.
    g_currentUrl = url;

    running.store(true);
    g_quit_flag.store(false);

    g_playbackThread = std::thread([url_str = std::string(url), playbackGeneration]() {

		g_playbackThreadRunning.store(true);
        struct PlaybackThreadRunningGuard {
            ~PlaybackThreadRunningGuard() {
                g_playbackThreadRunning.store(false);
            }
        } playbackThreadRunningGuard;

        if (playbackGeneration != g_playbackGeneration.load() || g_quit_flag.load()) {
            running.store(false);
            return;
        }

        const char* radioUrl = url_str.c_str();
        std::wstring statusUrl = utf8_to_wstring(url_str);
        if (statusUrl.size() > 96) {
            statusUrl = statusUrl.substr(0, 96) + L"...";
        }
        if (reconnect_attempts > 0) {
            PostFfmpegStatus(
                TrString("status.reconnect_url_prefix", L"Переподключение к URL (") +
                statusUrl +
                TrString("status.reconnect_url_middle", L"), попытка ") +
                std::to_wstring(reconnect_attempts) +
                TrString("status.reconnect_attempt_middle", L" из ") +
                std::to_wstring(MAX_RECONNECT_ATTEMPTS));
        }
        else {
            PostFfmpegStatus(
                TrString("status.connect_url_prefix", L"Подключение к URL (") +
                statusUrl +
                TrString("status.connect_url_suffix", L")"));
        }

        formatCtx = avformat_alloc_context();
        if (!formatCtx) {
            LogToUI("FFmpeg open: avformat_alloc_context failed");
            PostFfmpegStatus(TrString("status.avformat_alloc_error", L"FFmpeg: ошибка avformat_alloc_context()"));
            PostMessage(g_hMainWnd, WM_APP_PLAYBACK_ERROR, (WPARAM)playbackGeneration, 0);
            return;
        }

        formatCtx->interrupt_callback.callback = interrupt_callback;
        formatCtx->interrupt_callback.opaque = &g_quit_flag;

        AVDictionary* options = nullptr;

        // 1. Заголовки и User-Agent 
        av_dict_set(&options, "user_agent", "WinAmp/5.0", 0);
        av_dict_set(&options, "headers", "Icy-MetaData: 1\r\n", 0);

        // 2. Таймаут установки соединения (в микросекундах). 
        // 5000000 = 5 секунд. Даем время на подключение к серверу.
        av_dict_set(&options, "timeout", "5000000", 0);

        // 3. Таймаут чтения/записи (очень важно для радио!). 
        // Если сервер перестанет присылать данные (зависнет), FFmpeg через 10 секунд выдаст ошибку.
        av_dict_set(&options, "rw_timeout", "10000000", 0);

        // 4. Размер буфера. Для высокоскоростного (например, 320kbps MP3 или FLAC) 
        // потока 64KB маловато. Ставим 1MB, чтобы сглаживать скачки сети.
        av_dict_set(&options, "buffer_size", "1048576", 0); // 1024 KB

        // 5. Ускорение старта (Fast Start). Радио — это live-поток, нам не нужно анализировать 
        // несколько секунд, чтобы понять формат. Ограничиваем время анализа 1-2 секундами.
        av_dict_set(&options, "analyzeduration", "1500000", 0); // 1.5 сек

        // Не включаем внутренний FFmpeg reconnect: после смены VPN/маршрута
        // старый AVFormatContext может продолжать отдавать рваный поток.
        // Переподключением управляет внешний путь WM_APP_PLAYBACK_ERROR.

        PostFfmpegStatus(TrString("status.reading_stream_headers", L"Чтение заголовков потока..."));

        int openRet = avformat_open_input(&formatCtx, radioUrl, nullptr, &options);
        if (openRet < 0) {
            LogToUI("FFmpeg open: avformat_open_input failed: " + av_error_string(openRet) +
                " (" + std::to_string(openRet) + "), url=" + url_str);
            av_dict_free(&options);

            if (reconnect_attempts < MAX_RECONNECT_ATTEMPTS) {
                PostFfmpegStatus(TrString("status.ffmpeg_timeout", L"FFmpeg: таймаут сети / ожидание переподключения"));

                for (int i = 0; i < 50 && !g_quit_flag.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (g_quit_flag.load() || playbackGeneration != g_playbackGeneration.load()) {
                    return;
                }
                PostMessage(g_hMainWnd, WM_APP_PLAYBACK_ERROR, (WPARAM)playbackGeneration, 1);
            }
            else {
                reconnect_attempts = 0;
                PostFfmpegStatus(TrString("status.connection_attempts_exceeded", L"Поток недоступен: превышено число попыток подключения"));
                PostMessage(g_hMainWnd, WM_APP_PLAYBACK_ERROR, (WPARAM)playbackGeneration, 0);
            }
            return;
        }

        formatCtx->flags |= AVFMT_FLAG_NONBLOCK;
        av_dict_free(&options);

        PostFfmpegStatus(TrString("status.analyzing_stream", L"Анализ потока и определение формата..."));
        int streamInfoRet = avformat_find_stream_info(formatCtx, nullptr);
        if (streamInfoRet < 0) {
            LogToUI("FFmpeg open: avformat_find_stream_info failed: " +
                av_error_string(streamInfoRet) + " (" + std::to_string(streamInfoRet) + ")");
            avformat_close_input(&formatCtx);
            PostFfmpegStatus(TrString("status.stream_header_read_error", L"Ошибка чтения заголовков потока"));
            PostMessage(g_hMainWnd, WM_APP_PLAYBACK_ERROR, (WPARAM)playbackGeneration, 0);
            return;
        }

        if (formatCtx->iformat && formatCtx->iformat->name) {
            PostFfmpegStatus(TrString("status.demuxer_prefix", L"Используемый демультиплексер: ") + utf8_to_wstring(formatCtx->iformat->name));
        }

        int audioStreamIndex = av_find_best_stream(formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audioStreamIndex < 0) {
            LogToUI("FFmpeg open: av_find_best_stream(audio) failed: " +
                av_error_string(audioStreamIndex) + " (" + std::to_string(audioStreamIndex) + ")");
            avformat_close_input(&formatCtx);
            PostFfmpegStatus(TrString("status.audio_stream_not_found", L"Не найден аудиопоток"));
            PostMessage(g_hMainWnd, WM_APP_PLAYBACK_ERROR, (WPARAM)playbackGeneration, 0);
            return;
        }

        const AVCodecParameters* audioCodecPar = formatCtx->streams[audioStreamIndex]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(audioCodecPar->codec_id);
        if (!codec) {
            LogToUI("FFmpeg open: avcodec_find_decoder failed for codec_id=" +
                std::to_string(audioCodecPar->codec_id));
            avformat_close_input(&formatCtx);
            PostFfmpegStatus(TrString("status.audio_stream_not_found", L"Не найден аудиопоток"));
            PostMessage(g_hMainWnd, WM_APP_PLAYBACK_ERROR, (WPARAM)playbackGeneration, 0);
            return;
        }
        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            LogToUI("FFmpeg open: avcodec_alloc_context3 failed");
            avformat_close_input(&formatCtx);
            PostMessage(g_hMainWnd, WM_APP_PLAYBACK_ERROR, (WPARAM)playbackGeneration, 0);
            return;
        }

        int codecParamRet = avcodec_parameters_to_context(codecCtx, formatCtx->streams[audioStreamIndex]->codecpar);
        if (codecParamRet < 0) {
            LogToUI("FFmpeg open: avcodec_parameters_to_context failed: " +
                av_error_string(codecParamRet) + " (" + std::to_string(codecParamRet) + ")");
            avcodec_free_context(&codecCtx);
            avformat_close_input(&formatCtx);
            PostMessage(g_hMainWnd, WM_APP_PLAYBACK_ERROR, (WPARAM)playbackGeneration, 0);
            return;
        }

        int codecOpenRet = avcodec_open2(codecCtx, codec, nullptr);
        if (codecOpenRet < 0) {
            LogToUI("FFmpeg open: avcodec_open2 failed: " +
                av_error_string(codecOpenRet) + " (" + std::to_string(codecOpenRet) + ")");
            avcodec_free_context(&codecCtx);
            avformat_close_input(&formatCtx);
            PostMessage(g_hMainWnd, WM_APP_PLAYBACK_ERROR, (WPARAM)playbackGeneration, 0);
            return;
        }

        IAudioClient* audioClient = nullptr;
        IAudioRenderClient* renderClient = nullptr;
        WAVEFORMATEX* pwfx = nullptr;
        UINT32 bufferFrameCount = 0;

        if (FAILED(InitWASAPI(audioClient, renderClient, pwfx, bufferFrameCount))) {
            PostFfmpegStatus(TrString("status.audio_output_init_error", L"Ошибка инициализации аудиовывода"));
            avcodec_free_context(&codecCtx);
            avformat_close_input(&formatCtx);
            PostMessage(g_hMainWnd, WM_APP_PLAYBACK_ERROR, (WPARAM)playbackGeneration, 0);
            return;
        }


        SwrContext* swr = InitSwResample(codecCtx, pwfx);
        if (!swr && !CanBypassSwResample(codecCtx, pwfx)) {
            LogToUI("FFmpeg open: InitSwResample failed and bypass is not possible");
            PostFfmpegStatus(TrString("status.resampler_init_error", L"Ошибка инициализации ресемплера"));
            CoTaskMemFree(pwfx);
            if (renderClient) renderClient->Release();
            if (audioClient) {

                audioClient->Release();
            }
            avcodec_free_context(&codecCtx);
            avformat_close_input(&formatCtx);
            PostMessage(g_hMainWnd, WM_APP_PLAYBACK_ERROR, (WPARAM)playbackGeneration, 0);
            return;
        }

        filterGraph = avfilter_graph_alloc();

        AVChannelLayout filter_layout = {};

        if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* wfex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
            if (av_channel_layout_from_mask(&filter_layout, wfex->dwChannelMask) < 0) {
                av_channel_layout_default(&filter_layout, pwfx->nChannels);
            }
        }
        else {
            av_channel_layout_default(&filter_layout, pwfx->nChannels);
        }

        uint64_t channel_mask = 0;
        if (filter_layout.order == AV_CHANNEL_ORDER_NATIVE) {
            channel_mask = filter_layout.u.mask;
        }

        if (channel_mask == 0) {
            av_channel_layout_uninit(&filter_layout);
            av_channel_layout_default(&filter_layout, pwfx->nChannels);
            channel_mask = filter_layout.u.mask;
        }

        filter_abuf = nullptr;
        filter_aeq = nullptr;
        filter_avol = nullptr;
        filter_asink = nullptr;

        int filterRet = init_audio_filter_graph(filterGraph, AV_SAMPLE_FMT_FLTP, pwfx->nSamplesPerSec, channel_mask, current_volume.load(), &filter_abuf, &filter_aeq, &filter_avol, &filter_asink);
        if (filterRet < 0) {
            LogToUI("FFmpeg open: init_audio_filter_graph failed: " +
                av_error_string(filterRet) + " (" + std::to_string(filterRet) + ")");
            avfilter_graph_free(&filterGraph);
            filterGraph = nullptr;
            filter_abuf = nullptr;
            filter_aeq = nullptr;
            filter_avol = nullptr;
            filter_asink = nullptr;
        }
        av_channel_layout_uninit(&filter_layout);

        audioClient->Start();

        StartMetadataTimer();
        update_stream_metadata();

        InitShowCQT();

        PlaybackLoop(formatCtx, codecCtx, audioStreamIndex, audioClient, renderClient, bufferFrameCount, pwfx, swr, radioUrl, playbackGeneration);

        CleanupShowCQT();

        swr_free(&swr);

        });
    

}

void StartPlaybackThread(const char* url) {
    if (!url || strlen(url) == 0) return;

    {
        std::lock_guard<std::mutex> lock(ControlVectorMutex);
        g_controlVector.push_back(std::string(url));

        if (g_controlThreadRunning.load()) {
            return;
        }

        g_controlThreadRunning.store(true);
    }
   
    if (g_playbackControlThread.joinable()) {
        g_playbackControlThread.join();
    }

    g_playbackControlThread = std::thread([]() {

        for (;;) {
            std::string lastUrl;

            {
                std::lock_guard<std::mutex> lock(ControlVectorMutex);
                if (g_controlVector.empty()) {
                    g_controlThreadRunning.store(false);
                    return;
                }

                lastUrl = std::move(g_controlVector.back());
                g_controlVector.clear();
            }

            if (!lastUrl.empty()) {
                PlaybackControlFunction(lastUrl);
            }
        }

        });
}


void StopPlayback(bool resetDisplayedBitrate) {
    std::lock_guard<std::mutex> lock(g_stopPlaybackMutex);

    const bool hasPlaybackState =
        running.load() ||
        g_playbackThread.joinable() ||
        codecCtx != nullptr ||
        formatCtx != nullptr ||
        filterGraph != nullptr;

    if (!hasPlaybackState) {
        return;
    }

    g_quit_flag.store(true);
    running.store(false);

    PostMessageW(g_hMainWnd, WM_APP_ELAPSED_TIME, 0, 0);

    PostFfmpegStatus(TrString("status.stopped", L"Остановлено"));

    StopMetadataTimer();
    ResetMetadataCaches(resetDisplayedBitrate);

    if (g_playbackThread.joinable()) {
        if (g_playbackThread.get_id() != std::this_thread::get_id()) {
            g_playbackThread.join();
        }
    }

    if (codecCtx) {
        avcodec_free_context(&codecCtx);
        codecCtx = nullptr;
    }
    if (formatCtx) {
        avformat_close_input(&formatCtx);
        formatCtx = nullptr;
    }
    if (filterGraph) {
        avfilter_graph_free(&filterGraph);
        filterGraph = nullptr;
    }

    g_playbackThreadRunning.store(false);
}

static void StopPlaybackAsync() {
    g_playbackGeneration.fetch_add(1);
    g_quit_flag.store(true);
    running.store(false);

    PostMessageW(g_hMainWnd, WM_APP_ELAPSED_TIME, 0, 0);
    PostFfmpegStatus(TrString("status.stopped", L"Остановлено"));

    std::thread([]() {
        StopPlayback();
    }).detach();
}
