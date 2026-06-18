#include "file_recording.h"
#include "IRPFFmpeg.h"
#include "util.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

static const wchar_t* CURRENT_COVER_FILE_W = L"cover_cache\\cover.jpg";

static void RecordingThreadFlac();
static void RecordingThreadMp3();
static void AddCoverToMp3(const std::wstring& mp3_filename_w, const std::wstring& cover_path_w);
static void AddCoverToFlac(const std::wstring& flac_filename, const std::wstring& cover_path_w);

// Глобальные объекты для записи
// Структура для передачи данных в поток записи
struct AudioChunk {
    std::vector<float> data; // Float данные
    int sample_rate = 0;
    int channels = 0;
};

std::atomic<bool> g_is_recording(false);
std::atomic<bool> g_rec_semafor(false);
static std::thread* g_recording_thread = nullptr;
static std::queue<AudioChunk> g_recording_queue;
static std::mutex g_recording_mutex;
static std::string g_record_filename;

static bool IsUtf16HighSurrogate(wchar_t ch)
{
    return ch >= 0xD800 && ch <= 0xDBFF;
}

static bool IsUtf16LowSurrogate(wchar_t ch)
{
    return ch >= 0xDC00 && ch <= 0xDFFF;
}

static bool IsFilenameEmojiOrPresentationChar(wchar_t ch)
{
    const unsigned int code = static_cast<unsigned int>(ch);
    return code == 0x200D ||       // zero width joiner, used in emoji sequences
        code == 0x20E3 ||          // combining enclosing keycap
        (code >= 0xFE00 && code <= 0xFE0F) ||  // variation selectors
        (code >= 0x2600 && code <= 0x27BF);    // miscellaneous symbols/dingbats
}

static std::string SanitizeRecordingFileName(const std::string& name)
{
    std::wstring wide = utf8_to_wstring(name);
    std::wstring clean;
    clean.reserve(wide.size());

    for (size_t i = 0; i < wide.size(); ++i) {
        wchar_t ch = wide[i];

        if (IsUtf16HighSurrogate(ch)) {
            if (i + 1 < wide.size() && IsUtf16LowSurrogate(wide[i + 1]))
                ++i;
            continue;
        }
        if (IsUtf16LowSurrogate(ch) || IsFilenameEmojiOrPresentationChar(ch))
            continue;

        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' ||
            ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' ||
            ch == L'|' || ch == L'\r' || ch == L'\n' || ch == L'\t') {
            if (!clean.empty() && clean.back() != L'_')
                clean.push_back(L'_');
            continue;
        }

        clean.push_back(ch);
    }

    while (!clean.empty() && (clean.back() == L' ' || clean.back() == L'.' || clean.back() == L'_'))
        clean.pop_back();
    while (!clean.empty() && (clean.front() == L' ' || clean.front() == L'.' || clean.front() == L'_'))
        clean.erase(clean.begin());

    return clean.empty() ? "recording" : wstring_to_utf8(clean);
}

void StartRecording() {

    if (g_is_recording.load()) return;
  
    if (track_history.empty()) return;
       
    std::string safe_name = SanitizeRecordingFileName(track_history.front());
    
    char buffer[MAX_PATH];
    DWORD len = GetCurrentDirectoryA(MAX_PATH, buffer);

    if (len == 0 || len >= MAX_PATH) return;
   
    std::string currentDir = std::string(buffer, len);

    std::string lastfilename = currentDir + "\\Rec\\" + safe_name + ".mp3";

    std::string recDir = currentDir + "\\Rec";
    if (!PathFileExistsA(recDir.c_str())) {

        CreateDirectoryA(recDir.c_str(), NULL);
    }

    g_record_filename = lastfilename;

    // Очищаем очередь
    {
        std::lock_guard<std::mutex> lock(g_recording_mutex);
        while (!g_recording_queue.empty()) g_recording_queue.pop();
    }

    g_is_recording.store(true);

    if(rec_is_flac)
        g_recording_thread = new std::thread(RecordingThreadFlac);
    else
        g_recording_thread = new std::thread(RecordingThreadMp3);
}

void StopRecording() {
    if (!g_is_recording.load()) return;

    g_is_recording.store(false);
    if (g_recording_thread && g_recording_thread->joinable()) {
        g_recording_thread->join();
        delete g_recording_thread;
        g_recording_thread = nullptr;
    }
}

static std::pair<std::string, std::string> parseArtistTitle(const std::string& filepath) {
    namespace fs = std::filesystem;

    // Получаем имя файла без пути и расширения
    fs::path p(filepath);
    std::string filename = p.stem().string(); // без .mp3

    // Ищем разделитель " - "
    size_t pos = filename.find(" - ");
    if (pos == std::string::npos) {
        // Пробуем другие варианты
        pos = filename.find("_-_");
        if (pos == std::string::npos) {
            pos = filename.find("—");
        }
    }

    if (pos != std::string::npos) {
        std::string artist = filename.substr(0, pos);
        std::string title = filename.substr(pos + 3); // для " - "

        // Обрезаем пробелы
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t\n\r\f\v"));
            s.erase(s.find_last_not_of(" \t\n\r\f\v") + 1);
            };

        trim(artist);
        trim(title);

        return { artist, title };
    }

    // Если разделитель не найден, возвращаем весь filename как title
    return { "", filename };
}

static void RecordingThreadMp3() {
    // Устанавливаем расширение .mp3 по умолчанию
    if (g_record_filename.empty()) {
        g_record_filename = "output.mp3";
    }
    else {
        size_t dot_pos = g_record_filename.find_last_of('.');
        if (dot_pos == std::string::npos ||
            g_record_filename.substr(dot_pos + 1) != "mp3") {
            g_record_filename += ".mp3";
        }
    }
    std::string current_filename = g_record_filename;

    // Параметры кодека
    int nb_channels = 2;
    int target_sample_rate = 48000; 
    constexpr int64_t bit_rate = 320000;
    constexpr int frame_size = 1152;  // Строго 1152 для MP3 (MPEG-1 Layer III)

    // Буферы и состояние
    std::vector<float> audio_fifo;
    int64_t total_samples_written = 0;
    bool initialized = false;
    int input_sample_rate = 0;

    // Контексты FFmpeg
    AVCodecContext* codec_ctx = nullptr;
    AVFormatContext* fmt_ctx = nullptr;
    AVStream* stream = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwrContext* swr_ctx = nullptr;

    auto cleanup = [&]() {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        swr_free(&swr_ctx);
        if (codec_ctx) {
            av_channel_layout_uninit(&codec_ctx->ch_layout);
            avcodec_free_context(&codec_ctx);
        }
        if (fmt_ctx) {
            if (fmt_ctx->pb) avio_closep(&fmt_ctx->pb);
            avformat_free_context(fmt_ctx);
        }
        };

    while (g_is_recording.load() || !g_recording_queue.empty()) {
        AudioChunk chunk;
        bool has_chunk = false;

        if (!g_recording_queue.empty()) {
            {
                std::lock_guard<std::mutex> lock(g_recording_mutex);
                if (!g_recording_queue.empty()) {
                    chunk = std::move(g_recording_queue.front());
                    g_recording_queue.pop();
                    has_chunk = true;
                }
            }
        }

        if (!has_chunk) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (g_is_recording.load()) continue;
            else break;
        }

        
                
        if (!initialized) {

            input_sample_rate = chunk.sample_rate;
            target_sample_rate = input_sample_rate;
            if(input_sample_rate > 48000) target_sample_rate = 48000;
			nb_channels = chunk.channels;

            const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MP3);
            if (!codec) {
                //OutputDebugString(L"MP3 codec not found\n");
                cleanup();
                return;
            }

            codec_ctx = avcodec_alloc_context3(codec);
            if (!codec_ctx) {
                cleanup();
                return;
            }

            codec_ctx->sample_rate = target_sample_rate;
            codec_ctx->bit_rate = bit_rate;
            codec_ctx->time_base = AVRational{ 1, target_sample_rate };
            av_channel_layout_default(&codec_ctx->ch_layout, nb_channels);
            codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;  // Стандарт для MP3

            if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
               // OutputDebugString(L"Failed to open MP3 codec\n");
                cleanup();
                return;
            }

            if (avformat_alloc_output_context2(&fmt_ctx, nullptr, "mp3", current_filename.c_str()) < 0) {
                //OutputDebugString(L"Failed to allocate format context\n");
                cleanup();
                return;
            }

            stream = avformat_new_stream(fmt_ctx, codec);
            if (!stream) {
                //OutputDebugString(L"Failed to create stream\n");
                cleanup();
                return;
            }

            avcodec_parameters_from_context(stream->codecpar, codec_ctx);
            stream->time_base = codec_ctx->time_base;

            // Настройка ресемплера: 96000/48000/44100 → 48000 (целевая)
            swr_ctx = swr_alloc();
            if (!swr_ctx) {
                //OutputDebugString(L"Failed to allocate resampler\n");
                cleanup();
                return;
            }

            AVChannelLayout in_ch_layout, out_ch_layout;
            av_channel_layout_default(&in_ch_layout, nb_channels);
            av_channel_layout_copy(&out_ch_layout, &codec_ctx->ch_layout);

            av_opt_set_chlayout(swr_ctx, "in_chlayout", &in_ch_layout, 0);
            av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_ch_layout, 0);
            av_opt_set_int(swr_ctx, "in_sample_rate", input_sample_rate, 0);
            av_opt_set_int(swr_ctx, "out_sample_rate", target_sample_rate, 0);
            av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);   // WASAPI: packed float
            av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);  // MP3: planar float

            av_channel_layout_uninit(&in_ch_layout);
            av_channel_layout_uninit(&out_ch_layout);

            if (swr_init(swr_ctx) < 0) {
                //OutputDebugString(L"Failed to initialize resampler\n");
                cleanup();
                return;
            }

            if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
                if (avio_open(&fmt_ctx->pb, current_filename.c_str(), AVIO_FLAG_WRITE) < 0) {
                    //OutputDebugString(L"Failed to open output file\n");
                    cleanup();
                    return;
                }
            }
            //METADATA
            auto [artist, title] = parseArtistTitle(current_filename);
            if (!artist.empty()) av_dict_set(&fmt_ctx->metadata, "artist", artist.c_str(), 0);
            if (!title.empty()) av_dict_set(&fmt_ctx->metadata, "title", title.c_str(), 0);
            
            int ret_hdr = avformat_write_header(fmt_ctx, nullptr);
            if (ret_hdr < 0) {
                cleanup();
                return;
            }
             
            frame = av_frame_alloc();
            if (!frame) {
                cleanup();
                return;
            }

            frame->format = codec_ctx->sample_fmt;
            av_channel_layout_copy(&frame->ch_layout, &codec_ctx->ch_layout);
            frame->sample_rate = target_sample_rate;
            frame->nb_samples = frame_size;

            if (av_frame_get_buffer(frame, 0) < 0) {
                //OutputDebugString(L"Failed to allocate frame buffer\n");
                cleanup();
                return;
            }

            pkt = av_packet_alloc();
            if (!pkt) {
                cleanup();
                return;
            }

            initialized = true;

            if (input_sample_rate != target_sample_rate) {
                wchar_t msg[128];
                swprintf_s(msg, L"MP3 resampling: %d Hz → %d Hz\n", input_sample_rate, target_sample_rate);
                //OutputDebugString(msg);
            }
        }
        else if (chunk.sample_rate != input_sample_rate) {
            continue; // Пропускаем несогласованные чанки
        }

        // НАКОПЛЕНИЕ
        audio_fifo.insert(audio_fifo.end(), chunk.data.begin(), chunk.data.end());

        // ВАЖНО: правильный расчёт входных сэмплов 
        // Формула: in_samples = out_samples * (in_rate / out_rate)
        const int64_t in_samples_needed = av_rescale_rnd(
            frame_size,              // 1152 выходных сэмпла
            input_sample_rate,       // Например, 96000
            target_sample_rate,      // 48000
            AV_ROUND_UP
        );
        const size_t floats_needed = static_cast<size_t>(in_samples_needed) * nb_channels;

        // КОДИРОВАНИЕ 
        while (audio_fifo.size() >= floats_needed) {
            const uint8_t* in_data[1] = { reinterpret_cast<const uint8_t*>(audio_fifo.data()) };

            // Конвертируем с точным расчётом сэмплов
            int converted = swr_convert(
                swr_ctx,
                frame->data,
                frame_size,                      // Запрашиваем ровно 1152
                in_data,
                static_cast<int>(in_samples_needed)  // Отдаём рассчитанное количество
            );

            if (converted < 0) {
                //OutputDebugString(L"swr_convert error\n");
                break;
            }

            // MP3 ТРЕБУЕТ строго 1152 сэмпла на фрейм (MPEG-1 Layer III)
            if (converted != frame_size) {
                // Дозаполняем нулями до 1152 (защита от погрешностей ресемплера)
                for (int ch = 0; ch < nb_channels; ch++) {
                    float* channel_data = reinterpret_cast<float*>(frame->data[ch]);
                    std::fill(channel_data + converted, channel_data + frame_size, 0.0f);
                }
            }

            frame->nb_samples = frame_size;  // Гарантируем 1152
            frame->pts = total_samples_written;

            if (avcodec_send_frame(codec_ctx, frame) < 0) {
                //OutputDebugString(L"avcodec_send_frame failed (wrong sample count or format)\n");
                break;
            }

            while (avcodec_receive_packet(codec_ctx, pkt) == 0) {
                av_packet_rescale_ts(pkt, codec_ctx->time_base, stream->time_base);
                pkt->stream_index = stream->index;
                av_interleaved_write_frame(fmt_ctx, pkt);
                av_packet_unref(pkt);
            }

            audio_fifo.erase(audio_fifo.begin(), audio_fifo.begin() + floats_needed);
            total_samples_written += frame_size;
        }
    }

    // === ХВОСТ ===
    if (initialized && !audio_fifo.empty()) {
        int remaining = static_cast<int>(audio_fifo.size() / nb_channels);
        if (remaining > 0) {
            const uint8_t* in_data[1] = { reinterpret_cast<const uint8_t*>(audio_fifo.data()) };
            int converted = swr_convert(swr_ctx, frame->data, frame_size, in_data, remaining);

            if (converted > 0) {
                // Дозаполняем до 1152
                if (converted < frame_size) {
                    for (int ch = 0; ch < nb_channels; ch++) {
                        float* channel_data = reinterpret_cast<float*>(frame->data[ch]);
                        std::fill(channel_data + converted, channel_data + frame_size, 0.0f);
                    }
                }
                frame->nb_samples = frame_size;
                frame->pts = total_samples_written;

                if (avcodec_send_frame(codec_ctx, frame) >= 0) {
                    while (avcodec_receive_packet(codec_ctx, pkt) == 0) {
                        av_packet_rescale_ts(pkt, codec_ctx->time_base, stream->time_base);
                        pkt->stream_index = stream->index;
                        av_interleaved_write_frame(fmt_ctx, pkt);
                        av_packet_unref(pkt);
                    }
                }
            }
            swr_convert(swr_ctx, nullptr, 0, nullptr, 0);
        }
    }

    // === ФИНАЛИЗАЦИЯ ===
    if (initialized && codec_ctx && fmt_ctx) {
        avcodec_send_frame(codec_ctx, nullptr);
        while (avcodec_receive_packet(codec_ctx, pkt) == 0) {
            av_packet_rescale_ts(pkt, codec_ctx->time_base, stream->time_base);
            pkt->stream_index = stream->index;
            av_interleaved_write_frame(fmt_ctx, pkt);
            av_packet_unref(pkt);
        }
        av_write_trailer(fmt_ctx);
    }

    cleanup();

	std::wstring w_current_filename = utf8_to_wstring(current_filename);

    AddCoverToMp3(w_current_filename, CURRENT_COVER_FILE_W);
}

static void AddCoverToMp3(const std::wstring& mp3_filename, const std::wstring& cover_path_w) {
    std::wstring temp_filename = mp3_filename + L".temp.mp3";

    std::vector<uint8_t> cover_data;
    long cover_size = 0;

    {
        std::lock_guard<std::mutex> lock(g_coverFileMutex);

        FILE* cover_file = nullptr;
        _wfopen_s(&cover_file, cover_path_w.c_str(), L"rb");
        if (!cover_file) {
            OutputDebugString(L"[ERROR] Cover file not found. Skipping.\n");
            return;
        }

        fseek(cover_file, 0, SEEK_END);
        cover_size = ftell(cover_file);
        fseek(cover_file, 0, SEEK_SET);

        if (cover_size <= 0) {
            fclose(cover_file);
            return;
        }

        cover_data.resize(cover_size);
        fread(cover_data.data(), 1, cover_size, cover_file);
        fclose(cover_file);
    }

    // 2. Переименование MP3 (Backup strategy)
    if (_wrename(mp3_filename.c_str(), temp_filename.c_str()) != 0) {
        return;
    }

    AVFormatContext* input_ctx = nullptr;
    AVFormatContext* output_ctx = nullptr;

    // Открываем исходный файл
    if (avformat_open_input(&input_ctx, wstring_to_utf8(temp_filename).c_str(), nullptr, nullptr) < 0) {
        if (_wrename(temp_filename.c_str(), mp3_filename.c_str()) != 0) {};
        return;
    }

    // Проверяем поток инфо
    if (avformat_find_stream_info(input_ctx, nullptr) < 0) {
        avformat_close_input(&input_ctx);
        if (_wrename(temp_filename.c_str(), mp3_filename.c_str()) != 0) {};
        return;
    }

    // Создаем выходной контекст
    if (avformat_alloc_output_context2(&output_ctx, nullptr, nullptr, wstring_to_utf8(mp3_filename).c_str()) < 0) {
        avformat_close_input(&input_ctx);
        if (_wrename(temp_filename.c_str(), mp3_filename.c_str()) != 0) {};
        return;
    }

    // --- STREAM #0: звук ---
    int in_audio_index = av_find_best_stream(input_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (in_audio_index < 0) {
        // Обработка ошибки: нет аудио
        avformat_free_context(output_ctx);
        avformat_close_input(&input_ctx);
        if (_wrename(temp_filename.c_str(), mp3_filename.c_str()) != 0) {};
        return;
    }

    AVStream* in_audio_stream = input_ctx->streams[in_audio_index];
    AVStream* out_audio_stream = avformat_new_stream(output_ctx, nullptr);

    avcodec_parameters_copy(out_audio_stream->codecpar, in_audio_stream->codecpar);
    out_audio_stream->codecpar->codec_tag = 0;
    out_audio_stream->time_base = in_audio_stream->time_base;

    // --- STREAM #1: картинка ---
    AVStream* out_cover_stream = avformat_new_stream(output_ctx, nullptr);

    out_cover_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    // Определяем кодек
    if (cover_size > 8 && cover_data[0] == 0x89 && cover_data[1] == 'P' && cover_data[2] == 'N' && cover_data[3] == 'G') {
        out_cover_stream->codecpar->codec_id = AV_CODEC_ID_PNG;
    }
    else {
        out_cover_stream->codecpar->codec_id = AV_CODEC_ID_MJPEG; // По умолчанию JPEG
    }

    out_cover_stream->codecpar->width = 400;
    out_cover_stream->codecpar->height = 400;
    out_cover_stream->codecpar->format = AV_PIX_FMT_NONE;

    out_cover_stream->time_base = AVRational{ 1, 1 }; // Для attached pic обычно 1/1
    out_cover_stream->disposition = AV_DISPOSITION_ATTACHED_PIC;

    // Открытие файла для записи
    if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&output_ctx->pb, wstring_to_utf8(mp3_filename).c_str(), AVIO_FLAG_WRITE) < 0) {
            avformat_free_context(output_ctx);
            avformat_close_input(&input_ctx);
            if (_wrename(temp_filename.c_str(), mp3_filename.c_str()) != 0) {};
            return;
        }
    }

    av_dict_copy(&output_ctx->metadata, input_ctx->metadata, 0);

   if (avformat_write_header(output_ctx, nullptr) < 0) {
       if (!(output_ctx->oformat->flags & AVFMT_NOFILE) && output_ctx->pb) {
           avio_closep(&output_ctx->pb);
       }
        avformat_free_context(output_ctx);
        avformat_close_input(&input_ctx);
        _wremove(mp3_filename.c_str());
        if (_wrename(temp_filename.c_str(), mp3_filename.c_str()) != 0) {};
		//OutputDebugString(L"[ERROR] Failed to write MP3 header with cover.\n");
        return;
    }

    // Запись обложки (Stream Index 1)
    AVPacket* cover_pkt = av_packet_alloc();
    av_new_packet(cover_pkt, (int)cover_data.size());
    memcpy(cover_pkt->data, cover_data.data(), cover_data.size());

    // Важно: stream_index должен соответствовать порядку добавления.
    // Аудио было 0, Обложка - 1.
    cover_pkt->stream_index = 1;
    cover_pkt->flags |= AV_PKT_FLAG_KEY;
    // PTS/DTS не важны для attached_pic, но хорошим тоном считается 0
    cover_pkt->pts = 0;
    cover_pkt->dts = 0;

    av_interleaved_write_frame(output_ctx, cover_pkt);
    av_packet_free(&cover_pkt);

    // Запись аудио (Stream Index 0)
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(input_ctx, pkt) >= 0) {
        if (pkt->stream_index == in_audio_index) {
            pkt->stream_index = 0; // Переназначаем на 0
            av_packet_rescale_ts(pkt, in_audio_stream->time_base, out_audio_stream->time_base);
            av_interleaved_write_frame(output_ctx, pkt);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    av_write_trailer(output_ctx);

    // Очистка ресурсов
    if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&output_ctx->pb);
    }
    avformat_free_context(output_ctx);
    avformat_close_input(&input_ctx);

    _wremove(temp_filename.c_str());
   
}


static void RecordingThreadFlac() {
    // Устанавливаем расширение .flac по умолчанию
    if (g_record_filename.empty()) {
        g_record_filename = "output.flac";
    }
    else {
        size_t dot_pos = g_record_filename.find_last_of('.');
        if (dot_pos == std::string::npos ||
            g_record_filename.substr(dot_pos + 1) != "flac") {
            g_record_filename.erase(dot_pos);
            g_record_filename += ".flac";
        }
    }

    std::string current_filename = g_record_filename;

    int target_sample_rate = 48000;

    std::vector<float> audio_fifo;
    int64_t total_samples_written = 0;
    bool initialized = false;

    constexpr int nb_channels = 2;
    int input_sample_rate = 0;
    int input_channels = 2;

    AVCodecContext* codec_ctx = nullptr;
    AVFormatContext* fmt_ctx = nullptr;
    AVStream* stream = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwrContext* swr_ctx = nullptr;

    while (g_is_recording.load() || !g_recording_queue.empty()) {
        AudioChunk chunk;
        if (!g_recording_queue.empty()) {
            std::lock_guard<std::mutex> lock(g_recording_mutex);
            if (g_recording_queue.empty()) continue;
            chunk = std::move(g_recording_queue.front());
            g_recording_queue.pop();
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (g_is_recording.load()) continue;
            else break;
        }

        if (!initialized) {

            input_sample_rate = chunk.sample_rate;
            input_channels = chunk.channels;
			target_sample_rate = input_sample_rate;

            const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
            if (!codec) {
                //OutputDebugString(L"FLAC codec not found");
                return;
            }

            codec_ctx = avcodec_alloc_context3(codec);
            if (!codec_ctx) {
                //OutputDebugString(L"Failed to alloc codec_ctx");
                return;
            }

            // Жёстко задаём упакованный 16-bit (s16)
            codec_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
            codec_ctx->sample_rate = target_sample_rate;
            codec_ctx->time_base = AVRational{ 1, target_sample_rate };
            av_channel_layout_default(&codec_ctx->ch_layout, nb_channels);
            // Обязательно синхронизируем channels
            codec_ctx->ch_layout.nb_channels  =  nb_channels;

            AVDictionary* codec_opts = nullptr;
            av_dict_set(&codec_opts, "compression_level", "8", 0);

            int ret = avcodec_open2(codec_ctx, codec, &codec_opts);
            av_dict_free(&codec_opts);
            if (ret < 0) {
                std::string ss = std::string("Failed to open FLAC codec");
                //OutputDebugStringA(ss.c_str());
                avcodec_free_context(&codec_ctx);
                return;
            }

            if (avformat_alloc_output_context2(&fmt_ctx, nullptr, nullptr, current_filename.c_str()) < 0) {
                //OutputDebugString(L"Failed to allocate format context");
                avcodec_free_context(&codec_ctx);
                return;
            }

            stream = avformat_new_stream(fmt_ctx, codec);
            if (!stream) {
                //OutputDebugString(L"Failed to create stream");
                avcodec_free_context(&codec_ctx);
                avformat_free_context(fmt_ctx);
                return;
            }

            avcodec_parameters_from_context(stream->codecpar, codec_ctx);
            stream->time_base = codec_ctx->time_base;

            // Ресемплер
            swr_ctx = swr_alloc();
            if (!swr_ctx) {
                //OutputDebugString(L"Failed to allocate resampler");
                avcodec_free_context(&codec_ctx);
                avformat_free_context(fmt_ctx);
                return;
            }

            AVChannelLayout in_ch_layout, out_ch_layout;
            av_channel_layout_default(&in_ch_layout, nb_channels);
            av_channel_layout_copy(&out_ch_layout, &codec_ctx->ch_layout);

            av_opt_set_chlayout(swr_ctx, "in_chlayout", &in_ch_layout, 0);
            av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_ch_layout, 0);
            av_opt_set_int(swr_ctx, "in_sample_rate", input_sample_rate, 0);
            av_opt_set_int(swr_ctx, "out_sample_rate", target_sample_rate, 0);
            av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0); // WASAPI packed float
            av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", codec_ctx->sample_fmt, 0); // s16 packed

            av_channel_layout_uninit(&in_ch_layout);
            av_channel_layout_uninit(&out_ch_layout);

            if (swr_init(swr_ctx) < 0) {
                //OutputDebugString(L"Failed to initialize resampler");
                swr_free(&swr_ctx);
                avcodec_free_context(&codec_ctx);
                avformat_free_context(fmt_ctx);
                return;
            }

            if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
                if (avio_open(&fmt_ctx->pb, current_filename.c_str(), AVIO_FLAG_WRITE) < 0) {
                    //OutputDebugString(L"Failed to open output file");
                    swr_free(&swr_ctx);
                    avcodec_free_context(&codec_ctx);
                    avformat_free_context(fmt_ctx);
                    return;
                }
            }
            //METADATA
            auto [artist, title] = parseArtistTitle(current_filename);
            if (!artist.empty()) av_dict_set(&fmt_ctx->metadata, "artist", artist.c_str(), 0);
            if (!title.empty()) av_dict_set(&fmt_ctx->metadata, "title", title.c_str(), 0);

            if (avformat_write_header(fmt_ctx, nullptr) < 0) {
                //OutputDebugString(L"Failed to write header");
                swr_free(&swr_ctx);
                avcodec_free_context(&codec_ctx);
                if (fmt_ctx->pb) avio_closep(&fmt_ctx->pb);
                avformat_free_context(fmt_ctx);
                return;
            }

            frame = av_frame_alloc();
            if (!frame) {
                //OutputDebugString(L"Failed to alloc frame");
                swr_free(&swr_ctx);
                avcodec_free_context(&codec_ctx);
                if (fmt_ctx->pb) avio_closep(&fmt_ctx->pb);
                avformat_free_context(fmt_ctx);
                return;
            }

            frame->format = codec_ctx->sample_fmt;
            av_channel_layout_copy(&frame->ch_layout, &codec_ctx->ch_layout);
            frame->sample_rate = target_sample_rate;
            int codec_frame_size = codec_ctx->frame_size > 0 ? codec_ctx->frame_size : 8192;
            frame->nb_samples = codec_frame_size;

            if (av_frame_get_buffer(frame, 0) < 0) {
                //OutputDebugString(L"Failed to allocate frame buffer");
                av_frame_free(&frame);
                swr_free(&swr_ctx);
                avcodec_free_context(&codec_ctx);
                if (fmt_ctx->pb) avio_closep(&fmt_ctx->pb);
                avformat_free_context(fmt_ctx);
                return;
            }

            pkt = av_packet_alloc();
            initialized = true;
        }

        else if (chunk.sample_rate != input_sample_rate || chunk.channels != input_channels) {
            continue;
        }

        // accumulate
        audio_fifo.insert(audio_fifo.end(), chunk.data.begin(), chunk.data.end());

        const int codec_frame_size = codec_ctx->frame_size > 0 ? codec_ctx->frame_size : 8192;
        const int out_channels = codec_ctx->ch_layout.nb_channels > 0 ? codec_ctx->ch_layout.nb_channels : nb_channels;
        int bytes_per_sample = av_get_bytes_per_sample(codec_ctx->sample_fmt);
        if (bytes_per_sample <= 0) bytes_per_sample = 2;

        const int in_samples_needed = static_cast<int>(av_rescale_rnd(
            codec_frame_size,
            (int64_t)input_sample_rate,
            (int64_t)target_sample_rate,
            AV_ROUND_UP));

        // выделяем один раз временный буфер для interleaved вывода
        std::vector<uint8_t> out_interleaved;
        out_interleaved.resize((size_t)codec_frame_size * out_channels * bytes_per_sample);
        uint8_t* out_ptrs[1] = { out_interleaved.data() };

        while (audio_fifo.size() >= (size_t)in_samples_needed * nb_channels) {
            const uint8_t* in_ptrs[1] = { reinterpret_cast<const uint8_t*>(audio_fifo.data()) };

            int converted = swr_convert(
                swr_ctx,
                out_ptrs,
                codec_frame_size,
                in_ptrs,
                in_samples_needed
            );

            if (converted < 0) {
                //OutputDebugString(L"Resampling error (chunked)\n");
                break;
            }

            if (av_frame_make_writable(frame) < 0) {
                //OutputDebugString(L"av_frame_make_writable failed\n");
                break;
            }

            size_t copy_bytes = (size_t)converted * out_channels * bytes_per_sample;
            memcpy(frame->data[0], out_interleaved.data(), copy_bytes);

            if (converted < codec_frame_size) {
                size_t pad_bytes = (size_t)(codec_frame_size - converted) * out_channels * bytes_per_sample;
                memset(frame->data[0] + copy_bytes, 0, pad_bytes);
            }

            frame->nb_samples = codec_frame_size;
            frame->pts = total_samples_written;

            int ret = avcodec_send_frame(codec_ctx, frame);
            if (ret < 0) {
                std::string msg = std::string("avcodec_send_frame failed (chunked)");
                //OutputDebugStringA(msg.c_str());
                break;
            }

            while (avcodec_receive_packet(codec_ctx, pkt) == 0) {
                av_packet_rescale_ts(pkt, codec_ctx->time_base, stream->time_base);
                pkt->stream_index = stream->index;
                av_interleaved_write_frame(fmt_ctx, pkt);
                av_packet_unref(pkt);
            }

            audio_fifo.erase(audio_fifo.begin(), audio_fifo.begin() + (size_t)in_samples_needed * nb_channels);
            total_samples_written += codec_frame_size;
        }
    }

    // tail
    if (initialized && !audio_fifo.empty()) {
        int remaining_input_samples = static_cast<int>(audio_fifo.size() / nb_channels);
        if (remaining_input_samples > 0) {
            const uint8_t* in_data[1] = { reinterpret_cast<const uint8_t*>(audio_fifo.data()) };
            int codec_frame_size = codec_ctx->frame_size > 0 ? codec_ctx->frame_size : 8192;
            int bytes_per_sample = av_get_bytes_per_sample(codec_ctx->sample_fmt);
            if (bytes_per_sample <= 0) bytes_per_sample = 2;

            std::vector<uint8_t> out_interleaved_tail((size_t)codec_frame_size * nb_channels * bytes_per_sample);
            uint8_t* out_ptrs_tail[1] = { out_interleaved_tail.data() };

            int converted = swr_convert(
                swr_ctx,
                out_ptrs_tail,
                codec_frame_size,
                in_data,
                remaining_input_samples
            );

            if (converted > 0) {
                if (av_frame_make_writable(frame) >= 0) {
                    size_t out_bytes = (size_t)converted * nb_channels * bytes_per_sample;
                    memcpy(frame->data[0], out_interleaved_tail.data(), out_bytes);
                    if (converted < codec_frame_size) {
                        memset(frame->data[0] + out_bytes, 0, (size_t)(codec_frame_size - converted) * nb_channels * bytes_per_sample);
                    }
                    frame->nb_samples = codec_frame_size;
                    frame->pts = total_samples_written;

                    int ret = avcodec_send_frame(codec_ctx, frame);
                    if (ret >= 0) {
                        while (avcodec_receive_packet(codec_ctx, pkt) == 0) {
                            av_packet_rescale_ts(pkt, codec_ctx->time_base, stream->time_base);
                            pkt->stream_index = stream->index;
                            av_interleaved_write_frame(fmt_ctx, pkt);
                            av_packet_unref(pkt);
                        }
                    }
                    total_samples_written += frame->nb_samples;
                }
            }

            swr_convert(swr_ctx, nullptr, 0, nullptr, 0);
        }
    }

    if (initialized) {
        avcodec_send_frame(codec_ctx, nullptr);
        while (avcodec_receive_packet(codec_ctx, pkt) == 0) {
            av_packet_rescale_ts(pkt, codec_ctx->time_base, stream->time_base);
            pkt->stream_index = stream->index;
            av_interleaved_write_frame(fmt_ctx, pkt);
            av_packet_unref(pkt);
        }

        av_write_trailer(fmt_ctx);

        av_channel_layout_uninit(&codec_ctx->ch_layout);
        av_channel_layout_uninit(&frame->ch_layout);

        if (fmt_ctx->pb) avio_closep(&fmt_ctx->pb);
        avformat_free_context(fmt_ctx);
        avcodec_free_context(&codec_ctx);
        swr_free(&swr_ctx);
        av_frame_free(&frame);
        av_packet_free(&pkt);
    }

    std::wstring flac_filename = utf8_to_wstring(current_filename);

    AddCoverToFlac(flac_filename, CURRENT_COVER_FILE_W);
}

static void AddCoverToFlac(const std::wstring& flac_filename, const std::wstring& cover_path_w) {
    // 1. Формируем имя временного файла (меняем расширение на .flac)
    std::wstring temp_filename = flac_filename + L".temp.flac";

    std::vector<uint8_t> cover_data;
    long cover_size = 0;

    {
        std::lock_guard<std::mutex> lock(g_coverFileMutex);

        FILE* cover_file = nullptr;
        _wfopen_s(&cover_file, cover_path_w.c_str(), L"rb");
        if (!cover_file) {
            OutputDebugString(L"[ERROR] Cover file not found. Skipping.\n");
            return;
        }

        fseek(cover_file, 0, SEEK_END);
        cover_size = ftell(cover_file);
        fseek(cover_file, 0, SEEK_SET);

        if (cover_size <= 0) {
            fclose(cover_file);
            return;
        }

        cover_data.resize(cover_size);
        fread(cover_data.data(), 1, cover_size, cover_file);
        fclose(cover_file);
    }

    // 2. Переименование исходного FLAC (Backup strategy)
    if (_wrename(flac_filename.c_str(), temp_filename.c_str()) != 0) {
        OutputDebugString(L"[ERROR] Failed to rename original FLAC file.\n");
        return;
    }

    AVFormatContext* input_ctx = nullptr;
    AVFormatContext* output_ctx = nullptr;

    // Открываем исходный файл (временный)
    if (avformat_open_input(&input_ctx, wstring_to_utf8(temp_filename).c_str(), nullptr, nullptr) < 0) {
        if (_wrename(temp_filename.c_str(), flac_filename.c_str()) != 0) {};
        return;
    }

    // Проверяем потоки
    if (avformat_find_stream_info(input_ctx, nullptr) < 0) {
        avformat_close_input(&input_ctx);
        if (_wrename(temp_filename.c_str(), flac_filename.c_str()) != 0) {};
        return;
    }

    // Создаем выходной контекст. FFmpeg определит формат FLAC по расширению имени файла.
    if (avformat_alloc_output_context2(&output_ctx, nullptr, nullptr, wstring_to_utf8(flac_filename).c_str()) < 0) {
        avformat_close_input(&input_ctx);
        if (_wrename(temp_filename.c_str(), flac_filename.c_str()) != 0) {};
        return;
    }

    // --- STREAM #0: Звук ---
    int in_audio_index = av_find_best_stream(input_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (in_audio_index < 0) {
        avformat_free_context(output_ctx);
        avformat_close_input(&input_ctx);
        if (_wrename(temp_filename.c_str(), flac_filename.c_str()) != 0) {};
        return;
    }

    AVStream* in_audio_stream = input_ctx->streams[in_audio_index];
    AVStream* out_audio_stream = avformat_new_stream(output_ctx, nullptr);

    if (!out_audio_stream) {
        // Ошибка создания потока
        avformat_free_context(output_ctx);
        avformat_close_input(&input_ctx);
        if (_wrename(temp_filename.c_str(), flac_filename.c_str()) != 0) {};
        return;
    }

    // Копируем параметры кодека (для FLAC это важно)
    avcodec_parameters_copy(out_audio_stream->codecpar, in_audio_stream->codecpar);
    out_audio_stream->codecpar->codec_tag = 0;
    out_audio_stream->time_base = in_audio_stream->time_base;

    // --- #1: Attached Picture ---
    AVStream* out_cover_stream = avformat_new_stream(output_ctx, nullptr);

    out_cover_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;

    if (cover_size > 8 && cover_data[0] == 0x89 && cover_data[1] == 'P' && cover_data[2] == 'N' && cover_data[3] == 'G') {
        out_cover_stream->codecpar->codec_id = AV_CODEC_ID_PNG;
    }
    else {
        out_cover_stream->codecpar->codec_id = AV_CODEC_ID_MJPEG;
    }

    out_cover_stream->codecpar->width = 400; 
    out_cover_stream->codecpar->height = 400;
    out_cover_stream->codecpar->format = AV_PIX_FMT_NONE; 
    out_cover_stream->time_base = AVRational{ 1, 1 };
    out_cover_stream->disposition = AV_DISPOSITION_ATTACHED_PIC;

    // Открытие файла для записи
    if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&output_ctx->pb, wstring_to_utf8(flac_filename).c_str(), AVIO_FLAG_WRITE) < 0) {
            avformat_free_context(output_ctx);
            avformat_close_input(&input_ctx);
            _wremove(flac_filename.c_str());
            if (_wrename(temp_filename.c_str(), flac_filename.c_str()) != 0) {};
            return;
        }
    }

    // Копирование метаданных (тегов Vorbis Comments, используемых в FLAC)
    av_dict_copy(&output_ctx->metadata, input_ctx->metadata, 0);

    // Запись заголовка
    if (avformat_write_header(output_ctx, nullptr) < 0) {
        if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&output_ctx->pb);
        avformat_free_context(output_ctx);
        avformat_close_input(&input_ctx);
        _wremove(flac_filename.c_str());
        if (_wrename(temp_filename.c_str(), flac_filename.c_str()) != 0) {};
        return;
    }

    // Запись обложки (Stream 1)
    AVPacket* cover_pkt = av_packet_alloc();
    av_new_packet(cover_pkt, (int)cover_data.size());
    memcpy(cover_pkt->data, cover_data.data(), cover_data.size());

    cover_pkt->stream_index = 1; // Индекс потока картинки
    cover_pkt->flags |= AV_PKT_FLAG_KEY;
    cover_pkt->pts = 0;
    cover_pkt->dts = 0;

    av_interleaved_write_frame(output_ctx, cover_pkt);
    av_packet_free(&cover_pkt);

    //Запись аудио (Stream 0)
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(input_ctx, pkt) >= 0) {
        if (pkt->stream_index == in_audio_index) {
            pkt->stream_index = 0; 
            // Важно: пересчет временных меток для FLAC
            av_packet_rescale_ts(pkt, in_audio_stream->time_base, out_audio_stream->time_base);
            av_interleaved_write_frame(output_ctx, pkt);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    av_write_trailer(output_ctx);

    // Очистка ресурсов
    if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&output_ctx->pb);
    }
    avformat_free_context(output_ctx);
    avformat_close_input(&input_ctx);

    // Удаляем временный файл только при успешном завершении
    _wremove(temp_filename.c_str());
}

void PushRecordingAudio(std::vector<float>&& audioData, int sampleRate, int channels)
{
    if (audioData.empty() || sampleRate <= 0 || channels <= 0)
        return;

    constexpr size_t MAX_QUEUE_SIZE = 10;
    std::lock_guard<std::mutex> lock(g_recording_mutex);
    if (g_recording_queue.size() >= MAX_QUEUE_SIZE)
        return;

    AudioChunk chunk;
    chunk.sample_rate = sampleRate;
    chunk.channels = channels;
    chunk.data = std::move(audioData);
    g_recording_queue.push(std::move(chunk));
}
