#pragma once

#include <windows.h>
#include <string>

constexpr UINT IDT_COMPACT_TITLE_SCROLL = 7;

struct CompactModeCallbacks {
    std::wstring (*getTitleText)() = nullptr;
    void (*refreshTooltips)(HWND hDlg) = nullptr;
    void (*invalidateNormalText)(HWND hDlg) = nullptr;
    void (*showContextMenu)(HWND hDlg, POINT pt) = nullptr;
};

void CompactModeConfigure(
    HWND hDlg,
    HWND hSpectrumHost,
    HFONT hTitleFont,
    HFONT hIconFont,
    const CompactModeCallbacks& callbacks);

bool CompactModeIsActive();
void CompactModeEnter(HWND hDlg);
void CompactModeExit(HWND hDlg);
void CompactModeSetAlwaysOnTop(HWND hDlg, bool enabled);
void CompactModeApplyZOrder(HWND hDlg);
void CompactModeBeginDrag(HWND hDlg);
bool CompactModeHandleMouseMove(HWND hDlg);
void CompactModeEndDrag(HWND hDlg);
void CompactModeInstallCoverSubclass(HWND hDlg);
void CompactModeLayout(HWND hDlg);
void CompactModeAdvanceTitleScroll(HWND hDlg);
void CompactModeInvalidateTitle();
bool CompactModeHandleGetMinMaxInfo(HWND hDlg, LPARAM lParam);
bool CompactModeHandleNcHitTest(HWND hDlg, LPARAM lParam, INT_PTR& result);
bool CompactModeDrawItem(LPDRAWITEMSTRUCT pDIS);
void CompactModeDrawChrome(HWND hDlg, HDC hdc);
