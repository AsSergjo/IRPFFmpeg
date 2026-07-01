#include "cover_art.h"
#include "IRPFFmpeg.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <wininet.h>

#pragma comment(lib, "wininet.lib")

static bool TryLoadCoverFromCache(const std::wstring& trackName);
static bool DownloadCoverToCurrentAndCache(const std::wstring& imageUrl, const std::wstring& trackName);

static bool is_image_url_valid(const std::wstring& url)
{
    if (url.empty()) return false;

    HINTERNET hInternet = InternetOpen(L"WinAmp/5.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet)
        return false;

    // Set timeouts
    DWORD timeout = 1000;
    InternetSetOption(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOption(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOption(hInternet, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));

    HINTERNET hUrl = InternetOpenUrlW(hInternet, url.c_str(), nullptr, 0,
        INTERNET_FLAG_NO_UI |
        INTERNET_FLAG_NO_CACHE_WRITE |
        INTERNET_FLAG_PRAGMA_NOCACHE |
        INTERNET_FLAG_RELOAD, 0);

    bool isValid = false;

    if (hUrl)
    {
        wchar_t contentType[256] = { 0 };
        DWORD dwSize = sizeof(contentType);
        if (HttpQueryInfoW(hUrl, HTTP_QUERY_CONTENT_TYPE, contentType, &dwSize, nullptr))
        {
            std::wstring ct(contentType);
            std::transform(ct.begin(), ct.end(), ct.begin(), ::towlower);

            if (ct.find(L"image/webp") != std::wstring::npos)
            {
                isValid = false;
            }
            else if (ct.find(L"image/") != std::wstring::npos)
            {
                isValid = true;

                DWORD fileSize = 0;
                DWORD sizeLen = sizeof(fileSize);
                if (HttpQueryInfoW(hUrl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &fileSize, &sizeLen, nullptr))
                {
                    isValid = (fileSize > 20000 && fileSize < 1000000);
                }
            }
        }
        InternetCloseHandle(hUrl);
    }

    InternetCloseHandle(hInternet);

    return isValid;
}

//  HTTP GET через WinINet
static std::string SimpleHttpGet(const std::wstring& url)
{
    HINTERNET hInternet = InternetOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:148.0) Gecko/20100101 Firefox/148.0",
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) return "";

    HINTERNET hConnect = InternetOpenUrlW(
        hInternet, url.c_str(),
        NULL, 0,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE,
        0);
    if (!hConnect) {
        InternetCloseHandle(hInternet);
        return "";
    }

    std::string result;
    char buffer[8192];
    DWORD bytesRead = 0;
    while (InternetReadFile(hConnect, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0)
        result.append(buffer, bytesRead);

    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);
    return result;
}

static std::string url_encode_itunes(const std::string& s)
{
    std::string result;
    result.reserve(s.size() * 3);

    for (unsigned char c : s)
    {
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '*')
        {
            // безопасные символы
            result += (char)c;
        }
        else if (c == ' ')
        {
            // пробел = +
            result += '+';
        }
        else
        {
            // всё остальное
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            result += hex;
        }
    }
    return result;
}

static std::wstring ParseITunesArtwork(const std::string& json)
{
    const std::string key = "\"artworkUrl100\":\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return L"";

    pos += key.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return L"";

    std::string artUrl = json.substr(pos, end - pos);

    // 100x100bb заменяем на 600x600bb
    auto sizePos = artUrl.find("100x100bb");
    if (sizePos != std::string::npos)
        artUrl.replace(sizePos, 9, "600x600bb");

    //LogToUI("iTunes artwork: " + artUrl);
    return utf8_to_wstring(artUrl);
}

// Разбиваем строку на отдельные имена
static std::vector<std::string> SplitArtists(const std::string& s)
{
    std::vector<std::string> result;
    std::string current;
    const size_t n = s.size();

    auto match_ci = [&](size_t pos, const char* token) -> bool {
        size_t len = strlen(token);
        if (pos + len > n) return false;
        for (size_t k = 0; k < len; ++k) {
            if (tolower((unsigned char)s[pos + k]) != tolower((unsigned char)token[k]))
                return false;
        }
        return true;
        };

    for (size_t i = 0; i < n; ++i)
    {
        bool isSep = false;
        size_t skip = 0; // сколько символов дополнительно пропустить

        // Односимвольные разделители
        if (s[i] == '&' || s[i] == ',' || s[i] == '/' || s[i] == '+')
        {
            isSep = true;
        }
        else
        {
            // многосимвольные разделители (case-insensitive)
            if (match_ci(i, "feat.")) { isSep = true; skip = 5 - 1; } // "feat."
            else if (match_ci(i, "feat ")) { isSep = true; skip = 5 - 1; } // "feat "
            else if (match_ci(i, "ft.")) { isSep = true; skip = 3 - 1; } // "ft."
            else if (match_ci(i, "ft ")) { isSep = true; skip = 3 - 1; } // "ft "
            else if (match_ci(i, "vs.")) {
                // допускаем "vs." как разделитель, но требуем пробел слева или справа (чтобы не резать слова)
                bool leftSpace = (i > 0 && s[i - 1] == ' ');
                bool rightSpace = (i + 3 < n && s[i + 3] == ' ');
                if (leftSpace || rightSpace) { isSep = true; skip = 3 - 1; }
            }
            else if (match_ci(i, "vs")) {
                // " vs " или "vs" окружённый пробелами
                if ((i > 0 && s[i - 1] == ' ') &&
                    (i + 2 == n || (i + 2 < n && s[i + 2] == ' '))) {
                    isSep = true; skip = 2 - 1;
                }
            }
            else if (s[i] == 'x' || s[i] == 'X') {
                // " x " как разделитель: проверяем границы
                if (i > 0 && i + 1 < n && s[i - 1] == ' ' && s[i + 1] == ' ')
                {
                    isSep = true;
                }
            }
                  
        }

        if (isSep)
        {
            // trim current
            size_t start = current.find_first_not_of(" \t");
            size_t end = current.find_last_not_of(" \t");
            if (start != std::string::npos)
                result.push_back(current.substr(start, end - start + 1));
            current.clear();

            // пропускаем дополнительные символы внутри разделителя
            if (skip > 0) {
                // убедимся, что безопасно увеличить i (skip уже <= n)
                size_t add = static_cast<size_t>(skip);
                i += add; // цикл прибавит ещё 1
            }
        }
        else
        {
            current += s[i];
        }
    }

    // последний токен
    size_t start = current.find_first_not_of(" \t");
    size_t end = current.find_last_not_of(" \t");
    if (start != std::string::npos)
        result.push_back(current.substr(start, end - start + 1));

    return result;
}

static bool IsArtistWordChar(unsigned char c)
{
    return std::isalnum(c) || c >= 0x80;
}

static bool ContainsArtistPhrase(const std::string& text,
    const std::string& phrase)
{
    if (phrase.empty() || text.size() < phrase.size())
        return false;

    size_t pos = 0;
    while ((pos = text.find(phrase, pos)) != std::string::npos) {
        const bool leftBoundary = (pos == 0) ||
            !IsArtistWordChar(static_cast<unsigned char>(text[pos - 1]));
        const size_t end = pos + phrase.size();
        const bool rightBoundary = (end >= text.size()) ||
            !IsArtistWordChar(static_cast<unsigned char>(text[end]));

        if (leftBoundary && rightBoundary)
            return true;

        ++pos;
    }

    return false;
}

static bool is_artist_part_match(const std::string& foundPart,
    const std::string& searchPart)
{
    if (foundPart == searchPart)
        return true;

    // Явное совпадение фразы по границам слов: "don omar" совпадёт
    // с "don omar lucenzo", но "don" не совпадёт с "madonna".
    return ContainsArtistPhrase(foundPart, searchPart) ||
        ContainsArtistPhrase(searchPart, foundPart);
}

static bool MatchArtists(const std::string& foundArtistLower,
    const std::string& searchArtistLower)
{
    // Быстрая проверка: полное совпадение
    if (foundArtistLower == searchArtistLower) return true;

    // Разбиваем оба на отдельные имена
    auto foundParts = SplitArtists(foundArtistLower);
    auto searchParts = SplitArtists(searchArtistLower);

    if (foundParts.empty() || searchParts.empty())
        return false;

    constexpr size_t kMinArtistTokenLen = 2;

    // Первый артист должен совпасть явно: точно или как фраза на границах слов.
    const std::string* primarySearchArtist = nullptr;
    for (const auto& sp : searchParts) {
        if (sp.size() >= kMinArtistTokenLen) {
            primarySearchArtist = &sp;
            break;
        }
    }

    if (!primarySearchArtist)
        return false;

    auto has_explicit_artist_match = [&](const std::string& searchPart) -> bool {
        for (const auto& fp : foundParts) {
            if (is_artist_part_match(fp, searchPart))
                return true;
        }
        return false;
        };

    if (!has_explicit_artist_match(*primarySearchArtist))
        return false;

    // Второй и остальные значимые артисты тоже должны совпасть явно.
    for (const auto& sp : searchParts)
    {
        if (&sp == primarySearchArtist)
            continue;

        if (sp.size() < kMinArtistTokenLen)
            continue; // игнорируем однобуквенные токены

        if (!has_explicit_artist_match(sp))
            return false;
    }

    return true;
}

static std::wstring ParseITunesArtworkFiltered(const std::string& json,
    const std::wstring& artistName,
    const std::wstring& trackName)
{
    const std::string trackKey = "\"trackName\":\"";
    const std::string artistKey = "\"artistName\":\"";
    const std::string artworkKey = "\"artworkUrl100\":\"";

    std::string artistLower = wstring_to_utf8(artistName);
    std::string trackLower = wstring_to_utf8(trackName);
    std::transform(artistLower.begin(), artistLower.end(), artistLower.begin(), ::tolower);
    std::transform(trackLower.begin(), trackLower.end(), trackLower.begin(), ::tolower);

    //LogToUI(json) ;

    std::string::size_type pos = 0;
    while ((pos = json.find(trackKey, pos)) != std::string::npos)
    {
        // Ищем начало объекта { — идём назад от trackName
        auto objStart = json.rfind('{', pos);
        if (objStart == std::string::npos) { pos++; continue; }

        // Ищем конец объекта } — идём вперёд от trackName
        auto objEnd = json.find('}', pos);
        if (objEnd == std::string::npos) break;

        // Вся запись целиком включая artworkUrl100 которая может быть до trackName
        std::string record = json.substr(objStart, objEnd - objStart + 1);

        // Извлекаем trackName
        std::string foundTrack;
        {
            auto p = record.find(trackKey);
            if (p == std::string::npos) { pos++; continue; }
            p += trackKey.size();
            auto e = record.find('"', p);
            if (e == std::string::npos) { pos++; continue; }
            foundTrack = record.substr(p, e - p);
        }

        // Извлекаем artistName
        std::string foundArtist;
        {
            auto p = record.find(artistKey);
            if (p != std::string::npos)
            {
                p += artistKey.size();
                auto e = record.find('"', p);
                if (e != std::string::npos)
                    foundArtist = record.substr(p, e - p);
            }
        }

        std::string foundTrackLower = foundTrack;
        std::string foundArtistLower = foundArtist;
        std::transform(foundTrackLower.begin(), foundTrackLower.end(),
            foundTrackLower.begin(), ::tolower);
        std::transform(foundArtistLower.begin(), foundArtistLower.end(),
            foundArtistLower.begin(), ::tolower);

        //LogToUI("iTunes кандидат: [" + foundArtist + "] - [" + foundTrack + "]");

        bool artistMatch = MatchArtists(foundArtistLower, artistLower);

        if (artistMatch)
        {
            auto artPos = record.find(artworkKey);
            if (artPos != std::string::npos)
            {
                artPos += artworkKey.size();
                auto artEnd = record.find('"', artPos);
                if (artEnd != std::string::npos)
                {
                    std::string artUrl = record.substr(artPos, artEnd - artPos);
                    auto sizePos = artUrl.find("100x100bb");
                    if (sizePos != std::string::npos)
                        artUrl.replace(sizePos, 9, "600x600bb");
                    //LogToUI("iTunes matched: [" + foundArtist + "] - [" + foundTrack + "]");
                    return utf8_to_wstring(artUrl);
                }
            }
        }

        pos = objEnd + 1; // переходим к следующей записи
    }
    return L"";
}

static std::wstring TryITunes(const std::wstring& trackName)
{
    auto sep = trackName.find(L" - ");
    if (sep == std::wstring::npos)
    {
        // Нет разделителя - ищем всю строку как название песни
        std::string encoded = url_encode_itunes(wstring_to_utf8(trackName));

        std::string url = "https://itunes.apple.com/search?term=" + encoded +
			"&media=music&entity=song&limit=1"; //первую же запись берём без фильтрации, т.к. нет данных для фильтра
        return ParseITunesArtwork(SimpleHttpGet(utf8_to_wstring(url)));
    }
 
    std::wstring artistW = trackName.substr(0, sep);
    std::wstring trackW = trackName.substr(sep + 3);

    //LogToUI("TryITunes: " + wstring_to_utf8(trackName));

    std::string trackEncoded = url_encode_itunes(wstring_to_utf8(trackW));

    std::string url = "https://itunes.apple.com/search?term=" + trackEncoded +
        "&media=music&entity=song&attribute=songTerm&limit=25";

    std::string json = SimpleHttpGet(utf8_to_wstring(url));
    if (json.empty()) return L"";

    return ParseITunesArtworkFiltered(json, artistW, trackW);
}

//вспомогательная гп для проверки повторяющихся URL из Bing Images
static std::string OldUrlImage;

static constexpr uintmax_t COVER_CACHE_MAX_BYTES = 30ull * 1024ull * 1024ull;
static const wchar_t* COVER_CACHE_DIR = L"cover_cache";
static const wchar_t* CURRENT_COVER_FILE = L"cover_cache\\cover.jpg";

static bool TryBingImages(const std::wstring& trackName)
{        
    std::string encoded = url_encode(wstring_to_utf8(trackName));
    
    std::string url = "https://www.bing.com/images/search?q=single%20or%20album%20cover%20" + encoded + "&form=HDRSC3&first=1&qft=+filterui:aspect-square";

    //LogToUI("BingImages for url: " + url);

    std::wstring headers =
        L"Accept: text/html,application/xhtml+xml\r\n"
        L"Accept-Language: en-US,en;q=0.9\r\n";

    HINTERNET hInternet = InternetOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:148.0) Gecko/20100101 Firefox/255.0",
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) return false;

    HINTERNET hConnect = InternetOpenUrlW(
        hInternet, utf8_to_wstring(url).c_str(),
        headers.c_str(), (DWORD)-1,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE,
        0);
    if (!hConnect) { InternetCloseHandle(hInternet); return false; }

    std::string html;
    char buffer[8192];
    DWORD bytesRead = 0;
    while (InternetReadFile(hConnect, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0)
        html.append(buffer, bytesRead);

    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);

    if (html.empty()) return false;

    //LogToUI(html);

    const std::string key = "mediaurl=";
    std::string::size_type pos = 0;

    while ((pos = html.find(key, pos)) != std::string::npos)
    {
        pos += key.size();

        auto end = html.find("&", pos);
        if (end == std::string::npos) break;

        // URL-decode
        std::string encodedUrl = html.substr(pos, end - pos);
        std::string decoded;
        decoded.reserve(encodedUrl.size());
        for (size_t i = 0; i < encodedUrl.size(); ++i)
        {
            if (encodedUrl[i] == '%' && i + 2 < encodedUrl.size())
            {
                char hex[3] = { encodedUrl[i + 1], encodedUrl[i + 2], 0 };
                decoded += (char)strtol(hex, nullptr, 16);
                i += 2;
            }
            else if (encodedUrl[i] == '+') decoded += ' ';
            else                           decoded += encodedUrl[i];
        }

        if (decoded.size() < 10) { pos++; continue; }

        if (OldUrlImage == decoded) {
            pos++;
            continue;
        }

        OldUrlImage = decoded;

        std::wstring decodedUrlW = utf8_to_wstring(decoded);

        if (decodedUrlW.find(L"i.ytimg.com") != std::wstring::npos) { pos++; continue; }
        if (decodedUrlW.find(L"youtube.com") != std::wstring::npos) { pos++; continue; }
        if (decodedUrlW.find(L"archive.org") != std::wstring::npos) { pos++; continue; }
        if (decodedUrlW.find(L"bing.com") != std::wstring::npos) { pos++; continue; }

        //LogToUI(wstring_to_utf8(decodedUrlW));

        if (is_image_url_valid(decodedUrlW))
        {
            if (DownloadCoverToCurrentAndCache(decodedUrlW, trackName))
            {
                //LogToUI(wstring_to_utf8(trackName));
                PostMessage(g_hMainWnd, WM_APP_COVER_DOWNLOADED, 0, 0);
                return true;  // нашли и скачали - выходим
            }
        }

        pos++;
    }

    return false; // ни один не подошёл
}

// Основной поток
void GetImageUrlThread()
{
    g_bIsImageUrlThreadRunning = true;

    std::wstring trackName;
    EnterCriticalSection(&g_url_vec_cs);
    if (vec_url.empty()) {
        LeaveCriticalSection(&g_url_vec_cs);
        g_bIsImageUrlThreadRunning = false;
        return;
    }
    trackName = vec_url.back();
    vec_url.clear();
    LeaveCriticalSection(&g_url_vec_cs);

    if (trackName.empty()) {
        g_bIsImageUrlThreadRunning = false;
        return;
    }
    //ищем сначала в кеше
    if (TryLoadCoverFromCache(trackName)) {
        //LogToUI("Loaded cover from cache for: " + wstring_to_utf8(trackName));
        PostMessage(g_hMainWnd, WM_APP_COVER_DOWNLOADED, 0, 0);
        g_bIsImageUrlThreadRunning = false;
        return;
    }
    //Trying iTunes...
    std::wstring artUrlW = TryITunes(trackName);

    if (!artUrlW.empty())
    {
        if (is_image_url_valid(artUrlW))
        {
            if (DownloadCoverToCurrentAndCache(artUrlW, trackName))
            {
                PostMessage(g_hMainWnd, WM_APP_COVER_DOWNLOADED, 0, 0);
                g_bIsImageUrlThreadRunning = false;
                return;
            }
        }
    }

    //Bing Images
    if (!TryBingImages(trackName))
    {
        //todo показываем дефолтную картинку, вместо скачивания с интернета.
        //LogToUI(wstring_to_utf8(trackName));
    }

    g_bIsImageUrlThreadRunning = false;
}

static std::wstring TrimWideCopy(const std::wstring& text)
{
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
        return L"";

    const size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

static std::wstring NormalizeCoverCacheKey(const std::wstring& trackName)
{
    std::wstring key = TrimWideCopy(trackName);
    const size_t sep = key.find(L" - ");
    if (sep != std::wstring::npos) {
        const std::wstring artist = TrimWideCopy(key.substr(0, sep));
        const std::wstring title = TrimWideCopy(key.substr(sep + 3));
        if (!artist.empty() && !title.empty()) {
            key = artist + L" - " + title;
        }
    }

    bool prevSpace = false;
    std::wstring normalized;
    normalized.reserve(key.size());
    for (wchar_t ch : key) {
        if (std::iswspace(ch)) {
            if (!prevSpace) {
                normalized.push_back(L' ');
                prevSpace = true;
            }
            continue;
        }

        normalized.push_back(static_cast<wchar_t>(std::towlower(ch)));
        prevSpace = false;
    }

    return TrimWideCopy(normalized);
}

static uint64_t Fnv1a64Wide(const std::wstring& text)
{
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t ch : text) {
        for (size_t i = 0; i < sizeof(wchar_t); ++i) {
            const auto byte = static_cast<unsigned char>((static_cast<unsigned int>(ch) >> (i * 8)) & 0xff);
            hash ^= byte;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

static std::filesystem::path MakeCoverCachePath(const std::wstring& trackName)
{
    const std::wstring key = NormalizeCoverCacheKey(trackName);

    wchar_t hashBuf[17] = {};
    swprintf_s(hashBuf, L"%016llx", static_cast<unsigned long long>(Fnv1a64Wide(key)));

    return std::filesystem::path(COVER_CACHE_DIR) / hashBuf;
}

static bool EnsureCoverCacheDirectory()
{
    std::error_code ec;
    std::filesystem::create_directories(COVER_CACHE_DIR, ec);
    return !ec && std::filesystem::is_directory(COVER_CACHE_DIR, ec);
}

static bool IsRegularNonEmptyFile(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) &&
        std::filesystem::file_size(path, ec) > 0 && !ec;
}

static bool IsCoverCacheServiceFile(const std::filesystem::path& path)
{
    const std::wstring filename = path.filename().wstring();
    return filename == L"cover.jpg" ||
        filename == L"cache.dat" ||
        filename == L"cache.dat.tmp" ||
        filename.rfind(L"cover.tmp.", 0) == 0;
}

struct CoverCacheRecord {
    std::wstring filename;
    uintmax_t size = 0;
    long long created = 0;
};

static std::map<std::wstring, CoverCacheRecord> g_coverCacheIndex;
static std::mutex g_coverCacheIndexMutex;
static bool g_coverCacheIndexLoaded = false;
static bool g_coverCacheIndexDirty = false;
static bool g_clearCoverCacheOnExit = false;

static long long FileTimeToUnixTime(const FILETIME& fileTime)
{
    ULARGE_INTEGER value;
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;

    static const unsigned long long WINDOWS_TICK = 10000000ULL;
    static const unsigned long long SEC_TO_UNIX_EPOCH = 11644473600ULL;
    const unsigned long long seconds = value.QuadPart / WINDOWS_TICK;
    if (seconds <= SEC_TO_UNIX_EPOCH)
        return 0;

    return static_cast<long long>(seconds - SEC_TO_UNIX_EPOCH);
}

static bool TryGetFileCreationTime(const std::filesystem::path& path, long long& created)
{
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (GetFileAttributesExW(path.wstring().c_str(), GetFileExInfoStandard, &data)) {
        created = FileTimeToUnixTime(data.ftCreationTime);
        return created > 0;
    }

    return false;
}

static long long GetFileCreationTimeOrNow(const std::filesystem::path& path)
{
    long long created = 0;
    return TryGetFileCreationTime(path, created) ? created : static_cast<long long>(std::time(nullptr));
}

static void RebuildCoverCacheIndexFromDiskLocked()
{
    if (!EnsureCoverCacheDirectory())
        return;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(COVER_CACHE_DIR, ec)) {
        if (ec)
            break;

        std::error_code itemEc;
        if (!entry.is_regular_file(itemEc) || itemEc)
            continue;

        if (IsCoverCacheServiceFile(entry.path()))
            continue;

        const uintmax_t size = entry.file_size(itemEc);
        if (itemEc || size == 0)
            continue;

        CoverCacheRecord record;
        record.filename = entry.path().filename().wstring();
        record.size = size;
        record.created = GetFileCreationTimeOrNow(entry.path());
        g_coverCacheIndex[record.filename] = record;
    }

    g_coverCacheIndexDirty = true;
}

static void LoadCoverCacheIndexLocked()
{
    if (g_coverCacheIndexLoaded)
        return;

    g_coverCacheIndexLoaded = true;
    if (!EnsureCoverCacheDirectory())
        return;

    std::ifstream file(std::filesystem::path(COVER_CACHE_DIR) / L"cache.dat", std::ios::binary);
    if (!file.is_open()) {
        RebuildCoverCacheIndexFromDiskLocked();
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        std::istringstream lineStream(line);
        std::string filename;
        uintmax_t size = 0;
        long long created = 0;
        if (!(lineStream >> filename >> size >> created))
            continue;

        std::wstring wFilename = utf8_to_wstring(filename);
        const std::filesystem::path cachePath = std::filesystem::path(COVER_CACHE_DIR) / wFilename;
        if (IsCoverCacheServiceFile(cachePath) || !IsRegularNonEmptyFile(cachePath))
            continue;

        std::error_code sizeEc;
        const uintmax_t fileSize = std::filesystem::file_size(cachePath, sizeEc);
        const uintmax_t actualSize = sizeEc ? 0 : fileSize;
        if (actualSize == 0)
            continue;

        CoverCacheRecord record;
        record.filename = wFilename;
        record.size = actualSize;
        record.created = created > 0 ? created : GetFileCreationTimeOrNow(cachePath);
        g_coverCacheIndex[record.filename] = record;
    }

    if (g_coverCacheIndex.empty()) {
        RebuildCoverCacheIndexFromDiskLocked();
    }
}

static void EnforceCoverCacheLimitLocked()
{
    uintmax_t totalSize = 0;
    for (const auto& pair : g_coverCacheIndex) {
        totalSize += pair.second.size;
    }

    while (totalSize > COVER_CACHE_MAX_BYTES && !g_coverCacheIndex.empty()) {
        auto oldest = std::min_element(g_coverCacheIndex.begin(), g_coverCacheIndex.end(),
            [](const auto& a, const auto& b) {
                if (a.second.created != b.second.created)
                    return a.second.created < b.second.created;
                return a.second.filename < b.second.filename;
            });

        if (oldest == g_coverCacheIndex.end())
            break;

        const CoverCacheRecord record = oldest->second;
        const std::filesystem::path cachePath = std::filesystem::path(COVER_CACHE_DIR) / record.filename;

        std::error_code removeEc;
        const bool removed = std::filesystem::remove(cachePath, removeEc);
        if (removeEc) {
            break;
        }

        std::error_code existsEc;
        const bool exists = std::filesystem::exists(cachePath, existsEc);
        if (removed || (!exists && !existsEc)) {
            totalSize = (record.size > totalSize) ? 0 : totalSize - record.size;
            g_coverCacheIndex.erase(oldest);
            g_coverCacheIndexDirty = true;
        }
    }
}

static void SynchronizeCoverCacheIndexWithDiskLocked()
{
    if (!EnsureCoverCacheDirectory())
        return;

    std::map<std::wstring, CoverCacheRecord> diskIndex;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(COVER_CACHE_DIR, ec)) {
        if (ec)
            break;

        std::error_code itemEc;
        if (!entry.is_regular_file(itemEc) || itemEc)
            continue;

        if (IsCoverCacheServiceFile(entry.path()))
            continue;

        const uintmax_t size = entry.file_size(itemEc);
        if (itemEc || size == 0)
            continue;

        const std::wstring filename = entry.path().filename().wstring();
        auto known = g_coverCacheIndex.find(filename);

        CoverCacheRecord record;
        if (known != g_coverCacheIndex.end()) {
            record = known->second;
        }
        else {
            record.filename = filename;
            record.created = GetFileCreationTimeOrNow(entry.path());
            g_coverCacheIndexDirty = true;
        }

        record.filename = filename;

        long long created = 0;
        if (TryGetFileCreationTime(entry.path(), created) && record.created != created) {
            record.created = created;
            g_coverCacheIndexDirty = true;
        }
        if (record.size != size) {
            record.size = size;
            g_coverCacheIndexDirty = true;
        }

        diskIndex[filename] = record;
    }

    if (diskIndex.size() != g_coverCacheIndex.size()) {
        g_coverCacheIndexDirty = true;
    }

    g_coverCacheIndex.swap(diskIndex);
    EnforceCoverCacheLimitLocked();
}

static void ClearCoverCacheFilesLocked()
{
    if (!EnsureCoverCacheDirectory())
        return;

    bool changed = false;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(COVER_CACHE_DIR, ec)) {
        if (ec)
            break;

        std::error_code itemEc;
        if (!entry.is_regular_file(itemEc) || itemEc)
            continue;

        const std::wstring filename = entry.path().filename().wstring();
        if (filename == L"cover.jpg")
            continue;

        std::error_code removeEc;
        const bool removed = std::filesystem::remove(entry.path(), removeEc);
        if (removed && !removeEc)
            changed = true;
    }

    if (!g_coverCacheIndex.empty())
        changed = true;

    g_coverCacheIndex.clear();
    if (changed)
        g_coverCacheIndexDirty = true;
}

static void WriteCoverCacheIndexLocked()
{
    const std::filesystem::path tempPath = std::filesystem::path(COVER_CACHE_DIR) / L"cache.dat.tmp";
    const std::filesystem::path indexPath = std::filesystem::path(COVER_CACHE_DIR) / L"cache.dat";

    std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return;

    for (const auto& pair : g_coverCacheIndex) {
        const CoverCacheRecord& record = pair.second;
        if (record.filename.empty() || record.size == 0)
            continue;

        file << wstring_to_utf8(record.filename) << '\t'
            << record.size << '\t'
            << record.created << '\n';
    }

    file.close();

    std::error_code ec;
    std::filesystem::remove(indexPath, ec);
    ec.clear();
    std::filesystem::rename(tempPath, indexPath, ec);
    if (!ec) {
        g_coverCacheIndexDirty = false;
    }
}

void SaveCoverCacheIndex()
{
    std::lock_guard<std::mutex> lock(g_coverCacheIndexMutex);
    if (!g_coverCacheIndexLoaded && !g_clearCoverCacheOnExit)
        return;

    LoadCoverCacheIndexLocked();
    SynchronizeCoverCacheIndexWithDiskLocked();
    WriteCoverCacheIndexLocked();
}

void SetClearCoverCacheOnExit(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_coverCacheIndexMutex);
    g_clearCoverCacheOnExit = enabled;
}

bool IsClearCoverCacheOnExitEnabled()
{
    std::lock_guard<std::mutex> lock(g_coverCacheIndexMutex);
    return g_clearCoverCacheOnExit;
}

void ClearCoverCacheNow()
{
    std::lock_guard<std::mutex> lock(g_coverCacheIndexMutex);
    ClearCoverCacheFilesLocked();
    WriteCoverCacheIndexLocked();
}

static void UpdateCoverCacheRecord(const std::filesystem::path& cachePath, uintmax_t size)
{
    if (size == 0 || IsCoverCacheServiceFile(cachePath))
        return;

    std::lock_guard<std::mutex> lock(g_coverCacheIndexMutex);
    LoadCoverCacheIndexLocked();

    const std::wstring filename = cachePath.filename().wstring();

    CoverCacheRecord& record = g_coverCacheIndex[filename];
    if (record.filename.empty()) {
        record.filename = filename;
        record.created = GetFileCreationTimeOrNow(cachePath);
        g_coverCacheIndexDirty = true;
    }

    long long created = 0;
    if (TryGetFileCreationTime(cachePath, created) && record.created != created) {
        record.created = created;
        g_coverCacheIndexDirty = true;
    }

    if (record.size != size) {
        record.size = size;
        g_coverCacheIndexDirty = true;
    }
}

static bool CopyFileOverwriteLocked(const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
    std::lock_guard<std::mutex> lock(g_coverFileMutex);
    std::error_code ec;
    std::filesystem::copy_file(source, destination,
        std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}

static bool TryLoadCoverFromCache(const std::wstring& trackName)
{
    if (!EnsureCoverCacheDirectory())
        return false;

    const std::filesystem::path cachePath = MakeCoverCachePath(trackName);
    if (!IsRegularNonEmptyFile(cachePath))
        return false;

    if (!CopyFileOverwriteLocked(cachePath, CURRENT_COVER_FILE))
        return false;

    return true;
}

static void SaveDownloadedCoverToCache(const std::filesystem::path& downloadedPath,
    const std::wstring& trackName)
{
    if (!EnsureCoverCacheDirectory() || !IsRegularNonEmptyFile(downloadedPath))
        return;

    const std::filesystem::path cachePath = MakeCoverCachePath(trackName);

    std::error_code ec;
    std::filesystem::copy_file(downloadedPath, cachePath,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        return;

    std::error_code sizeEc;
    const uintmax_t fileSize = std::filesystem::file_size(cachePath, sizeEc);
    UpdateCoverCacheRecord(cachePath, sizeEc ? 0 : fileSize);

    {
        std::lock_guard<std::mutex> lock(g_coverCacheIndexMutex);
        LoadCoverCacheIndexLocked();
        EnforceCoverCacheLimitLocked();
    }

    {
        std::lock_guard<std::mutex> lock(g_coverCacheIndexMutex);
        if (g_coverCacheIndexLoaded) {
            WriteCoverCacheIndexLocked();
        }
    }
}

static bool DownloadCoverToCurrentAndCache(const std::wstring& imageUrl,
    const std::wstring& trackName)
{
    if (!EnsureCoverCacheDirectory())
        return false;

    std::wstring tempName = L"cover.tmp.";
    tempName += std::to_wstring(GetCurrentProcessId());
    tempName += L".";
    tempName += std::to_wstring(GetCurrentThreadId());
    const std::filesystem::path tempPath = std::filesystem::path(COVER_CACHE_DIR) / tempName;

    std::error_code ec;
    std::filesystem::remove(tempPath, ec);

    if (download_file(imageUrl, tempPath.wstring(), 1000) != S_OK ||
        !IsRegularNonEmptyFile(tempPath)) {
        std::filesystem::remove(tempPath, ec);
        return false;
    }

    SDL_Surface* surface = IMG_Load(wstring_to_utf8(tempPath.wstring()).c_str());
    if (!surface) {
        std::filesystem::remove(tempPath, ec);
        return false;
    }
    SDL_FreeSurface(surface);

    if (!CopyFileOverwriteLocked(tempPath, CURRENT_COVER_FILE)) {
        std::filesystem::remove(tempPath, ec);
        return false;
    }

    SaveDownloadedCoverToCache(tempPath, trackName);
    std::filesystem::remove(tempPath, ec);
    return true;
}




