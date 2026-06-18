#include "util.h"
#include "audio_dsp.h"
#include "IRPFFmpeg.h"
#include "file_recording.h"
#include "metadata_decode.h"
#include "resource.h"
#include <fstream>
#include <string>
#include <vector>
#include <locale>
#include <codecvt>
#include <wininet.h>
#include <avrt.h>
#include <iomanip>
#include <iostream>
#include <libavutil/mathematics.h> // Для av_rescale_ts
#include <filesystem>
#include <cstdint>
#include <cstdio>
#include <regex>
#include <algorithm>
#include <cctype>
#include <commctrl.h>
#include <chrono>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "avrt.lib")

#ifndef ID_TIMER_METADATA
#define ID_TIMER_METADATA 4
#endif

// Globals for SDL
static SDL_Window* gWindow = NULL;
static SDL_Renderer* gRenderer = NULL;
static SDL_Texture* gTexture = NULL;
static const wchar_t* CURRENT_COVER_FILE_W = L"cover_cache\\cover.jpg";
static const char* CURRENT_COVER_FILE_A = "cover_cache\\cover.jpg";

// ShowCQT context
static SDL_Window* showcqt_window = nullptr;
static SDL_Renderer* showcqt_renderer = nullptr;
static SDL_Texture* showcqt_texture = nullptr;
static AVFilterGraph* showcqt_filter_graph = nullptr;
static AVFilterContext* showcqt_buffersrc = nullptr;
static AVFilterContext* showcqt_buffersink = nullptr;
static AVFrame* showcqt_frame = nullptr;
static std::mutex showcqt_mutex;
static std::vector<float> showcqt_audio_buffer;
static size_t showcqt_audio_read_pos = 0;
std::atomic<bool> showcqt_running{ false };
std::thread showcqt_thread;
std::vector<std::wstring> vec_url;

int showcqt_sample_rate = 44100;

static constexpr int SHOWCQT_VIS_CHANNELS = 2;
static constexpr int SHOWCQT_BUFFER_FRAMES = 24;

static void PostUiWideMessage(UINT msg, const std::wstring& text)
{
    if (!g_hMainWnd) {
        return;
    }

    auto* payload = new std::wstring(text);
    if (!PostMessageW(g_hMainWnd, msg, 0, (LPARAM)payload)) {
        delete payload;
    }
}

static int MeasureListBoxTextWidthPx(HWND hList, const std::wstring& text)
{
    if (!hList || text.empty()) return 0;

    HDC hdc = GetDC(hList);
    if (!hdc) return 0;

    HFONT hFont = (HFONT)SendMessageW(hList, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = nullptr;
    if (hFont) {
        oldFont = SelectObject(hdc, hFont);
    }

    SIZE textSize = {};
    int width = 0;
    if (GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.length()), &textSize)) {
        width = textSize.cx;
    }

    if (oldFont) {
        SelectObject(hdc, oldFont);
    }
    ReleaseDC(hList, hdc);

    return width + 24; // небольшой запас под внутренние отступы и скролл
}


// Функция для преобразования строки UTF-8 в wstring
std::wstring utf8_to_wstring(const std::string& str) {
    if (str.empty()) {
        return std::wstring();
    }
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

std::string wstring_to_utf8(const std::wstring& wstr)
{
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::string url_encode(const std::string& value) {
    std::string escaped;
    // Reserve memory (every character is encoded to 3 bytes)
    escaped.reserve(value.length() * 3);

    for (unsigned char c : value) {
        // Strict ASCII check to avoid locale-dependent issues with isalnum()
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            escaped += static_cast<char>(c);
        }
        else {
            // Fast hex conversion using a lookup table
            const char hex_chars[] = "0123456789ABCDEF";
            escaped += '%';
            escaped += hex_chars[c >> 4];
            escaped += hex_chars[c & 0x0F];
        }
    }

    return escaped;
}

std::wstring url_decode(const std::wstring& str) {
    std::wstring result;
    result.reserve(str.length());
    for (std::size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%') {
            if (i + 2 < str.length()) {
                std::wstring hex = str.substr(i + 1, 2);
                try {
                    wchar_t decoded_char = static_cast<wchar_t>(std::stoul(hex, nullptr, 16));
                    result += decoded_char;
                }
                catch (...) {
                    // invalid hex sequence, append as is
                    result += str[i];
                }
                i += 2;
            }
            else {
                result += str[i];
            }
        }
        else if (str[i] == '+') {
            result += ' ';
        }
        else {
            result += str[i];
        }
    }
    return result;
}

HRESULT download_file(const std::wstring& url, const std::wstring& file_path, int timeout) {
    HINTERNET hInternet = InternetOpen(L"WinAmp/5.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) {
        return E_FAIL;
    }

    // WinINet connect timeout must be configured before InternetOpenUrl.
    InternetSetOption(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOption(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOption(hInternet, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));

    HINTERNET hConnect = InternetOpenUrl(hInternet, url.c_str(), NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
    if (!hConnect) {
        InternetCloseHandle(hInternet);
        return E_FAIL;
    }

    HANDLE hFile = CreateFile(file_path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return E_FAIL;
    }

    char buffer[4096];
    DWORD bytesRead;
    DWORD bytesWritten;
    HRESULT hr = S_OK;

    while (InternetReadFile(hConnect, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        if (!WriteFile(hFile, buffer, bytesRead, &bytesWritten, NULL) || bytesRead != bytesWritten) {
            hr = E_FAIL;
            break;
        }
    }

    if (bytesRead == 0 && GetLastError() != ERROR_SUCCESS) {
        hr = E_FAIL;
    }

    CloseHandle(hFile);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);

    return hr;
}

std::string av_error_string(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, sizeof(errbuf));
    return std::string(errbuf);
}

// Helper function to write a wstring to a binary stream
void write_wstring(std::ofstream& ofs, const std::wstring& ws) {
    size_t len = ws.length();
    ofs.write(reinterpret_cast<const char*>(&len), sizeof(len));
    ofs.write(reinterpret_cast<const char*>(ws.c_str()), len * sizeof(wchar_t));
}

// Helper function to read a wstring from a binary stream
std::wstring read_wstring(std::ifstream& ifs) {
    size_t len = 0;
    ifs.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (ifs.gcount() != sizeof(len)) {
        // Handle error or EOF
        return L"";
    }
    std::vector<wchar_t> buffer(len);
    ifs.read(reinterpret_cast<char*>(buffer.data()), len * sizeof(wchar_t));
    if (ifs.gcount() != len * sizeof(wchar_t)) {
        // Handle error or short read
        return L"";
    }
    return std::wstring(buffer.begin(), buffer.end());
}


void savePlaylistToDat(const std::wstring& filename, const std::vector<PlaylistItem>& playlist, int selectedIndex) {
    std::ofstream ofs(filename, std::ios::binary);
    if (!ofs.is_open()) {
        return;
    }

    size_t count = playlist.size();
    ofs.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& item : playlist) {
        write_wstring(ofs, item.name);
        write_wstring(ofs, item.url);
    }

    ofs.write(reinterpret_cast<const char*>(&selectedIndex), sizeof(selectedIndex));

	//equlizer settings can be saved here
    float vol = current_volume.load();
    ofs.write(reinterpret_cast<const char*>(&vol), sizeof(vol));
	float eq_gain = current_eq_gain.load();
	ofs.write(reinterpret_cast<const char*>(&eq_gain), sizeof(eq_gain));
	float eq_gain_bass = current_eq_gain_bass.load();
	ofs.write(reinterpret_cast<const char*>(&eq_gain_bass), sizeof(eq_gain_bass));
    //flack or mp3 settings can be saved here
    ofs.write(reinterpret_cast<const char*>(&rec_is_flac), sizeof(rec_is_flac));
    ofs.write(reinterpret_cast<const char*>(&g_enableStereoWidth), sizeof(g_enableStereoWidth));
    bool obsoleteAutoVolume = false;
    ofs.write(reinterpret_cast<const char*>(&obsoleteAutoVolume), sizeof(obsoleteAutoVolume));
    ofs.write(reinterpret_cast<const char*>(&g_stereoWidthPercent), sizeof(g_stereoWidthPercent));
    ofs.write(reinterpret_cast<const char*>(&g_enableExciter), sizeof(g_enableExciter));
    ofs.write(reinterpret_cast<const char*>(&g_enableDynamicAutoVolume), sizeof(g_enableDynamicAutoVolume));
    ofs.write(reinterpret_cast<const char*>(&g_enableLimiterGainRider), sizeof(g_enableLimiterGainRider));
    ofs.write(reinterpret_cast<const char*>(&g_enableDeepBass), sizeof(g_enableDeepBass));
    ofs.write(reinterpret_cast<const char*>(&g_minimizeToTray), sizeof(g_minimizeToTray));


}

bool loadPlaylistFromDat(const std::wstring& filename, std::vector<PlaylistItem>& playlist, int& selectedIndex) {
    std::ifstream ifs(filename, std::ios::binary);
    if (!ifs.is_open()) {
        return false;
    }

    size_t count = 0;
    ifs.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (ifs.fail() || ifs.gcount() != sizeof(count)) {
        return false; // Failed to read count
    }

    playlist.clear();
    playlist.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        PlaylistItem item;
        item.name = read_wstring(ifs);
        item.url = read_wstring(ifs);
        if (ifs.fail()) {
            return false; // Failed while reading strings
        }
        playlist.push_back(item);
    }

    ifs.read(reinterpret_cast<char*>(&selectedIndex), sizeof(selectedIndex));
    if (ifs.fail()) {
        selectedIndex = -1; // set to invalid if read fails
        return false;
    }

	//equlizer settings can be loaded here
	float vol = 60.0f;
	ifs.read(reinterpret_cast<char*>(&vol), sizeof(vol));
	current_volume.store(vol);
	float eq_gain = 12.0f;
	ifs.read(reinterpret_cast<char*>(&eq_gain), sizeof(eq_gain));
	current_eq_gain.store(eq_gain);
	float eq_gain_bass = 3.0f;
	ifs.read(reinterpret_cast<char*>(&eq_gain_bass), sizeof(eq_gain_bass));
	current_eq_gain_bass.store(eq_gain_bass);
	//flack or mp3 settings can be loaded here
    ifs.read(reinterpret_cast<char*>(&rec_is_flac), sizeof(rec_is_flac));

    bool stereoWidth = g_enableStereoWidth;
    ifs.read(reinterpret_cast<char*>(&stereoWidth), sizeof(stereoWidth));
    if (!ifs.fail()) {
        g_enableStereoWidth = stereoWidth;
    }
    else {
        ifs.clear();
    }

    bool obsoleteAutoVolume = false;
    ifs.read(reinterpret_cast<char*>(&obsoleteAutoVolume), sizeof(obsoleteAutoVolume));
    if (ifs.fail()) {
        ifs.clear();
    }

    int stereoWidthPercent = g_stereoWidthPercent;
    ifs.read(reinterpret_cast<char*>(&stereoWidthPercent), sizeof(stereoWidthPercent));
    if (!ifs.fail()) {
        if (stereoWidthPercent < 0) stereoWidthPercent = 0;
        if (stereoWidthPercent > 100) stereoWidthPercent = 100;
        g_stereoWidthPercent = stereoWidthPercent;
    }
    else {
        ifs.clear();
    }

    bool exciter = g_enableExciter;
    ifs.read(reinterpret_cast<char*>(&exciter), sizeof(exciter));
    if (!ifs.fail()) {
        g_enableExciter = exciter;
    }
    else {
        ifs.clear();
    }

    bool dynamicAutoVolume = g_enableDynamicAutoVolume;
    ifs.read(reinterpret_cast<char*>(&dynamicAutoVolume), sizeof(dynamicAutoVolume));
    if (!ifs.fail()) {
        g_enableDynamicAutoVolume = dynamicAutoVolume;
    }
    else {
        ifs.clear();
    }

    bool limiterGainRider = g_enableLimiterGainRider;
    ifs.read(reinterpret_cast<char*>(&limiterGainRider), sizeof(limiterGainRider));
    if (!ifs.fail()) {
        g_enableLimiterGainRider = limiterGainRider;
    }
    else {
        ifs.clear();
    }

    bool deepBass = g_enableDeepBass;
    ifs.read(reinterpret_cast<char*>(&deepBass), sizeof(deepBass));
    if (!ifs.fail()) {
        g_enableDeepBass = deepBass;
    }
    else {
        ifs.clear();
    }

    bool minimizeToTray = g_minimizeToTray;
    ifs.read(reinterpret_cast<char*>(&minimizeToTray), sizeof(minimizeToTray));
    if (!ifs.fail()) {
        g_minimizeToTray = minimizeToTray;
    }
    else {
        ifs.clear();
    }
    return true;
}

static std::string TrimAscii(std::string value)
{
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };

    value.erase(value.begin(),
        std::find_if(value.begin(), value.end(), notSpace));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), notSpace).base(),
        value.end());

    return value;
}

static bool IsSupportedPlaylistUrl(const std::string& url)
{
    if (url.empty()) return false;

    const size_t colon = url.find(':');
    if (colon == std::string::npos || colon == 0) return false;

    std::string scheme = url.substr(0, colon);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static const char* kSchemes[] = {
        "http", "https", "mms", "mmsh", "mmst", "rtsp", "icy"
    };

    bool supported = false;
    for (const char* allowed : kSchemes) {
        if (scheme == allowed) {
            supported = true;
            break;
        }
    }
    if (!supported) return false;

    if (url.size() <= colon + 3) return false;
    if (url.compare(colon + 1, 2, "//") != 0) return false;

    const std::string remainder = url.substr(colon + 3);
    if (remainder.empty()) return false;
    if (remainder[0] == '/' || remainder[0] == '.') return false;

    for (unsigned char ch : url) {
        if (std::iscntrl(ch)) return false;
    }

    return true;
}

static std::wstring TryLoadStationNameFromIcy(const std::string& url)
{
    if (!IsSupportedPlaylistUrl(url)) return L"";

    HINTERNET hInternet = InternetOpenA(
        "WinAmp/5.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet) return L"";

    DWORD timeout = 1500;
    InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hInternet, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));

    const char* requestHeaders = "Icy-MetaData: 1\r\n";
    HINTERNET hUrl = InternetOpenUrlA(
        hInternet,
        url.c_str(),
        requestHeaders,
        -1,
        INTERNET_FLAG_NO_CACHE_WRITE |
        INTERNET_FLAG_PRAGMA_NOCACHE |
        INTERNET_FLAG_RELOAD,
        0);

    if (!hUrl) {
        InternetCloseHandle(hInternet);
        return L"";
    }

    const char* headerNames[] = {
        "icy-name",
        "ice-name",
        "x-audiocast-name"
    };

    std::wstring stationName;
    for (const char* headerName : headerNames) {
        char queryBuffer[512] = {};
        strcpy_s(queryBuffer, headerName);
        DWORD valueSize = sizeof(queryBuffer);
        if (HttpQueryInfoA(hUrl, HTTP_QUERY_CUSTOM, queryBuffer, &valueSize, nullptr)) {
            std::string value = TrimAscii(std::string(queryBuffer));
            if (!value.empty()) {
                stationName = DecodeMetadataToWideString(value);
                break;
            }
        }
    }

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);
    return stationName;
}

std::wstring ResolveStationNameFromUrl(const std::wstring& url)
{
    if (url.empty()) return L"";
    return TryLoadStationNameFromIcy(wstring_to_utf8(url));
}

void loadPlaylist(const std::wstring& filename, std::vector<PlaylistItem>& playlist) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::wstring msg = L"Could not open playlist file: " + filename;
            MessageBox(NULL, msg.c_str(), L"File Error", MB_OK | MB_ICONERROR);
        return;
    }

    playlist.clear();

    std::string line;
    int lineNumber = 0;
    int skippedEntries = 0;
    int validEntries = 0;

    PlaylistItem currentItem;
    bool expectingUrl = false; // Флаг состояния для парсинга пар #EXTINF/URL

    while (std::getline(file, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        line = TrimAscii(line);
        if (line.empty()) {
            continue;
        }

        if (expectingUrl) {
            // Предыдущая строка была #EXTINF, значит, эта строка должна быть URL.
            if (line[0] != '#' && IsSupportedPlaylistUrl(line)) {
                // Это действительный URL. Завершаем формирование элемента плейлиста.
                currentItem.url = utf8_to_wstring(line);
                if (currentItem.name.empty()) {
                    currentItem.name = currentItem.url;
                }
                playlist.push_back(currentItem);
                ++validEntries;
                expectingUrl = false;
                continue; // Элемент завершен, переходим к следующей строке.
            }
            else {
                // Это недействительный URL. #EXTINF с предыдущей строки недействителен.
                ++skippedEntries;
                //LogToUI("loadPlaylist: Expected a URL but found an invalid line " + std::to_string(lineNumber));
                // Сбрасываем состояние и go дальше для повторной обработки
                // текущей строки, так как она может быть началом новой записи (например, еще один #EXTINF).
                expectingUrl = false;
            }
        }

        if (line.rfind("#EXTINF:", 0) == 0) {
            // Начало нового расширенного элемента.
            currentItem = PlaylistItem();
            size_t comma_pos = line.find(',');
            if (comma_pos != std::string::npos) {
                std::string namePart = TrimAscii(line.substr(comma_pos + 1));
                if (!namePart.empty()) {
                    currentItem.name = utf8_to_wstring(namePart);
                }
            }
            expectingUrl = true; // Устанавливаем состояние ожидания URL на следующей строке.
        }
        else if (line[0] != '#') {
            // Это одиночный URL (не комментарий и не часть пары #EXTINF).
            if (IsSupportedPlaylistUrl(line)) {
                PlaylistItem item;
                item.url = utf8_to_wstring(line);
                item.name = item.url;
                playlist.push_back(item);
                ++validEntries;
            }
            else {
                ++skippedEntries;
                LogToUI("loadPlaylist: invalid standalone URL at line " + std::to_string(lineNumber) + " -> " + line);
            }
        }
        // Если строка является комментарием и мы не ожидаем URL, она просто игнорируется.
    }

    // Если файл закончился, пока мы ждали URL для последнего элемента.
    if (expectingUrl) {
        ++skippedEntries;
    }

    if (validEntries == 0) {
        std::wstring msg = L"No valid playlist entries found in: " + filename;
            MessageBox(NULL, msg.c_str(), L"Playlist Error", MB_OK | MB_ICONWARNING);
    }
    else if (skippedEntries > 0) {
        LogToUI("loadPlaylist: loaded " + std::to_string(validEntries) + " entries, skipped " + std::to_string(skippedEntries) + " invalid item(s).");
    }
}

bool initCoverRenderer(HWND hDlg)
{
    if (gRenderer) return true; // уже инициализировано

    HWND hStatic = GetDlgItem(hDlg, IDC_STATIC_IMG);
    if (!hStatic) return false;

    RECT rc;
    GetWindowRect(hStatic, &rc);
    POINT pt1 = { rc.left, rc.top }, pt2 = { rc.right, rc.bottom };
    ScreenToClient(hDlg, &pt1);
    ScreenToClient(hDlg, &pt2);
    rc.left = pt1.x; rc.top = pt1.y;
    rc.right = pt2.x; rc.bottom = pt2.y;

    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    // Создаём SDL окно один раз
    gWindow = SDL_CreateWindow("", rc.left, rc.top, w, h,
        SDL_WINDOW_BORDERLESS | SDL_WINDOW_SHOWN); 
    if (!gWindow) return false;

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(gWindow, &wmInfo);
    HWND sdlHwnd = wmInfo.info.win.window;

    SetParent(sdlHwnd, hDlg);
    LONG_PTR style = GetWindowLongPtr(sdlHwnd, GWL_STYLE);
    style &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    SetWindowLongPtr(sdlHwnd, GWL_STYLE, style);
    SetWindowPos(sdlHwnd, HWND_TOP, rc.left, rc.top, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    ShowWindow(GetDlgItem(hDlg, IDC_STATIC_IMG), SW_HIDE);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "2");
    gRenderer = SDL_CreateRenderer(gWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!gRenderer) {
        gRenderer = SDL_CreateRenderer(gWindow, -1, SDL_RENDERER_SOFTWARE);
        if (!gRenderer) {
            SDL_DestroyWindow(gWindow);
            gWindow = nullptr;
            return false;
        }
    }

    // Загружаем текстуру один раз
    SDL_Surface* surf = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_coverFileMutex);
        surf = IMG_Load(CURRENT_COVER_FILE_A);
    }
    if (!surf) {
        // ошибка
        SDL_DestroyRenderer(gRenderer);
        SDL_DestroyWindow(gWindow);
        gRenderer  = nullptr;
		gWindow = nullptr;
        return false;
    }
    gTexture = SDL_CreateTextureFromSurface(gRenderer, surf);
    SDL_FreeSurface(surf);
    if (!gTexture) {
        // ошибка
        SDL_DestroyRenderer(gRenderer);
        SDL_DestroyWindow(gWindow);
        gRenderer =  nullptr;
		gWindow = nullptr;
        return false;
    }

    return true;
}

bool reloadCoverTexture()
{
    if (!gRenderer) {
        //LogToUI("!gRenderer error: " + std::string(SDL_GetError()));
        return false;
    }

    // Сначала полностью готовим новую текстуру. Старую удаляем только после успеха.
    SDL_Surface* surf = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_coverFileMutex);
        surf = IMG_Load(CURRENT_COVER_FILE_A);
    }
    if (!surf) {
        //LogToUI("IMG_Load error: " + std::string(SDL_GetError()));
        return false;
    }

    SDL_Texture* newTexture = SDL_CreateTextureFromSurface(gRenderer, surf);
    SDL_FreeSurface(surf);

    if (!newTexture) {
        //LogToUI("SDL_CreateTextureFromSurface error: " + std::string(SDL_GetError()));
        return false;
    }

    if (gTexture) {
        SDL_DestroyTexture(gTexture);
    }
    gTexture = newTexture;

    return true;
}

bool redrawCoverImage(HWND hDlg)
{
    // 1. Валидация ресурсов
    if (!gRenderer || !gTexture || !gWindow) {
        //LogToUI("Error: NULL SDL resource");
        return false;
    }

    HWND hStatic = GetDlgItem(hDlg, IDC_STATIC_IMG);
    if (!hStatic) return false;

    RECT rc;
    GetWindowRect(hStatic, &rc);
    MapWindowPoints(HWND_DESKTOP, hDlg, (LPPOINT)&rc, 2);

    int drawW = rc.right - rc.left;
    int drawH = rc.bottom - rc.top;
    if (drawW <= 0 || drawH <= 0) return false;

    // SetWindowPos может вызывать WM_SIZE до того, как SDL обновит внутренние буферы
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(gWindow, &wmInfo)) {
        HWND sdlHwnd = wmInfo.info.win.window;
        // Проверяем, нужно ли вообще двигать окно (избегаем лишних пересозданий)
        RECT curRc;
        GetWindowRect(sdlHwnd, &curRc);
        MapWindowPoints(HWND_DESKTOP, hDlg, (LPPOINT)&curRc, 2);

        if (curRc.left != rc.left || curRc.top != rc.top ||
            curRc.right - curRc.left != drawW || curRc.bottom - curRc.top != drawH) {
            SetWindowPos(sdlHwnd, HWND_TOP, rc.left, rc.top, drawW, drawH,
                SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOZORDER);
            // Даём SDL время обработать изменение размера
            SDL_PumpEvents();
        }
    }

    // Размеры текстуры
    int texW, texH;
    if (SDL_QueryTexture(gTexture, nullptr, nullptr, &texW, &texH) != 0 || texW <= 0 || texH <= 0) {
        //LogToUI("Texture query failed: " + std::string(SDL_GetError()));
        return false;
    }

    // Центральный кроп
    SDL_Rect srcRect = { 0, 0, texW, texH };
    if (texW > texH) {
        srcRect.x = (texW - texH) / 2;
        srcRect.w = texH;
    }
    else if (texH > texW) {
        srcRect.y = (texH - texW) / 2;
        srcRect.h = texW;
    }

    // Расчёт dstRect с сохранением аспекта
    float texAspect = static_cast<float>(srcRect.w) / static_cast<float>(srcRect.h);
    float wndAspect = static_cast<float>(drawW) / static_cast<float>(drawH);

    SDL_Rect dstRect = { 0, 0, drawW, drawH }; // по умолчанию — fill
    if (texAspect > wndAspect) {
        dstRect.h = static_cast<int>(drawW / texAspect + 0.5f);
        dstRect.y = (drawH - dstRect.h) / 2;
    }
    else if (texAspect < wndAspect) {
        dstRect.w = static_cast<int>(drawH * texAspect + 0.5f);
        dstRect.x = (drawW - dstRect.w) / 2;
    }

    // Отрисовка
    COLORREF bg = GetSysColor(COLOR_3DFACE);
    SDL_SetRenderDrawColor(gRenderer, GetRValue(bg), GetGValue(bg), GetBValue(bg), 255);

    if (SDL_RenderClear(gRenderer) != 0 ||
        SDL_RenderCopy(gRenderer, gTexture, &srcRect, &dstRect) != 0) {
        //LogToUI("Render error: " + std::string(SDL_GetError()));
        return false;
    }

    SDL_RenderPresent(gRenderer);

    return true;
}


void cleanupSDL() {
    if (gTexture) {
        SDL_DestroyTexture(gTexture);
        gTexture = NULL;
    }
    if (gRenderer) {
        SDL_DestroyRenderer(gRenderer);
        gRenderer = NULL;
    }
    if (gWindow) {
        SDL_DestroyWindow(gWindow);
        gWindow = NULL;
    }

    IMG_Quit();
    //SDL_Quit();
}

// Initialize WASAPI
HRESULT InitWASAPI(IAudioClient*& audioClient, IAudioRenderClient*& renderClient,
    WAVEFORMATEX*& pwfx, UINT32& bufferFrameCount) {
    HRESULT hr = S_OK;

    // Reset realtime DSP state for the new WASAPI stream.
    ResetRealtimeAudioDspState();
	
    hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return hr;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void**)&enumerator);
    if (FAILED(hr)) return hr;

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    enumerator->Release();
    if (FAILED(hr)) return hr;

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    device->Release();
    if (FAILED(hr)) return hr;

    // Get device supported format
    hr = audioClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) {
        audioClient->Release();
        return hr;
    }
        // Try to use device native format
    REFERENCE_TIME hnsBufferDuration = WASAPI_BUFFER_MS * 10000;

    // Try to initialize with obtained format
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_NOPERSIST,
        hnsBufferDuration,
        0,
        pwfx,
        nullptr);

    // If failed, try to create compatible format
    if (FAILED(hr) && hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        
        CoTaskMemFree(pwfx);
        audioClient->Release();
        audioClient = nullptr;
        return hr;
    }

    if (FAILED(hr)) {
        CoTaskMemFree(pwfx);
        audioClient->Release();
        audioClient = nullptr;
        return hr;
    }

    // Get buffer size
    hr = audioClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr)) {
        CoTaskMemFree(pwfx);
        audioClient->Release();
        audioClient = nullptr;
        return hr;
    }

    // Get interface for writing data
    hr = audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
    if (FAILED(hr)) {
        CoTaskMemFree(pwfx);
        audioClient->Release();
        audioClient = nullptr;
        return hr;
    }

    return hr;
}

static bool IsWaveExtensibleFloat(const WAVEFORMATEX* wf) {
    if (!wf) return false;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* wfe = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        return IsEqualGUID(wfe->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    return false;
}

static AVSampleFormat WasapiSampleFormatToAV(const WAVEFORMATEX* wf)
{
    if (!wf) return AV_SAMPLE_FMT_NONE;
    if (IsWaveExtensibleFloat(wf)) return AV_SAMPLE_FMT_FLT;
    if (wf->wBitsPerSample == 32) return AV_SAMPLE_FMT_S32;
    if (wf->wBitsPerSample == 24) return AV_SAMPLE_FMT_S32;
    return AV_SAMPLE_FMT_S16;
}

static bool GetWasapiChannelLayout(const WAVEFORMATEX* wf, AVChannelLayout* layout)
{
    if (!wf || !layout || wf->nChannels == 0) return false;

    *layout = {};
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* wfe = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        if (wfe->dwChannelMask != 0 &&
            av_channel_layout_from_mask(layout, wfe->dwChannelMask) >= 0 &&
            layout->nb_channels == wf->nChannels) {
            return true;
        }
        av_channel_layout_uninit(layout);
        *layout = {};
    }

    av_channel_layout_default(layout, wf->nChannels);
    return layout->nb_channels == wf->nChannels;
}

bool CanBypassSwResample(AVCodecContext* inputCtx, WAVEFORMATEX* wasapiFormat)
{
    if (!inputCtx || !wasapiFormat) return false;
    if (!IsWaveExtensibleFloat(wasapiFormat)) return false;
    if (inputCtx->sample_fmt != AV_SAMPLE_FMT_FLT) return false;
    if (inputCtx->sample_rate != static_cast<int>(wasapiFormat->nSamplesPerSec)) return false;

    AVChannelLayout in_ch_layout = {};
    if (av_channel_layout_copy(&in_ch_layout, &inputCtx->ch_layout) < 0) {
        return false;
    }
    if (in_ch_layout.order == AV_CHANNEL_ORDER_UNSPEC && in_ch_layout.nb_channels > 0) {
        av_channel_layout_uninit(&in_ch_layout);
        av_channel_layout_default(&in_ch_layout, inputCtx->ch_layout.nb_channels);
    }

    AVChannelLayout out_ch_layout = {};
    bool hasOutputLayout = GetWasapiChannelLayout(wasapiFormat, &out_ch_layout);
    bool canBypass = hasOutputLayout &&
        av_channel_layout_compare(&in_ch_layout, &out_ch_layout) == 0;

    av_channel_layout_uninit(&in_ch_layout);
    av_channel_layout_uninit(&out_ch_layout);
    return canBypass;
}

// Function to initialize swresample
SwrContext* InitSwResample(AVCodecContext* inputCtx, WAVEFORMATEX* wasapiFormat) {
    SwrContext* swr = nullptr;

    // Define input parameters
    AVChannelLayout in_ch_layout = {};
    if (av_channel_layout_copy(&in_ch_layout, &inputCtx->ch_layout) < 0) {
        return nullptr;
    }
    if (in_ch_layout.order == AV_CHANNEL_ORDER_UNSPEC && in_ch_layout.nb_channels > 0) {
        av_channel_layout_uninit(&in_ch_layout);
        av_channel_layout_default(&in_ch_layout, inputCtx->ch_layout.nb_channels);
    }

    // Output sample format: choose by WASAPI format + bits-per-sample
    AVSampleFormat output_fmt = WasapiSampleFormatToAV(wasapiFormat);

    AVChannelLayout out_ch_layout = {};
    if (!GetWasapiChannelLayout(wasapiFormat, &out_ch_layout)) {
        av_channel_layout_uninit(&in_ch_layout);
        return nullptr;
    }

    if (CanBypassSwResample(inputCtx, wasapiFormat)) {
        av_channel_layout_uninit(&in_ch_layout);
        av_channel_layout_uninit(&out_ch_layout);
        return nullptr;
    }

    // Create swresample context
    int ret = swr_alloc_set_opts2(&swr,
        &out_ch_layout,
        output_fmt,
        wasapiFormat->nSamplesPerSec,
        &in_ch_layout,
        inputCtx->sample_fmt,
        inputCtx->sample_rate,
        0,
        nullptr);

    if (ret < 0 || !swr) {
        av_channel_layout_uninit(&in_ch_layout);
        av_channel_layout_uninit(&out_ch_layout);
        return nullptr;
    }

    // Повышаем качество ресемплинга, если сборка FFmpeg поддерживает soxr.
    if (av_opt_set(swr, "resampler", "soxr", 0) >= 0) {
        av_opt_set_int(swr, "precision", 28, 0);
        av_opt_set_int(swr, "cheby", 0, 0);
    }

    // Initialize
    ret = swr_init(swr);
    if (ret < 0) {
        swr_free(&swr);
        av_channel_layout_uninit(&in_ch_layout);
        av_channel_layout_uninit(&out_ch_layout);
        return nullptr;
    }

    av_channel_layout_uninit(&in_ch_layout);
    av_channel_layout_uninit(&out_ch_layout);
    return swr;
}

int init_audio_filter_graph(
    AVFilterGraph* graph,
    enum AVSampleFormat sample_fmt,
    int sample_rate,
    uint64_t channel_layout,
    float volume_start,
    AVFilterContext** out_abuf,
    AVFilterContext** out_aeq,
    AVFilterContext** out_avol,
    AVFilterContext** out_asink)
{
    int ret = 0;
    AVFilterContext* abuf = nullptr;
    AVFilterContext* preHeadroom = nullptr;
    AVFilterContext* aeq = nullptr;
    AVFilterContext* aeq0 = nullptr;
    AVFilterContext* aeqMid = nullptr;
    AVFilterContext* bass = nullptr;
    AVFilterContext* treble = nullptr;
    AVFilterContext* lnorm = nullptr;
    AVFilterContext* makeupGain = nullptr;
    AVFilterContext* avol = nullptr;
    AVFilterContext* asink = nullptr;
    
    char args[512];
    char eq_args[256];
    char vol_args[256];
    char layout_desc[128] = {};

    // Get filters
    const AVFilter* f_abuffer = avfilter_get_by_name("abuffer");
    const AVFilter* f_equalizer = avfilter_get_by_name("equalizer");
    const AVFilter* f_equalizer0 = avfilter_get_by_name("equalizer");
    const AVFilter* f_equalizerMid = avfilter_get_by_name("equalizer");
    const AVFilter* f_bass = avfilter_get_by_name("bass");
    const AVFilter* f_treble = avfilter_get_by_name("treble");
    const AVFilter* f_lnorm = avfilter_get_by_name("loudnorm");
    const AVFilter* f_volume = avfilter_get_by_name("volume");
    const AVFilter* f_buffersink = avfilter_get_by_name("abuffersink");

    if (!f_abuffer || !f_equalizer || !f_bass || !f_treble || !f_volume || !f_buffersink) {
      
        return AVERROR_FILTER_NOT_FOUND;
    }

    // Инициализация параметров эквалайзера внутри функции
    float eq_freq = 14000.0f;
    float eq_q = 1.0f;
    float eq_gain_db = current_eq_gain.load();

    current_volume.store(volume_start);
    current_eq_gain.store(eq_gain_db);

    AVChannelLayout input_layout = {};
    if (av_channel_layout_from_mask(&input_layout, channel_layout) >= 0) {
        if (av_channel_layout_describe(&input_layout, layout_desc, sizeof(layout_desc)) < 0) {
            layout_desc[0] = '\0';
        }
        av_channel_layout_uninit(&input_layout);
    }

    if (layout_desc[0] == '\0') {
        strcpy_s(layout_desc, channel_layout == AV_CH_LAYOUT_MONO ? "mono" : "stereo");
    }

    // Create abuffer argument string
    snprintf(args, sizeof(args),
        "sample_rate=%d:sample_fmt=%s:channel_layout=%s",
        sample_rate,
        av_get_sample_fmt_name(sample_fmt),
        layout_desc);

    // Create input abuffer filter
    ret = avfilter_graph_create_filter(&abuf, f_abuffer, "in", args, nullptr, graph);
    if (ret < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }

    // -3 dB запас перед EQ/effects.
    ret = avfilter_graph_create_filter(&preHeadroom, f_volume, "preHeadroom", "volume=0.70794576", nullptr, graph);
    if (ret < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }

    // Create equalizer argument string
    snprintf(eq_args, sizeof(eq_args), "f=%.1f:width_type=q:w=%.2f:g=%.1f",
        eq_freq, eq_q, eq_gain_db);
    // Create equalizer filter
    ret = avfilter_graph_create_filter(&aeq, f_equalizer, "eq", eq_args, nullptr, graph);
    if (ret < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }

    FillMemory(eq_args, sizeof(eq_args), 0);

    // Create equalizer0 argument string
    snprintf(eq_args, sizeof(eq_args), "f=30:width_type=q:w=0.8:g=%f", current_eq_gain_bass.load());
    // Create equalizer0 filter
    ret = avfilter_graph_create_filter(&aeq0, f_equalizer0, "eq0", eq_args, nullptr, graph);
    if (ret < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }

    FillMemory(eq_args, sizeof(eq_args), 0);

    // Create equalizerMid argument string
    snprintf(eq_args, sizeof(eq_args), "f=9200:width_type=q:w=1.4:g=4.0");
    // Create equalizerMid filter
    ret = avfilter_graph_create_filter(&aeqMid, f_equalizerMid, "eqMid", eq_args, nullptr, graph);
    if (ret < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }
	// Create bass filter
    ret = avfilter_graph_create_filter(&bass, f_bass, "bassFx", "g=1.0", nullptr, graph);
    if (ret < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }
	// Create treble filter
    ret = avfilter_graph_create_filter(&treble, f_treble, "trebleFx", "g=2.0", nullptr, graph);
    if (ret < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }
    // Restore the -3 dB pre-FX headroom before the user-controlled volume.
    ret = avfilter_graph_create_filter(&makeupGain, f_volume, "makeupGain", "volume=0.70794576", nullptr, graph); //запас почти -1.5 dB
    if (ret < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }
    // Create volume argument string
    snprintf(vol_args, sizeof(vol_args), "volume=%.2f", volume_start);
    // Create volume filter
    ret = avfilter_graph_create_filter(&avol, f_volume, "postVol", vol_args, nullptr, graph);
    if (ret < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }
    // Create output abuffersink filter
    ret = avfilter_graph_create_filter(&asink, f_buffersink, "out", nullptr, nullptr, graph);
    if (ret < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }
    // Link filters in chain: in -> preHeadroom -> aeq -> aeqMid -> aeq0 -> bass -> treble -> makeup -> vol -> out
	//+preHeadroom
    if ((ret = avfilter_link(abuf, 0, preHeadroom, 0)) < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }
	//+eq
    if ((ret = avfilter_link(preHeadroom, 0, aeq, 0)) < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }
	//+eqMid
    if ((ret = avfilter_link(aeq, 0, aeqMid, 0)) < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }
	//+eq0
    if ((ret = avfilter_link(aeqMid, 0, aeq0, 0)) < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }
	//+bass 
    if ((ret = avfilter_link(aeq0, 0, bass, 0)) < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }
	//+treble
    if ((ret = avfilter_link(bass, 0, treble, 0)) < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }
	//+makeup
    if ((ret = avfilter_link(treble, 0, makeupGain, 0)) < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }
	//+volume   
    if ((ret = avfilter_link(makeupGain, 0, avol, 0)) < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }
	//+out
    if ((ret = avfilter_link(avol, 0, asink, 0)) < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }

    // Configure graph
    if ((ret = avfilter_graph_config(graph, nullptr)) < 0) {
        OutputDebugStringA(av_error_string(ret).c_str());
        return ret;
    }

    // Return created filter contexts
    if (out_abuf) *out_abuf = abuf;
    if (out_aeq) *out_aeq = aeq;
    if (out_avol) *out_avol = avol;
    if (out_asink) *out_asink = asink;

 
    return 0;
}

// Helper: AVRational -> "num/den"
static std::string rational_to_string(const AVRational& r) {
    if (r.num == 0 && r.den == 0) return {};
    std::ostringstream ss;
    ss << r.num << "/" << r.den;
    return ss.str();
}

static bool is_nonempty(const char* s) { return s && *s; }

std::string oldssout = "";
void print_and_check_all_metadata(AVFormatContext* fmt, std::string& out) {
    
    if (!fmt) return;
    
    out.clear();

    AVDictionaryEntry* tag = nullptr;
    // Format-level metadata
    while ((tag = av_dict_get(fmt->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        if (tag->key && tag->value) {
            if (!out.empty()) out += '\n';
            out += tag->key;
            out += " = ";
            out += tag->value;
        }
    }

    // Stream-level metadata
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (!st) continue;
        AVDictionaryEntry* st_tag = nullptr;
        while ((st_tag = av_dict_get(st->metadata, "", st_tag, AV_DICT_IGNORE_SUFFIX))) {
            if (st_tag->key && st_tag->value) {
                if (!out.empty()) out += '\n';
                out += "stream";
                out += std::to_string(i);
                out += '.';
                out += st_tag->key;
                out += " = ";
                out += st_tag->value;
            }
        }

        std::ostringstream ssout;
        // AVStream basic fields
        if (st->index >= 0) ssout << "index = " << st->index << '\n';
        if (st->id != 0) ssout << "id = " << st->id << '\n';
        if (auto tb = rational_to_string(st->time_base); !tb.empty()) ssout << "time_base = " << tb << '\n';
        if (st->start_time != AV_NOPTS_VALUE) ssout << "start_time = " << st->start_time << '\n';
        if (st->duration != AV_NOPTS_VALUE) ssout << "duration = " << st->duration << '\n';
        if (st->nb_frames > 0) ssout << "nb_frames = " << st->nb_frames << '\n';
        if (auto rf = rational_to_string(st->r_frame_rate); !rf.empty()) ssout << "r_frame_rate = " << rf << '\n';
        if (auto af = rational_to_string(st->avg_frame_rate); !af.empty()) ssout << "avg_frame_rate = " << af << '\n';
        if (st->disposition) ssout << "disposition = " << st->disposition << '\n';

        if (!out.empty()) out += '\n';
        out += ssout.str();

        // also include codecpar basic info if present
        if (st->codecpar) {
            std::ostringstream ssout;
            if (!out.empty()) out += '\n';
            std::string codec_name;
            if (st->codecpar->codec_id) {
                codec_name = avcodec_get_name(st->codecpar->codec_id);
                out += "Codec name: ";
                out += codec_name;
                out += '\n';
            }
            if (st->codecpar->format != 0) ssout << "format = " << st->codecpar->format << '\n';
            if (st->codecpar->bit_rate > 0) ssout << "bit_rate = " << st->codecpar->bit_rate << '\n';
            if (st->codecpar->bits_per_coded_sample > 0) ssout << "bits_per_coded_sample = " << st->codecpar->bits_per_coded_sample << '\n';
            if (st->codecpar->sample_rate > 0) ssout << "sample_rate = " << st->codecpar->sample_rate << '\n';
     
            out += ssout.str();

			//LogToUI(out);

            if (oldssout == out) continue;
            // Формируем однострочный текст для заголовка
            std::ostringstream header_ss;

            auto append_dot = [&header_ss]() {
                if (!header_ss.str().empty()) {
                    header_ss << " \xE2\x80\xA2 ";
                }
                };

            if (!codec_name.empty()) {
                std::transform(codec_name.begin(), codec_name.end(), codec_name.begin(),
                    [](unsigned char c) { return std::toupper(c); });
                header_ss << codec_name;
            }
            if (st->codecpar->bit_rate > 0) {
                int kbit = static_cast<int>((st->codecpar->bit_rate) / 1000); // округление к ближайшему
                append_dot();
                header_ss << kbit << "kbps";
            }
            if (st->codecpar->sample_rate > 0) {
                float srate = st->codecpar->sample_rate / 1000.0f;
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(1) << srate;
                append_dot();
                header_ss << ss.str() << "kHz";
            }
            std::string header_str = header_ss.str();

            if (!header_str.empty()) {
                PostUiWideMessage(WM_APP_STREAM_INFO, utf8_to_wstring(header_str));
            }
        }
    }

    oldssout = out;
	
}

// Function to decode ICY metadata
std::string decode_icy_metadata(const std::string& metadata) {

    // Remove leading and trailing whitespace
    std::string trimmed = metadata;
    while (!trimmed.empty() && std::isspace(trimmed[0])) trimmed.erase(0, 1);
    while (!trimmed.empty() && std::isspace(trimmed.back())) trimmed.pop_back();

    std::string	result = trimmed;

    // Replace HTML entities
    std::vector<std::pair<std::string, std::string>> replacements = {
        {"&amp;", "&"},
        {"&quot;", "\""},
        {"&apos;", "'"},
        {"&lt;", "<"},
        {"&gt;", ">"},
        {"&#39;", "'"},
        {"&#34;", "\""},
        {"&nbsp;", " "}
    };

    for (const auto& repl : replacements) {
        size_t pos = 0;
        while ((pos = result.find(repl.first, pos)) != std::string::npos) {
            result.replace(pos, repl.first.length(), repl.second);
            pos += repl.second.length();
        }
    }

    // Remove extra spaces
    result.erase(std::unique(result.begin(), result.end(),
        [](char a, char b) { return a == ' ' && b == ' '; }), result.end());
    
    return result;
}


void StartMetadataTimer() {
    if (!g_hMainWnd) return;
    KillTimer(g_hMainWnd, ID_TIMER_METADATA);
    SetTimer(g_hMainWnd, ID_TIMER_METADATA, METADATA_UPDATE_INTERVAL_MS, NULL);
}


void StopMetadataTimer() {
    if (!g_hMainWnd) return;
    KillTimer(g_hMainWnd, ID_TIMER_METADATA);
}

std::string oldmeta = "";
void ResetMetadataCaches() { //очистка глобальных переменных, чтобы при смене трека не было артефактов от старых данных 
    oldssout.clear();
    oldmeta.clear();
}

// Удалить все вхождения
void remove_all_patterns(std::string& s, const std::string& pat) {
    size_t pos = 0;
    while ((pos = s.find(pat, pos)) != std::string::npos) {
        s.erase(pos, pat.length());
    }
}

// список паттернов и флаг
void remove_patterns(std::string& s, const std::vector<std::string>& patterns, bool remove_all = true) {
    for (const auto& pat : patterns) {
        if (pat.empty()) continue;
        if (remove_all) remove_all_patterns(s, pat);
    }
}

static std::string CollapseRepeatedTrailingTitle(std::string value)
{
    static const std::string separator = " - ";

    while (true) {
        const size_t sepPos = value.rfind(separator);
        if (sepPos == std::string::npos) {
            break;
        }

        std::string prefix = TrimAscii(value.substr(0, sepPos));
        const std::string suffix = TrimAscii(value.substr(sepPos + separator.size()));
        if (prefix.empty() || suffix.empty() || prefix.size() < suffix.size()) {
            break;
        }

        const size_t suffixPos = prefix.size() - suffix.size();
        if (prefix.compare(suffixPos, suffix.size(), suffix) != 0) {
            break;
        }

        const bool suffixStartsAfterSeparator =
            suffixPos >= separator.size() &&
            prefix.compare(suffixPos - separator.size(), separator.size(), separator) == 0;

        if (!suffixStartsAfterSeparator) {
            break;
        }

        value = prefix;
    }

    return value;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.length(), to);
        pos += to.length(); // шаг вперёд, чтобы не зациклиться если to содержит from
    }
}

// Function to get metadata from format context
void update_stream_metadata() {

    if (!formatCtx) return;

    std::string meta;
    print_and_check_all_metadata(formatCtx, meta);
    
    if (meta.empty()) return;
    if (oldmeta == meta) return;
	//всё, что не нужно, удаляем
    std::vector<std::string> patterns = { "Now Playing: ",
                                          " *** www.ipmusic.ch",
                                          "AutoDJ: "
                                        };

    remove_patterns(meta, patterns, true); // true — удалить все вхождения
	// заменяем " ," на " - "
    replace_all(meta, " ,", " - ");

    oldmeta = meta;
    meta = decode_icy_metadata(meta);
    std::string new_metadata;

    //LogToUI(meta);
    
    // Parse meta string which contains all metadata in "key = value" format on each line
    std::istringstream stream(meta);
    std::string line;
    std::map<std::string, std::string> meta_map;

    while (std::getline(stream, line)) {
        size_t eq_pos = line.find(" = ");
        if (eq_pos != std::string::npos) {
            std::string key = line.substr(0, eq_pos);
            std::string value = line.substr(eq_pos + 3); // length of " = "
            meta_map[key] = value;
        }
    }

    // Function to find value by key (exact match)
    auto find_value = [&meta_map](const std::string& key) -> std::string {
        auto it = meta_map.find(key);
        if (it != meta_map.end() && !it->second.empty()) {
            return it->second;
        }
        return "";
        };

    // find StreamTitle
    new_metadata = find_value("StreamTitle");

    // If not found, try ARTIST and TITLE
    if (new_metadata.empty()) {
        std::string artist = find_value("ARTIST");
        std::string title = find_value("TITLE");
        if (artist.empty() || title.empty()) {
            for (const auto& pair : meta_map) {
                const std::string& key = pair.first;
                if (key.find("ARTIST") != std::string::npos && artist.empty()) {
                    artist = pair.second;
                }
                if (key.find("TITLE") != std::string::npos && title.empty()) {
                    title = pair.second;
                }
            }
        }
        if (!artist.empty() && !title.empty()) {
            new_metadata = artist + " - " + title;
        }
        else if (!artist.empty()) {
            new_metadata = artist;
        }
        else if (!title.empty()) {
            new_metadata = title;
        }

    }
    else if (new_metadata.size() < 5){

		new_metadata.clear(); // too short to be valid  
    }

    if (new_metadata.empty()) {

        std::string key = "title = ";
        auto pos = meta.find(key);
        if (pos != std::string::npos) {
            pos += key.size();
            auto end = meta.find('\n', pos);
            new_metadata = meta.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        }

		key = "artist = ";
        pos = meta.find(key);
        if (pos != std::string::npos) {
            pos += key.size();
            auto end = meta.find('\n', pos);
            std::string artist = meta.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (!artist.empty()) {
                if (!new_metadata.empty()) {
                    new_metadata = artist + " - " + new_metadata;
                }
                else {
                    new_metadata = artist;
                }
            }
		}
    }

    if (!new_metadata.empty()) {
        //1.
        while (new_metadata[0] == '-' || new_metadata[0] == ' ')  new_metadata.erase(new_metadata.begin());

        if (new_metadata.empty()) return;

        //2.
        std::string key = "| ";
        auto pos = new_metadata.find(key);
        if (pos != std::string::npos) {
            pos += key.size();
            new_metadata = new_metadata.substr(pos, std::string::npos);
        }

        new_metadata = std::regex_replace(new_metadata, std::regex("—"), "-");
		new_metadata = CollapseRepeatedTrailingTitle(new_metadata);// удаляем повторяющуюся часть ("Artist - Title - Title" -> "Artist - Title")
        std::wstring decoded_metadata = DecodeMetadataToWideString(new_metadata);
        if (decoded_metadata.empty()) return;
        std::string normalized_metadata = wstring_to_utf8(decoded_metadata);
       
        if (current_metadata != normalized_metadata) {
            // Обновляем текущий трек
            current_metadata = normalized_metadata;
            current_track = normalized_metadata;
            //ограничиваем до 100
            track_history.size() >= 100 ?
				track_history.pop_back() : void();  

            // Добавляем НОВЫЙ трек в контейнер
            track_history.insert(track_history.begin(), normalized_metadata);

            // Recording
            if (g_is_recording.load()) {
                // находится в file_recording.cpp
                StopRecording();
            }
            if (g_rec_semafor.load()) {
                // находится в file_recording.cpp
                StartRecording();
            }
            
            const std::wstring& ss = decoded_metadata;
            PostUiWideMessage(WM_APP_METADATA_UPDATED, ss);

            //добавляем название трека для поиска изображения из интернета
            if (!ss.empty()) {
                EnterCriticalSection(&g_url_vec_cs);
                vec_url.push_back(ss);
                LeaveCriticalSection(&g_url_vec_cs);

                // Обновляем UI
                HWND hList = GetDlgItem(g_hMainWnd, IDC_LIST2);
              
                if (hList) {
                    // Добавляем элемент
                    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)ss.c_str());

                    int textWidthPx = MeasureListBoxTextWidthPx(hList, ss);
                    if (textWidthPx > alwaysVisibleExtent) {
                        int currentExtent = static_cast<int>(SendMessageW(hList, LB_GETHORIZONTALEXTENT, 0, 0));
                        int newExtent = (std::min)(textWidthPx, maxVisibleExtent);
                        if (newExtent > currentExtent) {
                            SendMessageW(hList, LB_SETHORIZONTALEXTENT, (WPARAM)newExtent, 0);
                        }
                    }

                    SendMessageW(hList, LB_SETCURSEL, 0, 0);
                    SendMessageW(hList, LB_SETTOPINDEX, 0, 0);
                    InvalidateRect(hList, NULL, TRUE);
                    UpdateWindow(hList);
                }
            }

        }
    }
}

bool ReinitializeWASAPI(IAudioClient*& audioClient, IAudioRenderClient*& renderClient,
    WAVEFORMATEX*& pwfx, UINT32& bufferFrameCount) {

    // Освобождаем текущие ресурсы
    if (pwfx)
    {
        CoTaskMemFree(pwfx);
		pwfx = nullptr;
    }


    if (renderClient) {
        renderClient->Release();
        renderClient = nullptr;
    }
    if (audioClient) {
        audioClient->Stop();
        audioClient->Reset();
        audioClient->Release();
        audioClient = nullptr;
    }

    // Инициализируем заново
    // Reset realtime DSP state so stale filter state from the old stream
    // does not produce a transient click at the start of the new one.
    ResetRealtimeAudioDspState();
      
    HRESULT hr = InitWASAPI(audioClient, renderClient, pwfx, bufferFrameCount);
    if (FAILED(hr)) {
        //LogToUI("Failed to reinitialize WASAPI: 0x" + std::to_string(hr));
        return false;
    }

    hr = audioClient->Start();
    if (FAILED(hr)) {
        //LogToUI("Failed to start WASAPI after reinitialization: 0x" + std::to_string(hr));
        return false;
    }

    return true;
}

static bool WaitBeforePlaybackReconnect(unsigned long playbackGeneration, int delayMs = 1500)
{
    constexpr int stepMs = 100;
    const int steps = delayMs / stepMs;

    for (int i = 0; i < steps && !g_quit_flag.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
    }

    return !g_quit_flag.load() && playbackGeneration == g_playbackGeneration.load();
}

struct MmcssAudioThreadRegistration {
    HANDLE task = nullptr;

    MmcssAudioThreadRegistration()
    {
        DWORD taskIndex = 0;
        task = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
        if (task) {
            AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
        }
    }

    ~MmcssAudioThreadRegistration()
    {
        if (task) {
            AvRevertMmThreadCharacteristics(task);
        }
    }
};

void PlaybackLoop(AVFormatContext*& formatCtx,
    AVCodecContext*& codecCtx,
    int& audioStreamIndex,
    IAudioClient* audioClient,
    IAudioRenderClient* renderClient,
    UINT32 bufferFrameCount,
    WAVEFORMATEX* pwfx,
    SwrContext*& swr,
    const char* radioUrl,
    unsigned long playbackGeneration) {

    MmcssAudioThreadRegistration mmcssAudio;

    ResetRealtimeAudioDspState();
    ResetLimiterGainRider();

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    if (!packet || !frame) return;

    int convertedBufferSamples = static_cast<int>(bufferFrameCount * 2);
    uint8_t* converted_buffer = (uint8_t*)av_malloc(convertedBufferSamples * pwfx->nChannels * sizeof(float));
    if (!converted_buffer) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        return;
    }

    // Check filter graph availability
    bool use_filters = (filterGraph && filter_abuf && filter_asink);

    auto playback_start = std::chrono::steady_clock::now();
    auto last_elapsed_update = playback_start;


    int consecutive_errors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 20;

    HRESULT hr0 = 0;

    PostMessage(g_hMainWnd, WM_APP_UPDATE_ICON, 0, 0);

    while (running.load()) {

        if (g_quit_flag.load())  break;

        if (!formatCtx) {// dummy error handling

            running.store(false);
            g_quit_flag.store(true);
            PostMessageW(g_hMainWnd, WM_APP_PLAYBACK_ERROR, (WPARAM)playbackGeneration, 0);
            break;
        }

        int read_result = av_read_frame(formatCtx, packet);

        if (read_result < 0) {
            // Handle read errors
            if (g_quit_flag.load()) {
                break;
            }
            if (read_result == AVERROR_EXIT) {
                break;
            }
            if (read_result == AVERROR_EOF) {
                //LogToUI("PlaybackLoop: End of stream.");
            }
            else if (read_result == AVERROR(EAGAIN)) {
                // Non‑blocking read would block; sleep a bit and retry
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                av_packet_unref(packet);
                continue;
            }
            else {
                consecutive_errors++;
                if (consecutive_errors > MAX_CONSECUTIVE_ERRORS) {

                    if (reconnect_attempts < MAX_RECONNECT_ATTEMPTS) {
                        // Wait before retrying??
                        PostFfmpegStatus(L"Ошибка чтения потока. Переподключение...");
                        const int nextAttempt = ++reconnect_attempts;
                        if (WaitBeforePlaybackReconnect(playbackGeneration)) {
                            PostMessageW(g_hMainWnd, WM_APP_PLAYBACK_ERROR, (WPARAM)playbackGeneration, nextAttempt);
                        }
                        break;

                    }
                    else {
                        //слишком много ошибок
                        PostFfmpegStatus(L"Ошибка потока: превышено число попыток переподключения");
                        if (WaitBeforePlaybackReconnect(playbackGeneration)) {
                            running.store(false);
                            g_quit_flag.store(true);
                            PostMessageW(g_hMainWnd, WM_APP_PLAYBACK_ERROR, (WPARAM)playbackGeneration, 0);
                        }
                        
                        break;
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            av_packet_unref(packet);
            continue;
        }

        // Reset error counter on successful read
        consecutive_errors = 0;
        reconnect_attempts = 0;

        // --- Elapsed time update (once per second) ---
        auto now = std::chrono::steady_clock::now();
        auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_elapsed_update).count();

        if (since_last >= 1000) {
            last_elapsed_update = now;
            int64_t elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(
                now - playback_start).count();
            // WPARAM = секунды. Главное окно само форматирует строку.
            PostMessageW(g_hMainWnd, WM_APP_ELAPSED_TIME, static_cast<WPARAM>(elapsed_sec), 0);
        }

        // Defensive check for packet not being null
        if (packet && packet->stream_index == audioStreamIndex) {
            if (avcodec_send_packet(codecCtx, packet) == 0) {
                while (avcodec_receive_frame(codecCtx, frame) == 0) {
                    // Defensive check for frame data
                    if (!frame || !frame->data[0]) {
                        continue;
                    }

                    // ДО КОНВЕРТАЦИИ пишем звук байт в байт, как в источнике.
                    if (g_is_recording.load()) {
                        // Для записи нам нужны оригинальные данные
                        // Определяем параметры аудио из фрейма
                        int frame_channels = frame->ch_layout.nb_channels;
                        int frame_sample_rate = frame->sample_rate;
                        // Копируем аудиоданные из фрейма
                        // В зависимости от формата фрейма преобразуем в float
                        std::vector<float> raw_audio_data;

                        if (frame->format == AV_SAMPLE_FMT_FLT ||
                            frame->format == AV_SAMPLE_FMT_FLTP) {
                            // Уже float формат
                            int total_samples = frame->nb_samples * frame_channels;
                            raw_audio_data.resize(total_samples);

							if (frame->format == AV_SAMPLE_FMT_FLT) { //уже то, что нужно
                                // Packed float - просто копируем
                                const float* src = (const float*)frame->data[0];
                                std::copy(src, src + total_samples, raw_audio_data.begin());
                            }
                            else {
                                // Planar float - нужно "интерливить"
                                for (int i = 0; i < frame->nb_samples; i++) {
                                    for (int ch = 0; ch < frame_channels; ch++) {
                                        const float* channel_data = (const float*)frame->extended_data[ch];
                                        raw_audio_data[i * frame_channels + ch] = channel_data[i];
                                    }
                                }
                            }
                        }
                        else {
                            // Нужно конвертировать в float AV_SAMPLE_FMT_FLT для записи
                            // Используем временный swr контекст для преобразования во float
                            SwrContext* record_swr = swr_alloc();
                            if (record_swr) {
                                AVChannelLayout in_layout = frame->ch_layout;
                                AVChannelLayout out_layout;
                                av_channel_layout_copy(&out_layout, &in_layout);

                                swr_alloc_set_opts2(&record_swr,
                                    &out_layout,
                                    AV_SAMPLE_FMT_FLT,  // Конвертируем во float для записи
                                    frame_sample_rate,
                                    &in_layout,
                                    (AVSampleFormat)frame->format,
                                    frame_sample_rate,
                                    0,
                                    nullptr);

                                if (swr_init(record_swr) >= 0) {
                                    int max_samples = frame->nb_samples + 256;
                                    std::vector<uint8_t> temp_buffer(max_samples * frame_channels * sizeof(float));
                                    uint8_t* out_data[1] = { temp_buffer.data() };

                                    int converted = swr_convert(record_swr,
                                        out_data, max_samples,
                                        (const uint8_t**)frame->data, frame->nb_samples);

                                    if (converted > 0) {
                                        int total_samples = converted * frame_channels;
                                        raw_audio_data.assign((float*)temp_buffer.data(),
                                            (float*)temp_buffer.data() + total_samples);
                                    }
                                }

                                swr_free(&record_swr);
                                av_channel_layout_uninit(&out_layout);
                            }
                        }

                        // Добавляем в очередь для записи
                        if (!raw_audio_data.empty()) {
                            // находится в file_recording.cpp
                            PushRecordingAudio(std::move(raw_audio_data), frame_sample_rate, frame_channels);
                        }
                    }

                    int converted_count = 0;
                    if (swr) {
                        uint8_t* out_data[1] = { converted_buffer };
                        converted_count = swr_convert(swr,
                            out_data, convertedBufferSamples,
                            (const uint8_t**)frame->data, frame->nb_samples);
                    }
                    else if (frame->format == AV_SAMPLE_FMT_FLT && frame->data[0]) {
                        if (frame->nb_samples > convertedBufferSamples) {
                            uint8_t* resized = (uint8_t*)av_realloc(
                                converted_buffer,
                                frame->nb_samples * pwfx->nChannels * sizeof(float));
                            if (!resized) {
                                running.store(false);
                                break;
                            }
                            converted_buffer = resized;
                            convertedBufferSamples = frame->nb_samples;
                        }

                        converted_count = frame->nb_samples;
                        if (!converted_buffer) {
                            running.store(false);
                            break;
                        }
                        memcpy(converted_buffer,
                            frame->data[0],
                            converted_count * pwfx->nChannels * sizeof(float));
                    }

                    if (converted_count > 0) {
                        float* audio_data = (float*)converted_buffer;
                        int channels = pwfx->nChannels;
                        int total_samples = converted_count * channels;

                        /*DSP фильтры*/
                        // Remove DC offset first so downstream stages see a clean signal
                        RemoveDCOffset(audio_data, static_cast<size_t>(converted_count),
                            pwfx->nChannels, static_cast<int>(pwfx->nSamplesPerSec));

                        if (g_enableDynamicAutoVolume) {
                            ProcessDynamicAutoVolume(audio_data, static_cast<size_t>(converted_count), pwfx->nChannels, static_cast<int>(pwfx->nSamplesPerSec));
                        }

                         // Process through filter graph if available
                        if (use_filters) {
                            // Create AVFrame for filters
                            AVFrame* frame_for_filter = av_frame_alloc();
                            if (frame_for_filter) {
                                frame_for_filter->format = AV_SAMPLE_FMT_FLTP;
                                frame_for_filter->sample_rate = pwfx->nSamplesPerSec;
                                frame_for_filter->nb_samples = converted_count;

                                AVChannelLayout filter_layout = {};

                                if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                                    auto* wfex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
                                    if (av_channel_layout_from_mask(&filter_layout, wfex->dwChannelMask) < 0) {
                                        av_channel_layout_default(&filter_layout, channels);
                                    }
                                }
                                else {
                                    av_channel_layout_default(&filter_layout, channels);
                                }

                                // Copy layout to frame
                                av_channel_layout_copy(&frame_for_filter->ch_layout, &filter_layout);

                                if (av_frame_get_buffer(frame_for_filter, 0) >= 0) {
                                    // Convert from FLT (packed) to FLTP (planar)
                                    float* src = audio_data;
                                    int filter_channels = frame_for_filter->ch_layout.nb_channels;
                                    int copy_channels = (std::min)(channels, filter_channels);

                                    for (int ch = 0; ch < copy_channels; ch++) {
                                        float* dst = (float*)frame_for_filter->extended_data[ch];
                                        for (int i = 0; i < converted_count; i++) {
                                            dst[i] = src[i * channels + ch];
                                        }
                                    }

                                    // Feed frame to filter graph
                                    int ret = av_buffersrc_add_frame(filter_abuf, frame_for_filter);
                                    if (ret < 0) {
                                        
                                        //LogToUI("Continue without filtering on error.");
                                    }
                                    else {
                                        // Get processed frame
                                        AVFrame* filtered_frame = av_frame_alloc();
                                        ret = av_buffersink_get_frame(filter_asink, filtered_frame);

                                        if (ret >= 0 && filtered_frame && filtered_frame->nb_samples > 0) {
                                            // Convert back from FLTP to FLT
                                            int filtered_channels = filtered_frame->ch_layout.nb_channels;
                                            int copy_back_channels = (std::min)(channels, filtered_channels);
                                            for (int i = 0; i < filtered_frame->nb_samples; i++) {
                                                for (int ch = 0; ch < copy_back_channels; ch++) {
                                                    float* channel_data = (float*)filtered_frame->extended_data[ch];
                                                    audio_data[i * channels + ch] = channel_data[i];
                                                }
                                            }
                                            av_frame_free(&filtered_frame);
                                        }
                                        else if (ret < 0) {
                                            av_frame_free(&filtered_frame);
                                        }
                                        else {
                                            av_frame_free(&filtered_frame);
                                        }
                                    }
                                }
                                // Free layout
                                av_channel_layout_uninit(&filter_layout);
                                av_frame_free(&frame_for_filter);
                            }
                        }
                        /*DSP фильтры*/
                        if (g_enableDeepBass) {
                            ProcessDeepBass(audio_data, static_cast<size_t>(converted_count),
								pwfx->nChannels, static_cast<int>(pwfx->nSamplesPerSec), 0.2f); // 0.2f - сколько подмешивать deep bass enhancement
                        }
                        // wide stereo base
                        if (g_enableStereoWidth && pwfx->nChannels == 2) {
                            float stereoWidthFactor = 1.0f + (static_cast<float>(g_stereoWidthPercent) / 100.0f);
                            ProcessStereoWidth(audio_data, static_cast<size_t>(converted_count), pwfx->nChannels, stereoWidthFactor);
                        }
                        if (g_enableExciter) {
                            ProcessExciter(audio_data, static_cast<size_t>(converted_count),
                                pwfx->nChannels, static_cast<int>(pwfx->nSamplesPerSec), 0.55f);
                        }
                        // Apply limiter to processed data
                        constexpr float kFinalLimiterLimit = 0.97f;
                        if (g_enableLimiterGainRider) {
                            ApplyLimiterGainRider(audio_data, total_samples);
                        }
                        else {
                            ResetLimiterGainRider(false);
                        }
						FinalLimiterActivity limiter_activity = ApplyFinalLimiter(audio_data, total_samples, kFinalLimiterLimit);// 0.97 - оставляем небольшой запас от полного клиппинга для более мягкого звучания
                        if (g_enableLimiterGainRider) {
                            UpdateLimiterGainRider(limiter_activity, total_samples);
                        }
               
                        int samples_written = 0;

                        while (samples_written < converted_count && running.load() && !g_quit_flag.load()) {
                            UINT32 padding = 0;

                            hr0 = audioClient->GetCurrentPadding(&padding);

                            if (hr0 == AUDCLNT_E_DEVICE_INVALIDATED) {
                                PostFfmpegStatus(L"Аудиоустройство изменено, переинициализация...");
                                av_free(converted_buffer);
                                av_packet_free(&packet);
                                av_frame_free(&frame);

                                // Сохраняем старую частоту, чтобы сравнить потом
                                int old_sample_rate = pwfx ? pwfx->nSamplesPerSec : 0;

                                if (ReinitializeWASAPI(audioClient, renderClient, pwfx, bufferFrameCount)) {

                                    if (old_sample_rate > 0 && pwfx->nSamplesPerSec != old_sample_rate) {

                                        PostMessageW(g_hMainWnd, WM_APP_PLAYBACK_ERROR, (WPARAM)playbackGeneration, ++reconnect_attempts);

                                    }

                                    convertedBufferSamples = static_cast<int>(bufferFrameCount * 2);
                                    converted_buffer = (uint8_t*)av_malloc(convertedBufferSamples * pwfx->nChannels * sizeof(float));
                                    packet = av_packet_alloc();
                                    frame = av_frame_alloc();

                                    if (!converted_buffer || !packet || !frame) {
                                       //LogToUI("PlaybackLoop: Failed to reallocate buffers after device change.");
                                        running.store(false);
                                        break;
                                    }
                                    PostFfmpegStatus(L"\u25B7 00:00");
                                    continue;
                                    
                                }
                                else {
                                    //LogToUI("PlaybackLoop: Failed to reinitialize audio system.");
                                    running.store(false);
                                    break;
                                }
                            }
                            else if (FAILED(hr0)) {
                                //LogToUI("PlaybackLoop: GetCurrentPadding failed.");
                                running.store(false);
                                break;
                            }

                            UINT32 available = bufferFrameCount - padding;

                            if (available == 0) {
                                                        
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                continue;
                            }
   
                            UINT32 to_write = std::min(available, (UINT32)(converted_count - samples_written));
                            BYTE* pData = nullptr;

                            HRESULT hr = renderClient->GetBuffer(to_write, &pData);

                            if (SUCCEEDED(hr)) {
                                int bytes_to_copy = to_write * pwfx->nChannels * sizeof(float);
                                int offset = samples_written * pwfx->nChannels * sizeof(float);
                                memcpy(pData, converted_buffer + offset, bytes_to_copy);

                                renderClient->ReleaseBuffer(to_write, 0);

                                if (showcqt_running.load()) {
                                    const float* showcqt_data = audio_data + (size_t)samples_written * pwfx->nChannels;
                                    ProcessAudioForShowCQT(showcqt_data, (int)to_write, pwfx->nSamplesPerSec, pwfx->nChannels);
                                }

                                samples_written += to_write;

                            }
                            else {
                                //LogToUI("PlaybackLoop: renderClient GetBuffer error.");
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            }
                        }

                    }
                    
                }
            }
            
        }

        av_packet_unref(packet);
    }

    PostMessage(g_hMainWnd, WM_APP_UPDATE_ICON, 0, 0);
    // Cleanup

    av_free(converted_buffer);
    av_packet_free(&packet);
    av_frame_free(&frame);

    if (pwfx)
    {
        CoTaskMemFree(pwfx);
        pwfx = nullptr;
    }

    if (renderClient)
        renderClient->Release();

    if (audioClient) {
        audioClient->Stop();
        audioClient->Reset();
        audioClient->Release();
        audioClient = nullptr;
    }
}

void ProcessAudioForShowCQT(const float* audio_data, int samples, int sample_rate, int channels) {
    if (!showcqt_running.load() || !audio_data || samples <= 0 || sample_rate <= 0 || channels <= 0) return;

    std::unique_lock<std::mutex> lock(showcqt_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }

    if (showcqt_sample_rate != sample_rate) {
        showcqt_audio_buffer.clear();
        showcqt_audio_read_pos = 0;
    }

    showcqt_sample_rate = sample_rate;

    const int samples_per_frame = (std::max)(1, (int)((double)sample_rate / SHOWCQT_FPS));
    size_t max_buffer = (size_t)samples_per_frame * SHOWCQT_VIS_CHANNELS * SHOWCQT_BUFFER_FRAMES;
    max_buffer -= max_buffer % SHOWCQT_VIS_CHANNELS;

    const int max_samples = (int)(max_buffer / SHOWCQT_VIS_CHANNELS);
    if (samples > max_samples) {
        audio_data += (size_t)(samples - max_samples) * channels;
        samples = max_samples;
    }

    std::vector<float> stereo_audio;
    const float* cqt_audio = audio_data;
    const size_t cqt_float_count = (size_t)samples * SHOWCQT_VIS_CHANNELS;

    if (channels != SHOWCQT_VIS_CHANNELS) {
        stereo_audio.resize(cqt_float_count);
        for (int i = 0; i < samples; ++i) {
            const float left = audio_data[(size_t)i * channels];
            const float right = (channels > 1) ? audio_data[(size_t)i * channels + 1] : left;
            stereo_audio[(size_t)i * SHOWCQT_VIS_CHANNELS] = left;
            stereo_audio[(size_t)i * SHOWCQT_VIS_CHANNELS + 1] = right;
        }
        cqt_audio = stereo_audio.data();
    }

    if (cqt_float_count >= max_buffer) {
        showcqt_audio_buffer.assign(
            cqt_audio + (cqt_float_count - max_buffer),
            cqt_audio + cqt_float_count
        );
        showcqt_audio_read_pos = 0;
    }
    else {
        showcqt_audio_buffer.insert(showcqt_audio_buffer.end(),
            cqt_audio, cqt_audio + cqt_float_count);

        const size_t available = (showcqt_audio_read_pos < showcqt_audio_buffer.size())
            ? showcqt_audio_buffer.size() - showcqt_audio_read_pos
            : 0;

        if (available > max_buffer) {
            size_t drop = available - max_buffer;
            drop -= drop % SHOWCQT_VIS_CHANNELS;
            showcqt_audio_read_pos += drop;

            showcqt_audio_buffer.erase(
                showcqt_audio_buffer.begin(),
                showcqt_audio_buffer.begin() + showcqt_audio_read_pos
            );
            showcqt_audio_read_pos = 0;
        }
    }
}


// Инициализация showcqt - только запускает поток
void InitShowCQT() {
    if (showcqt_running.load()) {
        return;
    }

    if (showcqt_thread.joinable()) {
        showcqt_thread.join();
    }

    // Теперь безопасно запускать новый поток.
    showcqt_running.store(true);
    showcqt_thread = std::thread(ShowCQTThread);
}

void CleanupShowCQT() {
    showcqt_running.store(false);
}

// Эта функция будет вызываться ИЗ потока ShowCQTTh
static bool InitShowCQTFilterGraph(int sample_rate) {
    if (showcqt_filter_graph) {
        avfilter_graph_free(&showcqt_filter_graph);
        showcqt_filter_graph = nullptr;
        showcqt_buffersrc = nullptr;
        showcqt_buffersink = nullptr;
    }

    showcqt_filter_graph = avfilter_graph_alloc();
    if (!showcqt_filter_graph) {
        return false;
    }

    char filter_descr[512];
    snprintf(filter_descr, sizeof(filter_descr),
        "showcqt=s=%dx%d:fps=%d:sono_h=0:axis_h=0:sono_g=7:"
        "timeclamp=0.3:bar_g=3:sono_v=32,format=yuv420p",
        SHOWCQT_WIDTH, SHOWCQT_HEIGHT, SHOWCQT_FPS);

    const AVFilter* abuffer = avfilter_get_by_name("abuffer");
    char args[512] = { 0 };
    snprintf(args, sizeof(args),
        "sample_rate=%d:sample_fmt=fltp:channel_layout=stereo", sample_rate);

    int ret = avfilter_graph_create_filter(&showcqt_buffersrc, abuffer, "in", args, NULL, showcqt_filter_graph);
    if (ret < 0) {
        avfilter_graph_free(&showcqt_filter_graph);
        showcqt_filter_graph = nullptr;
        return false;
    }

    const AVFilter* buffersink = avfilter_get_by_name("buffersink");
    ret = avfilter_graph_create_filter(&showcqt_buffersink, buffersink, "out", NULL, NULL, showcqt_filter_graph);
    if (ret < 0) {
        avfilter_graph_free(&showcqt_filter_graph);
        showcqt_filter_graph = nullptr;
        showcqt_buffersrc = nullptr;
        return false;
    }

    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = showcqt_buffersrc;
    outputs->pad_idx = 0;
    outputs->next = NULL;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = showcqt_buffersink;
    inputs->pad_idx = 0;
    inputs->next = NULL;

    ret = avfilter_graph_parse_ptr(showcqt_filter_graph, filter_descr, &inputs, &outputs, NULL);
    if (ret < 0) {
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        avfilter_graph_free(&showcqt_filter_graph);
        showcqt_filter_graph = nullptr;
        showcqt_buffersrc = nullptr;
        showcqt_buffersink = nullptr;
        return false;
    }
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    ret = avfilter_graph_config(showcqt_filter_graph, NULL);
    if (ret < 0) {
        avfilter_graph_free(&showcqt_filter_graph);
        showcqt_filter_graph = nullptr;
        showcqt_buffersrc = nullptr;
        showcqt_buffersink = nullptr;
        return false;
    }

    return true;
}
// Поток, который управляет всем жизненным циклом SDL
void ShowCQTThread() {

    HWND hSSdl = GetDlgItem(g_hMainWnd, IDC_STATIC_SDL);

    // создаём SDL-окно без позиционирования
    SDL_Window* showcqt_window = SDL_CreateWindow("ShowCQT Visualization",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        SHOWCQT_WIDTH, SHOWCQT_HEIGHT,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    if (!showcqt_window) {

        showcqt_running.store(false);
        return;
    }

    // получаем HWND SDL-окна
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(showcqt_window, &wmInfo)) {
        // обработка ошибки
    }
    HWND hwndSDL = wmInfo.info.win.window;
    // получаем координаты контрола в экранной системе координат
    RECT rc;
    GetWindowRect(hSSdl, &rc);
    int targetX = rc.left;
    int targetY = rc.top;
    int targetW = rc.right - rc.left;
    int targetH = rc.bottom - rc.top;
    // сделаем SDL-окно дочерним контролу (встраивание)
    SetParent(hwndSDL, hSSdl);
    SetWindowLong(hwndSDL, GWL_STYLE, WS_CHILD | WS_VISIBLE);
    SetWindowPos(hwndSDL, HWND_TOP, 0, 0, targetW, targetH, SWP_HIDEWINDOW);

    ShowWindow(hwndSDL, SW_SHOW);

    showcqt_renderer = SDL_CreateRenderer(showcqt_window, -1, SDL_RENDERER_ACCELERATED);
    if (!showcqt_renderer) {
       
        SDL_DestroyWindow(showcqt_window);
        showcqt_running.store(false);
        return;
    }

    showcqt_texture = SDL_CreateTexture(showcqt_renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, SHOWCQT_WIDTH, SHOWCQT_HEIGHT);
    if (!showcqt_texture) {
       
        SDL_DestroyRenderer(showcqt_renderer);
        SDL_DestroyWindow(showcqt_window);
        showcqt_running.store(false);
        return;
    }

    showcqt_frame = av_frame_alloc();
    if (!showcqt_frame) {
    
        SDL_DestroyTexture(showcqt_texture);
        SDL_DestroyRenderer(showcqt_renderer);
        SDL_DestroyWindow(showcqt_window);
        showcqt_running.store(false);
        return;
    }

    SDL_Event event;
    const double frame_delay = 1000.0 / SHOWCQT_FPS;
    double next_frame_time = (double)SDL_GetTicks();
    int current_initialized_rate = 44100;

    {
        std::lock_guard<std::mutex> lock(showcqt_mutex);
        current_initialized_rate = showcqt_sample_rate;
    }

    if (!InitShowCQTFilterGraph(current_initialized_rate)) {

        showcqt_running.store(false);
        return;
    }

    while (showcqt_running.load()) {

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT ) {
                showcqt_running.store(false);
                break;
            }
        }

        if (!showcqt_running.load()) break;

        // 2. Check if it's time to render a new frame
        const double now = (double)SDL_GetTicks();

        if (now >= next_frame_time) {

            int frame_sample_rate = current_initialized_rate;
            int samples_per_frame = (std::max)(1, (int)((double)frame_sample_rate / SHOWCQT_FPS));
            bool frame_filled = false;
            bool need_reinit = false;

            AVFrame* audio_frame = av_frame_alloc();
            if (audio_frame) {
                audio_frame->format = AV_SAMPLE_FMT_FLTP;
                audio_frame->sample_rate = frame_sample_rate;
                audio_frame->nb_samples = samples_per_frame; // ВАЖНО: ровно один кадр

                AVChannelLayout stereo_layout;
                av_channel_layout_default(&stereo_layout, SHOWCQT_VIS_CHANNELS);
                av_channel_layout_copy(&audio_frame->ch_layout, &stereo_layout);

                if (av_frame_get_buffer(audio_frame, 0) >= 0) {
                    {
                        std::lock_guard<std::mutex> lock(showcqt_mutex);
                        if (showcqt_sample_rate != current_initialized_rate) {
                            current_initialized_rate = showcqt_sample_rate;
                            showcqt_audio_buffer.clear();
                            showcqt_audio_read_pos = 0;
                            need_reinit = true;
                        }
                        else {
                            const size_t floats_per_frame = (size_t)samples_per_frame * SHOWCQT_VIS_CHANNELS;
                            const size_t available = (showcqt_audio_read_pos < showcqt_audio_buffer.size())
                                ? showcqt_audio_buffer.size() - showcqt_audio_read_pos
                                : 0;

                            if (available >= floats_per_frame) {
                                const float* src = showcqt_audio_buffer.data() + showcqt_audio_read_pos;

                                for (int ch = 0; ch < SHOWCQT_VIS_CHANNELS; ch++) {
                                    float* dst = (float*)audio_frame->extended_data[ch];
                                    for (int i = 0; i < samples_per_frame; i++) {
                                        dst[i] = src[(size_t)i * SHOWCQT_VIS_CHANNELS + ch];
                                    }
                                }

                                showcqt_audio_read_pos += floats_per_frame;
                                if (showcqt_audio_read_pos >= showcqt_audio_buffer.size() / 2) {
                                    showcqt_audio_buffer.erase(
                                        showcqt_audio_buffer.begin(),
                                        showcqt_audio_buffer.begin() + showcqt_audio_read_pos
                                    );
                                    showcqt_audio_read_pos = 0;
                                }

                                frame_filled = true;
                            }
                        }
                    }

                    if (need_reinit) {
                        if (!InitShowCQTFilterGraph(current_initialized_rate)) {
                            showcqt_running.store(false);
                        }
                    }
                    else if (frame_filled && showcqt_filter_graph && av_buffersrc_add_frame(showcqt_buffersrc, audio_frame) >= 0) {
                        while (av_buffersink_get_frame(showcqt_buffersink, showcqt_frame) >= 0) {
                            SDL_UpdateYUVTexture(showcqt_texture, NULL,
                                showcqt_frame->data[0], showcqt_frame->linesize[0],
                                showcqt_frame->data[1], showcqt_frame->linesize[1],
                                showcqt_frame->data[2], showcqt_frame->linesize[2]);

                            SDL_RenderClear(showcqt_renderer);
                            SDL_RenderCopy(showcqt_renderer, showcqt_texture, NULL, NULL);
                            SDL_RenderPresent(showcqt_renderer);

                            av_frame_unref(showcqt_frame);
                        }
                    }
                }

                av_channel_layout_uninit(&stereo_layout);
                av_frame_free(&audio_frame);
            }

            next_frame_time += frame_delay;
            if (now > next_frame_time + frame_delay) {
                next_frame_time = now + frame_delay;
            }
        }

        SDL_Delay(1);
    }

    if (showcqt_frame) av_frame_free(&showcqt_frame);
    if (showcqt_filter_graph) avfilter_graph_free(&showcqt_filter_graph);
    if (showcqt_texture) SDL_DestroyTexture(showcqt_texture);
    if (showcqt_renderer) SDL_DestroyRenderer(showcqt_renderer);
    if (showcqt_window) SDL_DestroyWindow(showcqt_window);

    showcqt_window = nullptr;
    showcqt_renderer = nullptr;
    showcqt_texture = nullptr;
    showcqt_frame = nullptr;
    showcqt_filter_graph = nullptr;

}

// Function to manage filters
void UpdateFilterSettings() {

    if (!filterGraph) return;

    float volume = current_volume.load();
    float eq_gain = current_eq_gain.load();
    float eq_gain_bass = current_eq_gain_bass.load();

    // Update volume - pass only numeric value
    char vol_str[32];
    snprintf(vol_str, sizeof(vol_str), "%.2f", volume); // Only number, without "volume="
    int ret = avfilter_graph_send_command(filterGraph, "postVol", "volume", vol_str, nullptr, 0, 0);
    if (ret < 0) {
        //LogToUI("Error changing volume: " + av_error_string(ret));
    }

    // Update equalizer
    char gain_str[32];
    snprintf(gain_str, sizeof(gain_str), "%f", eq_gain);
    ret = avfilter_graph_send_command(filterGraph, "eq", "g", gain_str, nullptr, 0, 0);
    if (ret < 0) {
        //LogToUI("Error changing equalizer: " + av_error_string(ret));
    }

    // Update equalizer bass
    char gain_str_bass[32];
    snprintf(gain_str_bass, sizeof(gain_str_bass), "%f", eq_gain_bass);
    ret = avfilter_graph_send_command(filterGraph, "eq0", "g", gain_str_bass, nullptr, 0, 0);
    if (ret < 0) {
        //LogToUI("Error changing equalizer bass: " + av_error_string(ret));
    }
}


