#include "language_manager.h"
#include "util.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

std::wstring g_languageId = L"russian";

static std::map<std::string, std::wstring> g_languageStrings;
static std::vector<LanguageOption> g_languageOptions;
static std::mutex g_languageMutex;

static constexpr uintmax_t kMaxLanguageFileBytes = 256 * 1024;
static constexpr size_t kMaxLanguageLineBytes = 4096;
static constexpr size_t kMaxLanguageEntries = 512;
static constexpr size_t kMaxLanguageKeyBytes = 128;
static constexpr size_t kMaxLanguageValueBytes = 3072;
static constexpr size_t kMaxLanguageIdChars = 64;
static constexpr size_t kMaxLanguageFiles = 128;

static std::string TrimLanguageAscii(std::string value)
{
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

static bool IsValidLanguageId(const std::wstring& id)
{
    if (id.empty() || id.size() > kMaxLanguageIdChars || id == L"." || id == L"..") {
        return false;
    }

    for (wchar_t ch : id) {
        const bool isAsciiLetter = (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
        const bool isDigit = (ch >= L'0' && ch <= L'9');
        if (!isAsciiLetter && !isDigit && ch != L'_' && ch != L'-') {
            return false;
        }
    }

    return true;
}

static bool IsValidLanguageKey(const std::string& key)
{
    if (key.empty() || key.size() > kMaxLanguageKeyBytes) {
        return false;
    }

    for (unsigned char ch : key) {
        const bool isAsciiLetter = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        const bool isDigit = (ch >= '0' && ch <= '9');
        if (!isAsciiLetter && !isDigit && ch != '_' && ch != '-' && ch != '.') {
            return false;
        }
    }

    return true;
}

static std::filesystem::path GetExeDirectory()
{
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return std::filesystem::current_path();
    }

    return std::filesystem::path(path).parent_path();
}

static std::filesystem::path GetLanguageDirectory()
{
    return GetExeDirectory() / L"Language";
}

static const char* GetDefaultRussianLanguageText()
{
    return
        "language.name=Русский\n"
        "settings.title=Настройки\n"
        "settings.ok=OK\n"
        "settings.group.recording= Режимы записи \n"
        "settings.group.effects= Эффекты воспроизведения \n"
        "settings.group.program= Настройки программы \n"
        "settings.language=Язык:\n"
        "settings.recording.mp3= MP3, LAME, 320 kbit/sec\n"
        "settings.recording.flac= FLAC, s16, ~1000 kbit/sec\n"
        "settings.effect.stereo_width= Расширение Стерео\n"
        "settings.effect.exciter= Exciter / Яркость\n"
        "settings.effect.deep_bass= DeepBass / Глубокий Бас\n"
        "settings.effect.dynamic_auto_volume= Динамическая Регулировка Усиления\n"
        "settings.effect.gain_rider= GainRider / Контроль Пиков\n"
        "settings.program.minimize_to_tray= При минимизации отправлять в трей\n"
        "settings.program.show_track_toast= В трее показывать обложку при смене трека\n"
        "tooltip.play_stop=Воспроизвести или остановить текущую станцию\n"
        "tooltip.prev_station=Перейти к предыдущей станции в списке\n"
        "tooltip.next_station=Перейти к следующей станции в списке\n"
        "tooltip.previous_station=Вернуться к ранее звучавшей станции\n"
        "tooltip.record=Начать или остановить запись текущего потока\n"
        "tooltip.settings=Открыть настройки программы\n"
        "tooltip.slider.bass=Низкие Частоты\n"
        "tooltip.slider.treble=Высокие Частоты\n"
        "tooltip.slider.volume=Громкость\n"
        "tooltip.recording.mp3=Записывать поток в MP3 320 kbit/sec\n"
        "tooltip.recording.flac=Записывать поток в FLAC без потерь\n"
        "tooltip.effect.stereo_width=Включить расширение стереобазы\n"
        "tooltip.effect.exciter=Добавить яркость и выразительность верхним частотам\n"
        "tooltip.effect.deep_bass=Усилить глубину низких частот\n"
        "tooltip.effect.dynamic_auto_volume=Автоматически выравнивать громкость воспроизведения\n"
        "tooltip.effect.gain_rider=Контролировать пики и удерживать комфортный уровень сигнала\n"
        "tooltip.program.minimize_to_tray=При нажатии кнопки свернуть прятать программу в трей\n"
        "tooltip.program.show_track_toast=Когда программа в трее, показывать обложку при смене трека\n"
        "tooltip.settings.ok=Сохранить настройки и закрыть окно\n"
        "tooltip.about.ok=Закрыть окно информации о программе\n"
        "tooltip.add.ok=Добавить станцию в список\n"
        "tooltip.add.cancel=Закрыть окно без добавления станции\n"
        "tray.balloon.title=IRP+ffmpeG работает в трее\n"
        "tray.balloon.text=Дважды щелкните значок, чтобы вернуть окно. Для выхода используйте меню трея.\n"
        "tray.restore=Открыть IRP+ffmpeG\n"
        "tray.play=Воспроизвести\n"
        "tray.stop=Остановить\n"
        "tray.prev=Предыдущая станция\n"
        "tray.next=Следующая станция\n"
        "tray.previous=Вернуться к прошлой станции\n"
        "tray.settings=Настройки\n"
        "tray.about=О программе\n"
        "tray.exit=Выход\n"
        "context.reload_m3u=Обновить m3u\n"
        "context.add_station=Добавить станцию\n"
        "context.edit_station=Изменить название станции\n"
        "context.save_station=Сохранить радиостанцию\n"
        "context.delete_station=Удалить станцию\n"
        "edit_station.title=Изменить название станции\n"
        "edit_station.msg.enter_name=Введите название станции.\n"
        "edit_station.msg.one_line=Название станции должно быть записано в одну строку.\n"
        "edit_station.msg.too_long=Название станции не должно быть длиннее 128 символов.\n"
        "edit_station.msg.create_failed=Не удалось создать форму изменения станции.\n"
        "edit_station.msg.open_failed=Не удалось открыть форму изменения станции.\n"
        "add.title=Добавить станцию\n"
        "add.name_label=Название станции:\n"
        "add.url_label=URL - адрес:\n"
        "common.ok=OK\n"
        "common.cancel=Отмена\n"
        "add.msg.enter_name=Введите название станции.\n"
        "add.msg.one_line=Название и URL должны быть записаны в одну строку.\n"
        "add.msg.enter_url=Введите интернет адрес станции.\n"
        "add.msg.invalid_url=URL должен начинаться с http:// или https://\n"
        "add.msg.invalid_supported_url=Некорректный URL. Поддерживаются адреса вида http://, https://, icy://, mms:// или rtsp:// без пробелов.\n"
        "add.msg.duplicate_url=Станция с таким URL уже есть в плейлисте.\n"
        "add.msg.create_failed=Не удалось создать форму добавления станции.\n"
        "add.msg.open_failed=Не удалось открыть форму добавления станции.\n"
        "nowplaying.no_data=Нет данных о треке\n"
        "status.stopped=Остановлено\n"
        "status.http_ok=Чтение заголовков (HTTP/1.1 200 OK)\n"
        "status.ffmpeg_reconnect=FFmpeg: переподключение к потоку...\n"
        "status.ffmpeg_timeout=FFmpeg: таймаут сети / ожидание переподключения\n"
        "status.ffmpeg_icy_metadata=FFmpeg: ICY / metadata\n"
        "status.http_request=Подключение к URL (HTTP request...)\n"
        "status.ffmpeg_prefix=FFmpeg: \n"
        "status.reconnect_attempt_prefix=Переподключение... попытка \n"
        "status.reconnect_attempt_middle= из \n"
        "status.stream_unavailable_timeout=Поток недоступен / таймаут подключения\n"
        "status.reconnect_url_prefix=Переподключение к URL (\n"
        "status.reconnect_url_middle=), попытка \n"
        "status.connect_url_prefix=Подключение к URL (\n"
        "status.connect_url_suffix=)\n"
        "status.avformat_alloc_error=FFmpeg: ошибка avformat_alloc_context()\n"
        "status.reading_stream_headers=Чтение заголовков потока...\n"
        "status.retry_after_timeout_prefix=Попытка \n"
        "status.retry_after_timeout_suffix= после таймаута.\n"
        "status.connection_attempts_exceeded=Поток недоступен: превышено число попыток подключения\n"
        "status.analyzing_stream=Анализ потока и определение формата...\n"
        "status.stream_header_read_error=Ошибка чтения заголовков потока\n"
        "status.demuxer_prefix=Используемый демультиплексер: \n"
        "status.audio_stream_not_found=Не найден аудиопоток\n"
        "status.audio_output_init_error=Ошибка инициализации аудиовывода\n"
        "status.resampler_init_error=Ошибка инициализации ресемплера\n"
        "status.stream_read_error_reconnect=Ошибка чтения потока. Переподключение...\n"
        "status.stream_reconnect_attempts_exceeded=Ошибка потока: превышено число попыток переподключения\n"
        "status.audio_device_changed=Аудиоустройство изменено, переинициализация...\n"
        "status.skipping_bad_server_data=Пропускаем битые данные от сервера...\n"
        "msg.file_error=File Error\n"
        "msg.playlist_error=Playlist Error\n"
        "msg.sdl_error=SDL Error\n"
        "msg.sdl_image_error=SDL_image Error\n";
}

static const char* GetDefaultEnglishLanguageText()
{
    return
        "language.name=English\n"
        "settings.title=Settings\n"
        "settings.ok=OK\n"
        "settings.group.recording= Recording modes \n"
        "settings.group.effects= Playback effects \n"
        "settings.group.program= Program settings \n"
        "settings.language=Language:\n"
        "settings.recording.mp3= MP3, LAME, 320 kbit/sec\n"
        "settings.recording.flac= FLAC, s16, ~1000 kbit/sec\n"
        "settings.effect.stereo_width= Stereo Width\n"
        "settings.effect.exciter= Exciter / Brightness\n"
        "settings.effect.deep_bass= DeepBass\n"
        "settings.effect.dynamic_auto_volume= Dynamic Auto Volume\n"
        "settings.effect.gain_rider= GainRider / Peak Control\n"
        "settings.program.minimize_to_tray= Minimize to tray\n"
        "settings.program.show_track_toast= Show cover popup in tray on track change\n"
        "tooltip.play_stop=Play or stop the current station\n"
        "tooltip.prev_station=Switch to the previous station in the list\n"
        "tooltip.next_station=Switch to the next station in the list\n"
        "tooltip.previous_station=Return to the previously played station\n"
        "tooltip.record=Start or stop recording the current stream\n"
        "tooltip.settings=Open program settings\n"
        "tooltip.slider.bass=Bass\n"
        "tooltip.slider.treble=Treble\n"
        "tooltip.slider.volume=Volume\n"
        "tooltip.recording.mp3=Record the stream as MP3 320 kbit/sec\n"
        "tooltip.recording.flac=Record the stream as lossless FLAC\n"
        "tooltip.effect.stereo_width=Enable stereo widening\n"
        "tooltip.effect.exciter=Add brightness and presence to high frequencies\n"
        "tooltip.effect.deep_bass=Enhance low-frequency depth\n"
        "tooltip.effect.dynamic_auto_volume=Automatically even out playback volume\n"
        "tooltip.effect.gain_rider=Control peaks and keep a comfortable signal level\n"
        "tooltip.program.minimize_to_tray=Hide the program in the tray when minimized\n"
        "tooltip.program.show_track_toast=When the program is in tray, show the cover on track change\n"
        "tooltip.settings.ok=Save settings and close this window\n"
        "tooltip.about.ok=Close the about window\n"
        "tooltip.add.ok=Add the station to the list\n"
        "tooltip.add.cancel=Close without adding a station\n"
        "tray.balloon.title=IRP+ffmpeG is running in the tray\n"
        "tray.balloon.text=Double-click the icon to restore the window. Use the tray menu to exit.\n"
        "tray.restore=Open IRP+ffmpeG\n"
        "tray.play=Play\n"
        "tray.stop=Stop\n"
        "tray.prev=Previous station\n"
        "tray.next=Next station\n"
        "tray.previous=Return to previous station\n"
        "tray.settings=Settings\n"
        "tray.about=About\n"
        "tray.exit=Exit\n"
        "context.reload_m3u=Reload m3u\n"
        "context.add_station=Add station\n"
        "context.edit_station=Edit station name\n"
        "context.save_station=Save station\n"
        "context.delete_station=Delete station\n"
        "edit_station.title=Edit station name\n"
        "edit_station.msg.enter_name=Enter a station name.\n"
        "edit_station.msg.one_line=Station name must fit on one line.\n"
        "edit_station.msg.too_long=Station name must not be longer than 128 characters.\n"
        "edit_station.msg.create_failed=Could not create the station edit form.\n"
        "edit_station.msg.open_failed=Could not open the station edit form.\n"
        "add.title=Add station\n"
        "add.name_label=Station name:\n"
        "add.url_label=URL address:\n"
        "common.ok=OK\n"
        "common.cancel=Cancel\n"
        "add.msg.enter_name=Enter a station name.\n"
        "add.msg.one_line=Name and URL must each fit on one line.\n"
        "add.msg.enter_url=Enter the station internet address.\n"
        "add.msg.invalid_url=URL must start with http:// or https://\n"
        "add.msg.invalid_supported_url=Invalid URL. Supported addresses look like http://, https://, icy://, mms:// or rtsp:// and must not contain spaces.\n"
        "add.msg.duplicate_url=A station with this URL is already in the playlist.\n"
        "add.msg.create_failed=Could not create the add station form.\n"
        "add.msg.open_failed=Could not open the add station form.\n"
        "nowplaying.no_data=No track data\n"
        "status.stopped=Stopped\n"
        "status.http_ok=Reading headers (HTTP/1.1 200 OK)\n"
        "status.ffmpeg_reconnect=FFmpeg: reconnecting to stream...\n"
        "status.ffmpeg_timeout=FFmpeg: network timeout / waiting to reconnect\n"
        "status.ffmpeg_icy_metadata=FFmpeg: ICY / metadata\n"
        "status.http_request=Connecting to URL (HTTP request...)\n"
        "status.ffmpeg_prefix=FFmpeg: \n"
        "status.reconnect_attempt_prefix=Reconnecting... attempt \n"
        "status.reconnect_attempt_middle= of \n"
        "status.stream_unavailable_timeout=Stream unavailable / connection timeout\n"
        "status.reconnect_url_prefix=Reconnecting to URL (\n"
        "status.reconnect_url_middle=), attempt \n"
        "status.connect_url_prefix=Connecting to URL (\n"
        "status.connect_url_suffix=)\n"
        "status.avformat_alloc_error=FFmpeg: avformat_alloc_context() failed\n"
        "status.reading_stream_headers=Reading stream headers...\n"
        "status.retry_after_timeout_prefix=Attempt \n"
        "status.retry_after_timeout_suffix= after timeout.\n"
        "status.connection_attempts_exceeded=Stream unavailable: connection attempt limit exceeded\n"
        "status.analyzing_stream=Analyzing stream and detecting format...\n"
        "status.stream_header_read_error=Failed to read stream headers\n"
        "status.demuxer_prefix=Active demuxer: \n"
        "status.audio_stream_not_found=Audio stream not found\n"
        "status.audio_output_init_error=Audio output initialization failed\n"
        "status.resampler_init_error=Resampler initialization failed\n"
        "status.stream_read_error_reconnect=Stream read error. Reconnecting...\n"
        "status.stream_reconnect_attempts_exceeded=Stream error: reconnect attempt limit exceeded\n"
        "status.audio_device_changed=Audio device changed, reinitializing...\n"
        "status.skipping_bad_server_data=Skipping corrupted data from server...\n"
        "msg.file_error=File Error\n"
        "msg.playlist_error=Playlist Error\n"
        "msg.sdl_error=SDL Error\n"
        "msg.sdl_image_error=SDL_image Error\n";
}

static void WriteDefaultLanguageFileIfMissing(const std::filesystem::path& path, const char* text)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return;
    }

    std::ofstream out(path, std::ios::binary);
    if (out.is_open()) {
        out.write(text, static_cast<std::streamsize>(std::strlen(text)));
    }
}

static void EnsureDefaultLanguageFiles()
{
    std::error_code ec;
    std::filesystem::create_directories(GetLanguageDirectory(), ec);
    WriteDefaultLanguageFileIfMissing(GetLanguageDirectory() / L"russian.lng", GetDefaultRussianLanguageText());
    WriteDefaultLanguageFileIfMissing(GetLanguageDirectory() / L"english.lng", GetDefaultEnglishLanguageText());
}

static std::map<std::string, std::wstring> LoadLanguageFile(const std::filesystem::path& path)
{
    std::map<std::string, std::wstring> result;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return result;
    }

    const uintmax_t fileSize = std::filesystem::file_size(path, ec);
    if (ec || fileSize == 0 || fileSize > kMaxLanguageFileBytes) {
        return result;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return result;
    }

    std::string line;
    while (result.size() < kMaxLanguageEntries && std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.size() > kMaxLanguageLineBytes) {
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        std::string key = TrimLanguageAscii(line.substr(0, eq));
        if (key.size() >= 3 &&
            static_cast<unsigned char>(key[0]) == 0xEF &&
            static_cast<unsigned char>(key[1]) == 0xBB &&
            static_cast<unsigned char>(key[2]) == 0xBF) {
            key.erase(0, 3);
        }

        if (!IsValidLanguageKey(key)) {
            continue;
        }

        std::string value = line.substr(eq + 1);
        if (value.size() > kMaxLanguageValueBytes) {
            value.resize(kMaxLanguageValueBytes);
        }
        result[key] = utf8_to_wstring(value);
    }

    return result;
}

static bool HasLanguageIdentity(const std::map<std::string, std::wstring>& strings)
{
    auto it = strings.find("language.name");
    return it != strings.end() && !it->second.empty();
}

bool LoadLanguageById(const std::wstring& languageId)
{
    if (!IsValidLanguageId(languageId)) {
        return false;
    }

    auto strings = LoadLanguageFile(GetLanguageDirectory() / (languageId + L".lng"));
    if (strings.empty() || !HasLanguageIdentity(strings)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_languageMutex);
        g_languageStrings = std::move(strings);
        g_languageId = languageId;
    }
    return true;
}

void InitializeLanguageSystem()
{
    EnsureDefaultLanguageFiles();
    if (!LoadLanguageById(g_languageId)) {
        g_languageId = L"russian";
        LoadLanguageById(g_languageId);
    }
}

const wchar_t* Tr(const char* key, const wchar_t* fallback)
{
    thread_local std::wstring translated;
    translated = TrString(key, fallback);
    return translated.c_str();
}

std::wstring TrString(const char* key, const wchar_t* fallback)
{
    if (!key) {
        return fallback ? fallback : L"";
    }

    std::lock_guard<std::mutex> lock(g_languageMutex);
    auto it = g_languageStrings.find(key);
    if (it != g_languageStrings.end()) {
        return it->second;
    }

    return fallback ? fallback : L"";
}

void LoadAvailableLanguages()
{
    g_languageOptions.clear();
    EnsureDefaultLanguageFiles();

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(GetLanguageDirectory(), ec)) {
        if (ec) {
            break;
        }
        if (g_languageOptions.size() >= kMaxLanguageFiles) {
            break;
        }
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entry.path().extension() != L".lng") {
            continue;
        }

        const std::wstring id = entry.path().stem().wstring();
        if (!IsValidLanguageId(id)) {
            continue;
        }

        auto strings = LoadLanguageFile(entry.path());
        if (!HasLanguageIdentity(strings)) {
            continue;
        }

        g_languageOptions.push_back({ id, strings["language.name"] });
    }

    std::sort(g_languageOptions.begin(), g_languageOptions.end(),
        [](const LanguageOption& a, const LanguageOption& b) {
            return a.displayName < b.displayName;
        });
}

const std::vector<LanguageOption>& GetAvailableLanguages()
{
    return g_languageOptions;
}
