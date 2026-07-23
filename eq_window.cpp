#include "eq_window.h"

#include "IRPFFmpeg.h"
#include "language_manager.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <windowsx.h>

#pragma comment(lib, "gdiplus.lib")

const float g_parametricEqFrequenciesHz[kParametricEqBandCount] = {
    30.0f, 200.0f, 2000.0f, 5000.0f, 10000.0f, 16000.0f
};

std::atomic<float> g_parametricEqGainDb[kParametricEqBandCount] = {
    2.0f, 0.0f, 0.0f, 0.0f, 3.0f, 10.0f
};

std::atomic<float> g_parametricEqQ[kParametricEqBandCount] = {
    0.80f, 1.00f, 1.00f, 1.00f, 1.00f, 0.80f
};

namespace
{
constexpr wchar_t kEqWindowClass[] = L"IRPFFmpegParametricEqWindow";
constexpr wchar_t kEqKnobClass[] = L"IRPFFmpegParametricEqKnob";

constexpr int kEqWindowWidth = 620;
constexpr int kEqWindowHeight = 458;
constexpr int kEqContentMargin = 8;
constexpr int kFrequencyTextY = 4;
constexpr int kFrequencyTextHeight = 18;
constexpr int kGainKnobY = 28;
constexpr int kGainKnobSize = 76;
constexpr int kGainTextY = 108;
constexpr int kQKnobY = 134;
constexpr int kQKnobSize = 48;
constexpr int kQTextY = 186;
constexpr float kMinGainDb = -16.0f;
constexpr float kMaxGainDb = 16.0f;
constexpr float kMinQ = 0.20f;
constexpr float kMaxQ = 4.0f;
constexpr float kGainStepDb = 0.5f;
constexpr float kQStep = 0.05f;
constexpr int kResponseGraphY = 212;
constexpr int kResponsePointCount = 192;
constexpr double kResponseMinFrequencyHz = 20.0;
constexpr double kResponseMaxFrequencyHz = 20000.0;
constexpr double kResponseSampleRateHz = 44100.0;
constexpr double kResponseMinDb = -18.0;
constexpr double kResponseMaxDb = 18.0;
constexpr double kPi = 3.14159265358979323846;

HWND g_hEqWindow = nullptr;
ULONG_PTR g_gdiplusToken = 0;
HICON g_hEqGlyphIconSmall = nullptr;
HICON g_hEqGlyphIconLarge = nullptr;

struct EqKnobState {
    int band = 0;
    bool controlsQ = false;
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float step = 0.1f;
    bool dragging = false;
    int dragStartY = 0;
    float dragStartValue = 0.0f;
};

float ClampFloat(float value, float minValue, float maxValue)
{
    return (std::max)(minValue, (std::min)(maxValue, value));
}

float SnapValue(float value, float step)
{
    if (step <= 0.0f) {
        return value;
    }
    return std::round(value / step) * step;
}

float GetKnobValue(const EqKnobState& state)
{
    if (state.controlsQ) {
        return g_parametricEqQ[state.band].load();
    }
    return g_parametricEqGainDb[state.band].load();
}

RECT GetBandColumnRect(HWND hwnd, int band)
{
    RECT rc = {};
    GetClientRect(hwnd, &rc);

    const int contentLeft = kEqContentMargin;
    const int contentRight = (std::max)(
        contentLeft + kParametricEqBandCount * 80,
        static_cast<int>(rc.right) - kEqContentMargin);
    const int contentWidth = contentRight - contentLeft;
    const int columnWidth = contentWidth / kParametricEqBandCount;
    const int left = contentLeft + band * columnWidth;
    const int right = (band == kParametricEqBandCount - 1)
        ? contentRight
        : left + columnWidth;

    return { left, 0, right, 0 };
}

int CenteredInRectX(const RECT& rect, int width)
{
    return rect.left + ((rect.right - rect.left) - width) / 2;
}

RECT GetBandValueRect(HWND hwnd, int band, bool qValue)
{
    RECT columnRect = GetBandColumnRect(hwnd, band);
    const int y = qValue ? kQTextY : kGainTextY;
    return { columnRect.left, y, columnRect.right, y + 22 };
}

RECT GetResponseGraphRect(HWND hwnd)
{
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    return {
        kEqContentMargin,
        kResponseGraphY,
        (std::max)(kEqContentMargin + 160, static_cast<int>(rc.right) - kEqContentMargin),
        (std::max)(kResponseGraphY + 80, static_cast<int>(rc.bottom) - kEqContentMargin)
    };
}

void SyncLegacyEqValues(int band, float gainDb)
{
    if (band == 0) {
        current_eq_gain_bass.store(gainDb);
    }
    else if (band == kParametricEqBandCount - 1) {
        current_eq_gain.store(gainDb);
    }
}

void SetKnobValue(HWND hwnd, EqKnobState& state, float value)
{
    value = SnapValue(ClampFloat(value, state.minValue, state.maxValue), state.step);
    const float previousValue = GetKnobValue(state);
    if (std::fabs(previousValue - value) < 0.0001f) {
        return;
    }

    if (state.controlsQ) {
        g_parametricEqQ[state.band].store(value);
    }
    else {
        g_parametricEqGainDb[state.band].store(value);
        SyncLegacyEqValues(state.band, value);
    }

    UpdateFilterSettings();
    InvalidateRect(hwnd, nullptr, FALSE);

    HWND parent = GetParent(hwnd);
    if (parent) {
        RECT valueRect = GetBandValueRect(parent, state.band, state.controlsQ);
        InvalidateRect(parent, &valueRect, FALSE);

        RECT responseRect = GetResponseGraphRect(parent);
        InvalidateRect(parent, &responseRect, FALSE);
    }
}

std::wstring FormatFrequency(float hz)
{
    wchar_t text[32] = {};
    if (hz >= 1000.0f) {
        const float khz = hz / 1000.0f;
        if (std::fabs(khz - std::round(khz)) < 0.01f) {
            swprintf_s(text, L"%.0f kHz", khz);
        }
        else {
            swprintf_s(text, L"%.1f kHz", khz);
        }
    }
    else {
        swprintf_s(text, L"%.0f Hz", hz);
    }
    return text;
}

std::wstring FormatGain(float gainDb)
{
    wchar_t text[32] = {};
    if (std::fabs(gainDb) < 0.05f) {
        gainDb = 0.0f;
    }
    swprintf_s(text, L"%+.1f dB", gainDb);
    return text;
}

std::wstring FormatQ(float q)
{
    wchar_t text[32] = {};
    swprintf_s(text, L"Q %.2f", q);
    return text;
}

std::wstring FormatDbTick(double db)
{
    wchar_t text[16] = {};
    if (std::fabs(db) < 0.05) {
        swprintf_s(text, L"0");
    }
    else {
        swprintf_s(text, L"%+.0f", db);
    }
    return text;
}

void EnsureGdiplusStarted()
{
    if (g_gdiplusToken != 0) {
        return;
    }

    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr);
}

HICON CreateEqualizerGlyphIcon(int size)
{
    EnsureGdiplusStarted();

    Gdiplus::Bitmap bitmap(size, size, PixelFormat32bppARGB);
    Gdiplus::Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

    Gdiplus::FontFamily iconFamily(L"Segoe Fluent Icons");
    const Gdiplus::REAL fontSize = static_cast<Gdiplus::REAL>(size) * 0.76f;
    Gdiplus::Font iconFont(&iconFamily, fontSize, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    Gdiplus::RectF rect(0.0f, 0.0f, static_cast<Gdiplus::REAL>(size), static_cast<Gdiplus::REAL>(size));
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, 24, 74, 124));
    graphics.DrawString(L"\uE9E9", -1, &iconFont, rect, &format, &brush);
    graphics.Flush(Gdiplus::FlushIntentionFlush);

    HICON icon = nullptr;
    bitmap.GetHICON(&icon);
    return icon;
}

HICON GetEqualizerGlyphIcon(int size)
{
    HICON& icon = size <= 16 ? g_hEqGlyphIconSmall : g_hEqGlyphIconLarge;
    if (!icon) {
        icon = CreateEqualizerGlyphIcon(size <= 16 ? 16 : 32);
    }
    return icon;
}

double EvaluatePeakingEqMagnitude(double frequencyHz, double centerHz, double q, double gainDb)
{
    q = ClampFloat(static_cast<float>(q), kMinQ, kMaxQ);

    if (std::fabs(gainDb) < 0.0001 || q <= 0.0) {
        return 1.0;
    }

    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * centerHz / kResponseSampleRateHz;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosW0 = std::cos(w0);

    const double a0Raw = 1.0 + alpha / A;
    const double b0 = (1.0 + alpha * A) / a0Raw;
    const double b1 = (-2.0 * cosW0) / a0Raw;
    const double b2 = (1.0 - alpha * A) / a0Raw;
    const double a1 = (-2.0 * cosW0) / a0Raw;
    const double a2 = (1.0 - alpha / A) / a0Raw;

    const double w = 2.0 * kPi * frequencyHz / kResponseSampleRateHz;
    const std::complex<double> z1(std::cos(w), -std::sin(w));
    const std::complex<double> z2(std::cos(2.0 * w), -std::sin(2.0 * w));

    const std::complex<double> numerator = b0 + b1 * z1 + b2 * z2;
    const std::complex<double> denominator = 1.0 + a1 * z1 + a2 * z2;
    const double denominatorMagnitude = std::abs(denominator);
    if (denominatorMagnitude < 1e-12) {
        return 1.0;
    }

    return std::abs(numerator / denominator);
}

double EvaluateTotalResponseDb(double frequencyHz)
{
    double magnitude = 1.0;
    for (int band = 0; band < kParametricEqBandCount; ++band) {
        magnitude *= EvaluatePeakingEqMagnitude(
            frequencyHz,
            g_parametricEqFrequenciesHz[band],
            g_parametricEqQ[band].load(),
            g_parametricEqGainDb[band].load());
    }

    return 20.0 * std::log10((std::max)(magnitude, 1e-9));
}

float ResponseFrequencyToX(double frequencyHz, const Gdiplus::RectF& plotRect)
{
    const double minLog = std::log10(kResponseMinFrequencyHz);
    const double maxLog = std::log10(kResponseMaxFrequencyHz);
    const double t = (std::log10(frequencyHz) - minLog) / (maxLog - minLog);
    return plotRect.X + static_cast<float>(ClampFloat(static_cast<float>(t), 0.0f, 1.0f)) * plotRect.Width;
}

float ResponseDbToY(double db, const Gdiplus::RectF& plotRect)
{
    const double clampedDb = (std::max)(kResponseMinDb, (std::min)(kResponseMaxDb, db));
    const double t = (clampedDb - kResponseMinDb) / (kResponseMaxDb - kResponseMinDb);
    return plotRect.Y + plotRect.Height - static_cast<float>(t) * plotRect.Height;
}

void PaintResponseGraph(HWND hwnd, HDC hdc)
{
    EnsureGdiplusStarted();

    RECT graphRc = GetResponseGraphRect(hwnd);
    if (graphRc.right - graphRc.left < 220 || graphRc.bottom - graphRc.top < 90) {
        return;
    }

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    const Gdiplus::RectF frame(
        static_cast<float>(graphRc.left),
        static_cast<float>(graphRc.top),
        static_cast<float>(graphRc.right - graphRc.left),
        static_cast<float>(graphRc.bottom - graphRc.top));
    const Gdiplus::RectF plotRect(
        frame.X + 42.0f,
        frame.Y + 12.0f,
        frame.Width - 54.0f,
        frame.Height - 36.0f);

    Gdiplus::SolidBrush panelBrush(Gdiplus::Color(255, 250, 252, 255));
    Gdiplus::Pen borderPen(Gdiplus::Color(255, 210, 219, 231), 1.0f);
    graphics.FillRectangle(&panelBrush, frame);
    graphics.DrawRectangle(&borderPen, frame);

    Gdiplus::FontFamily fontFamily(L"Segoe UI");
    Gdiplus::Font tickFont(&fontFamily, 10.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush tickBrush(Gdiplus::Color(255, 91, 105, 122));
    Gdiplus::StringFormat yFormat;
    yFormat.SetAlignment(Gdiplus::StringAlignmentFar);
    yFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::StringFormat xFormat;
    xFormat.SetAlignment(Gdiplus::StringAlignmentCenter);
    xFormat.SetLineAlignment(Gdiplus::StringAlignmentNear);
    Gdiplus::StringFormat xLeftFormat;
    xLeftFormat.SetAlignment(Gdiplus::StringAlignmentNear);
    xLeftFormat.SetLineAlignment(Gdiplus::StringAlignmentNear);
    Gdiplus::StringFormat xRightFormat;
    xRightFormat.SetAlignment(Gdiplus::StringAlignmentFar);
    xRightFormat.SetLineAlignment(Gdiplus::StringAlignmentNear);

    const double dbTicks[] = { -18.0, -9.0, 0.0, 9.0, 18.0 };
    for (double tickDb : dbTicks) {
        const float y = ResponseDbToY(tickDb, plotRect);
        Gdiplus::Pen gridPen(
            std::fabs(tickDb) < 0.05
                ? Gdiplus::Color(255, 169, 181, 197)
                : Gdiplus::Color(255, 226, 232, 240),
            std::fabs(tickDb) < 0.05 ? 1.4f : 1.0f);
        graphics.DrawLine(&gridPen, plotRect.X, y, plotRect.X + plotRect.Width, y);

        std::wstring label = FormatDbTick(tickDb);
        Gdiplus::RectF labelRect(frame.X + 2.0f, y - 8.0f, 34.0f, 16.0f);
        graphics.DrawString(label.c_str(), -1, &tickFont, labelRect, &yFormat, &tickBrush);
    }

    const double frequencyGrid[] = { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 };
    const double labeledFrequencies[] = { 20.0, 100.0, 1000.0, 5000.0, 20000.0 };

    for (double frequencyHz : frequencyGrid) {
        const float x = ResponseFrequencyToX(frequencyHz, plotRect);
        Gdiplus::Pen gridPen(Gdiplus::Color(255, 230, 235, 242), 1.0f);
        graphics.DrawLine(&gridPen, x, plotRect.Y, x, plotRect.Y + plotRect.Height);
    }

    for (double frequencyHz : labeledFrequencies) {
        const float x = ResponseFrequencyToX(frequencyHz, plotRect);
        std::wstring label = FormatFrequency(static_cast<float>(frequencyHz));
        Gdiplus::RectF labelRect(x - 32.0f, plotRect.Y + plotRect.Height + 3.0f, 64.0f, 16.0f);
        Gdiplus::StringFormat* labelFormat = &xFormat;
        if (frequencyHz <= kResponseMinFrequencyHz + 0.5) {
            labelRect.X = x;
            labelFormat = &xLeftFormat;
        }
        else if (frequencyHz >= kResponseMaxFrequencyHz - 0.5) {
            labelRect.X = x - labelRect.Width;
            labelFormat = &xRightFormat;
        }
        graphics.DrawString(label.c_str(), -1, &tickFont, labelRect, labelFormat, &tickBrush);
    }

    Gdiplus::Pen plotBorderPen(Gdiplus::Color(255, 194, 205, 219), 1.0f);
    graphics.DrawRectangle(&plotBorderPen, plotRect);

    std::vector<Gdiplus::PointF> responsePoints;
    responsePoints.reserve(kResponsePointCount);
    const double minLog = std::log10(kResponseMinFrequencyHz);
    const double maxLog = std::log10(kResponseMaxFrequencyHz);
    for (int i = 0; i < kResponsePointCount; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(kResponsePointCount - 1);
        const double frequencyHz = std::pow(10.0, minLog + (maxLog - minLog) * t);
        const double responseDb = EvaluateTotalResponseDb(frequencyHz);
        responsePoints.push_back({
            ResponseFrequencyToX(frequencyHz, plotRect),
            ResponseDbToY(responseDb, plotRect)
        });
    }

    if (responsePoints.size() >= 2) {
        Gdiplus::Pen shadowPen(Gdiplus::Color(70, 32, 58, 83), 3.4f);
        shadowPen.SetLineJoin(Gdiplus::LineJoinRound);
        graphics.DrawLines(&shadowPen, responsePoints.data(), static_cast<INT>(responsePoints.size()));

        Gdiplus::Pen responsePen(Gdiplus::Color(255, 43, 151, 216), 2.2f);
        responsePen.SetLineJoin(Gdiplus::LineJoinRound);
        graphics.DrawLines(&responsePen, responsePoints.data(), static_cast<INT>(responsePoints.size()));
    }
}

void PaintWindowBackground(HWND hwnd, HDC hdc)
{
    RECT rc = {};
    GetClientRect(hwnd, &rc);

    HBRUSH background = CreateSolidBrush(RGB(244, 247, 251));
    FillRect(hdc, &rc, background);
    DeleteObject(background);

    SetBkMode(hdc, TRANSPARENT);

    HFONT labelFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    HFONT oldFont = labelFont ? (HFONT)SelectObject(hdc, labelFont) : nullptr;
    SetTextColor(hdc, RGB(72, 86, 104));

    for (int band = 0; band < kParametricEqBandCount; ++band) {
        RECT columnRect = GetBandColumnRect(hwnd, band);
        RECT freqRect = {
            columnRect.left,
            kFrequencyTextY,
            columnRect.right,
            kFrequencyTextY + kFrequencyTextHeight
        };
        std::wstring frequency = FormatFrequency(g_parametricEqFrequenciesHz[band]);
        DrawTextW(hdc, frequency.c_str(), -1, &freqRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT gainRect = GetBandValueRect(hwnd, band, false);
        std::wstring gain = FormatGain(g_parametricEqGainDb[band].load());
        DrawTextW(hdc, gain.c_str(), -1, &gainRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT qRect = GetBandValueRect(hwnd, band, true);
        std::wstring q = FormatQ(g_parametricEqQ[band].load());
        DrawTextW(hdc, q.c_str(), -1, &qRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    PaintResponseGraph(hwnd, hdc);

    if (oldFont) {
        SelectObject(hdc, oldFont);
    }
    if (labelFont) {
        DeleteObject(labelFont);
    }
}

void PaintEqWindowBuffered(HWND hwnd, HDC targetDc)
{
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HDC bufferDc = CreateCompatibleDC(targetDc);
    HBITMAP bufferBitmap = bufferDc
        ? CreateDIBSection(targetDc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0)
        : nullptr;
    if (!bufferDc || !bufferBitmap) {
        if (bufferBitmap) {
            DeleteObject(bufferBitmap);
        }
        if (bufferDc) {
            DeleteDC(bufferDc);
        }
        PaintWindowBackground(hwnd, targetDc);
        return;
    }

    HGDIOBJ oldBitmap = SelectObject(bufferDc, bufferBitmap);
    PaintWindowBackground(hwnd, bufferDc);
    BitBlt(targetDc, 0, 0, width, height, bufferDc, 0, 0, SRCCOPY);
    SelectObject(bufferDc, oldBitmap);
    DeleteObject(bufferBitmap);
    DeleteDC(bufferDc);
}

void DrawKnob(HWND hwnd, HDC hdc, const EqKnobState& state, bool hot)
{
    EnsureGdiplusStarted();

    RECT rc = {};
    GetClientRect(hwnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int size = (std::min)(width, height);
    const float value = GetKnobValue(state);
    const float normalized = (value - state.minValue) / (state.maxValue - state.minValue);
    const float sweep = ClampFloat(normalized, 0.0f, 1.0f) * 270.0f;

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    Gdiplus::SolidBrush back(Gdiplus::Color(255, 244, 247, 251));
    graphics.FillRectangle(&back, 0, 0, width, height);

    const float margin = state.controlsQ ? 5.0f : 6.0f;
    const float diameter = static_cast<float>(size) - margin * 2.0f - 2.0f;
    const float left = (width - diameter) / 2.0f;
    const float top = (height - diameter) / 2.0f;
    Gdiplus::RectF knobRect(left, top, diameter, diameter);

    Gdiplus::SolidBrush shadow1(Gdiplus::Color(42, 55, 76, 105));
    Gdiplus::SolidBrush shadow2(Gdiplus::Color(25, 255, 255, 255));
    Gdiplus::RectF shadowRect1 = knobRect;
    shadowRect1.Offset(1.4f, 2.0f);
    graphics.FillEllipse(&shadow1, shadowRect1);
    Gdiplus::RectF shadowRect2 = knobRect;
    shadowRect2.Offset(-1.0f, -1.0f);
    graphics.FillEllipse(&shadow2, shadowRect2);

    Gdiplus::LinearGradientBrush faceBrush(
        knobRect,
        Gdiplus::Color(255, 252, 254, 255),
        Gdiplus::Color(255, 222, 229, 238),
        Gdiplus::LinearGradientModeForwardDiagonal);
    graphics.FillEllipse(&faceBrush, knobRect);

    Gdiplus::Pen outline(Gdiplus::Color(255, 171, 184, 201), 1.0f);
    graphics.DrawEllipse(&outline, knobRect);

    const float ringInset = state.controlsQ ? 6.5f : 8.0f;
    Gdiplus::RectF ringRect = knobRect;
    ringRect.Inflate(-ringInset, -ringInset);

    Gdiplus::Pen baseRing(Gdiplus::Color(255, 207, 216, 227), state.controlsQ ? 3.4f : 4.8f);
    baseRing.SetStartCap(Gdiplus::LineCapRound);
    baseRing.SetEndCap(Gdiplus::LineCapRound);
    graphics.DrawArc(&baseRing, ringRect, 135.0f, 270.0f);

    Gdiplus::Color accent = state.controlsQ
        ? Gdiplus::Color(255, 126, 164, 212)
        : Gdiplus::Color(255, 79, 178, 224);
    if (hot) {
        accent = state.controlsQ
            ? Gdiplus::Color(255, 104, 146, 202)
            : Gdiplus::Color(255, 49, 156, 216);
    }

    Gdiplus::Pen activeRing(accent, state.controlsQ ? 3.4f : 4.8f);
    activeRing.SetStartCap(Gdiplus::LineCapRound);
    activeRing.SetEndCap(Gdiplus::LineCapRound);
    graphics.DrawArc(&activeRing, ringRect, 135.0f, sweep);

    const float angle = (135.0f + sweep) * 3.1415926535f / 180.0f;
    const float centerX = knobRect.X + knobRect.Width / 2.0f;
    const float centerY = knobRect.Y + knobRect.Height / 2.0f;
    const float markerRadius = ringRect.Width / 2.0f;
    const float markerSize = state.controlsQ ? 5.0f : 7.0f;
    const float markerX = centerX + std::cos(angle) * markerRadius;
    const float markerY = centerY + std::sin(angle) * markerRadius;

    Gdiplus::SolidBrush markerBrush(accent);
    graphics.FillEllipse(&markerBrush,
        markerX - markerSize / 2.0f,
        markerY - markerSize / 2.0f,
        markerSize,
        markerSize);

    if (GetFocus() == hwnd) {
        Gdiplus::Pen focusPen(Gdiplus::Color(180, 76, 132, 190), 1.2f);
        focusPen.SetDashStyle(Gdiplus::DashStyleDash);
        Gdiplus::RectF focusRect = knobRect;
        focusRect.Inflate(2.0f, 2.0f);
        graphics.DrawEllipse(&focusPen, focusRect);
    }
}

void PaintKnobBuffered(HWND hwnd, HDC targetDc, const EqKnobState& state, bool hot)
{
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HDC bufferDc = CreateCompatibleDC(targetDc);
    HBITMAP bufferBitmap = bufferDc
        ? CreateDIBSection(targetDc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0)
        : nullptr;
    if (!bufferDc || !bufferBitmap) {
        if (bufferBitmap) {
            DeleteObject(bufferBitmap);
        }
        if (bufferDc) {
            DeleteDC(bufferDc);
        }
        DrawKnob(hwnd, targetDc, state, hot);
        return;
    }

    HGDIOBJ oldBitmap = SelectObject(bufferDc, bufferBitmap);
    HBRUSH background = CreateSolidBrush(RGB(244, 247, 251));
    RECT fillRect = { 0, 0, width, height };
    FillRect(bufferDc, &fillRect, background);
    DeleteObject(background);

    DrawKnob(hwnd, bufferDc, state, hot);
    BitBlt(targetDc, 0, 0, width, height, bufferDc, 0, 0, SRCCOPY);
    SelectObject(bufferDc, oldBitmap);
    DeleteObject(bufferBitmap);
    DeleteDC(bufferDc);
}

void RegisterEqClasses(HINSTANCE instance)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;

    if (!GetClassInfoW(instance, kEqWindowClass, &wc)) {
        wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
            switch (msg) {
            case WM_CREATE:
            {
                for (int band = 0; band < kParametricEqBandCount; ++band) {
                    RECT columnRect = GetBandColumnRect(hwnd, band);
                    auto* gainState = new EqKnobState;
                    gainState->band = band;
                    gainState->controlsQ = false;
                    gainState->minValue = kMinGainDb;
                    gainState->maxValue = kMaxGainDb;
                    gainState->step = kGainStepDb;

                    CreateWindowExW(0, kEqKnobClass, L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        CenteredInRectX(columnRect, kGainKnobSize), kGainKnobY, kGainKnobSize, kGainKnobSize,
                        hwnd, nullptr, GetModuleHandleW(nullptr), gainState);

                    auto* qState = new EqKnobState;
                    qState->band = band;
                    qState->controlsQ = true;
                    qState->minValue = kMinQ;
                    qState->maxValue = kMaxQ;
                    qState->step = kQStep;

                    CreateWindowExW(0, kEqKnobClass, L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        CenteredInRectX(columnRect, kQKnobSize), kQKnobY, kQKnobSize, kQKnobSize,
                        hwnd, nullptr, GetModuleHandleW(nullptr), qState);
                }
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT ps = {};
                HDC hdc = BeginPaint(hwnd, &ps);
                PaintEqWindowBuffered(hwnd, hdc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_CLOSE:
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            case WM_DESTROY:
                if (g_hEqWindow == hwnd) {
                    g_hEqWindow = nullptr;
                }
                return 0;
            default:
                break;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
            };
        wc.lpszClassName = kEqWindowClass;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.hIcon = GetEqualizerGlyphIcon(32);
        RegisterClassW(&wc);
    }

    if (!GetClassInfoW(instance, kEqKnobClass, &wc)) {
        wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
            auto* state = reinterpret_cast<EqKnobState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

            switch (msg) {
            case WM_NCCREATE:
            {
                auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
                return TRUE;
            }
            case WM_MOUSEMOVE:
            {
                if (!state) break;

                if (state->dragging) {
                    const int y = GET_Y_LPARAM(lParam);
                    const float range = state->maxValue - state->minValue;
                    const float pixelsPerRange = state->controlsQ ? 180.0f : 130.0f;
                    const float value = state->dragStartValue +
                        static_cast<float>(state->dragStartY - y) * range / pixelsPerRange;
                    SetKnobValue(hwnd, *state, value);
                }

                if (!GetPropW(hwnd, L"hot")) {
                    SetPropW(hwnd, L"hot", reinterpret_cast<HANDLE>(1));
                    TRACKMOUSEEVENT tme = { sizeof(tme) };
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwnd;
                    TrackMouseEvent(&tme);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            case WM_MOUSELEAVE:
                RemovePropW(hwnd, L"hot");
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_LBUTTONDOWN:
                if (state) {
                    SetFocus(hwnd);
                    SetCapture(hwnd);
                    state->dragging = true;
                    state->dragStartY = GET_Y_LPARAM(lParam);
                    state->dragStartValue = GetKnobValue(*state);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case WM_LBUTTONUP:
                if (state && state->dragging) {
                    state->dragging = false;
                    if (GetCapture() == hwnd) {
                        ReleaseCapture();
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case WM_CAPTURECHANGED:
                if (state) {
                    state->dragging = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case WM_MOUSEWHEEL:
                if (state) {
                    const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                    const float direction = delta > 0 ? 1.0f : -1.0f;
                    SetKnobValue(hwnd, *state, GetKnobValue(*state) + direction * state->step);
                }
                return 0;
            case WM_KEYDOWN:
                if (state) {
                    switch (wParam) {
                    case VK_LEFT:
                    case VK_DOWN:
                        SetKnobValue(hwnd, *state, GetKnobValue(*state) - state->step);
                        return 0;
                    case VK_RIGHT:
                    case VK_UP:
                        SetKnobValue(hwnd, *state, GetKnobValue(*state) + state->step);
                        return 0;
                    case VK_HOME:
                        SetKnobValue(hwnd, *state, state->minValue);
                        return 0;
                    case VK_END:
                        SetKnobValue(hwnd, *state, state->maxValue);
                        return 0;
                    default:
                        break;
                    }
                }
                break;
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT ps = {};
                HDC hdc = BeginPaint(hwnd, &ps);
                if (state) {
                    PaintKnobBuffered(hwnd, hdc, *state, GetPropW(hwnd, L"hot") != nullptr);
                }
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_NCDESTROY:
                RemovePropW(hwnd, L"hot");
                delete state;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                break;
            default:
                break;
            }

            return DefWindowProcW(hwnd, msg, wParam, lParam);
            };
        wc.lpszClassName = kEqKnobClass;
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
    }
}
}

void StoreParametricEqBand(int band, float gainDb, float q)
{
    if (band < 0 || band >= kParametricEqBandCount) {
        return;
    }

    gainDb = ClampFloat(gainDb, kMinGainDb, kMaxGainDb);
    q = ClampFloat(q, kMinQ, kMaxQ);

    g_parametricEqGainDb[band].store(gainDb);
    g_parametricEqQ[band].store(q);
    SyncLegacyEqValues(band, gainDb);
}

void ShowParametricEqWindow(HWND owner)
{
    HINSTANCE instance = GetModuleHandleW(nullptr);
    RegisterEqClasses(instance);

    if (g_hEqWindow && IsWindow(g_hEqWindow)) {
        ShowWindow(g_hEqWindow, SW_SHOWNORMAL);
        SetForegroundWindow(g_hEqWindow);
        InvalidateRect(g_hEqWindow, nullptr, TRUE);
        return;
    }

    RECT ownerRect = {};
    if (owner && IsWindow(owner)) {
        GetWindowRect(owner, &ownerRect);
    }
    else {
        ownerRect = { 100, 100, 100 + kEqWindowWidth, 100 + kEqWindowHeight };
    }

    const int x = ownerRect.left + 36;
    const int y = ownerRect.top + 36;

    g_hEqWindow = CreateWindowExW(
        0,
        kEqWindowClass,
        Tr("eq.title", L""),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        x,
        y,
        kEqWindowWidth,
        kEqWindowHeight,
        owner,
        nullptr,
        instance,
        nullptr);

    if (g_hEqWindow) {
        SendMessageW(g_hEqWindow, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(GetEqualizerGlyphIcon(32)));
        SendMessageW(g_hEqWindow, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(GetEqualizerGlyphIcon(16)));
        ShowWindow(g_hEqWindow, SW_SHOWNORMAL);
        UpdateWindow(g_hEqWindow);
    }
}
