#pragma once
#include "resource.h"
#include "util.h"

#ifndef MAIN_H
#define MAIN_H

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <conio.h>
#include <cmath>
#include <deque>
#include <mutex>
#include <iomanip>
#include <vector>
#include <map>
#include <algorithm>
#include <sstream>
#include <SDL2/SDL.h>
#include <queue>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}

#define WM_APP_PLAYBACK_ERROR (WM_APP + 2)
#define WM_APP_UPDATE_COVER (WM_APP + 4)
// -------------------------------
// Constants
// -------------------------------
constexpr int WASAPI_BUFFER_MS = 200;
constexpr int METADATA_UPDATE_INTERVAL_MS = 1000; // Metadata check interval 1 sec

// -------------------------------
// Global variables
// -------------------------------
extern std::atomic<bool> running;
extern std::atomic_bool g_quit_flag;
extern std::string current_track;
extern std::string current_metadata;
extern std::mutex metadata_mutex;
extern std::mutex g_coverFileMutex;
extern std::atomic<int> metadata_interval;
extern std::atomic<time_t> last_metadata_update;
extern std::vector<std::wstring> vec_url;
// Global variables for track history
extern std::vector<std::string> track_history;
extern const int MAX_HISTORY_SIZE;
extern std::atomic<int> total_tracks_played;

extern std::atomic<bool> network_error;
extern int reconnect_attempts;
extern const int MAX_RECONNECT_ATTEMPTS;
extern const int RECONNECT_DELAY_MS;

extern AVCodecContext* codecCtx;
extern AVFormatContext* formatCtx;

extern AVFilterGraph* filterGraph;
extern AVFilterContext* filter_abuf;
extern AVFilterContext* filter_aeq;
extern AVFilterContext* filter_avol;
extern AVFilterContext* filter_asink;
extern std::atomic<float> current_volume;
extern std::atomic<float> current_eq_gain;
extern std::atomic<float> current_eq_gain_bass;
//защита для сообщений от старых потоков
extern std::atomic<unsigned long> g_playbackGeneration;
extern bool g_enableStereoWidth;
extern bool g_enableDynamicAutoVolume;
extern bool g_enableExciter;
extern bool g_enableDeepBass;
extern bool g_enableLimiterGainRider;
extern bool g_minimizeToTray;
extern bool g_showTrackToastInTray;
extern bool g_trackToastPositionSaved;
extern int g_trackToastX;
extern int g_trackToastY;
extern int g_stereoWidthPercent;

extern std::atomic<bool> showcqt_running;
extern std::thread showcqt_thread;

extern std::thread g_playbackControlThread;

extern std::thread g_imageUrlThread;
extern std::atomic<bool> g_bIsImageUrlThreadRunning;
extern CRITICAL_SECTION g_url_vec_cs;
extern bool rec_is_flac;
// -------------------------------
// Global UI Handles
// -------------------------------
extern HWND g_hMainWnd;
extern HWND g_hCoverArt;
extern HWND g_hPlaylist;
extern HWND g_hHistory;
extern HWND g_hBtnPlayPause, g_hBtnStop, g_hBtnOpen, g_hBtnPrev, g_hBtnNext;
extern HWND g_hSliderVolume, g_hSliderTreble;
extern HWND g_hLabelVolume, g_hLabelTreble;

constexpr int SHOWCQT_WIDTH = 560;
constexpr int SHOWCQT_HEIGHT = 220;
constexpr int SHOWCQT_FPS = 60;
extern const int alwaysVisibleExtent;
extern const int maxVisibleExtent;

// -------------------------------
#define WM_APP_METADATA_UPDATED (WM_APP + 1)
#define WM_APP_COVER_RESTORE_AFTER_UAC (WM_APP + 9)
#define WM_APP_PLAYLIST_NAME_RESOLVED (WM_APP + 10)
#define WM_APP_FFMPEG_STATUS (WM_APP + 11)
#define WM_APP_STREAM_INFO (WM_APP + 12)
#define WM_APP_LIMITER_RIDER_STATUS (WM_APP + 13)
#define WM_APP_TRAY_ICON (WM_APP + 14)
#define WM_APP_COVER_DOWNLOADED (WM_APP + 5)
#define WM_APP_UPDATE_ICON (WM_APP + 6)
#define WM_APP_ELAPSED_TIME  (WM_APP + 7)
#define WM_APP_SET_VOLUME_SLIDER (WM_APP + 8)

HRESULT InitWASAPI(IAudioClient*& audioClient, IAudioRenderClient*& renderClient, WAVEFORMATEX*& pwfx, UINT32& bufferFrameCount);
SwrContext* InitSwResample(AVCodecContext* inputCtx, WAVEFORMATEX* wasapiFormat);
bool CanBypassSwResample(AVCodecContext* inputCtx, WAVEFORMATEX* wasapiFormat);
int init_audio_filter_graph(AVFilterGraph* graph, enum AVSampleFormat sample_fmt, int sample_rate, uint64_t channel_layout, float volume_start, AVFilterContext** out_abuf, AVFilterContext** out_aeq, AVFilterContext** out_avol, AVFilterContext** out_asink);
//void MetadataThread(AVFormatContext* formatCtx);

void PlaybackLoop(AVFormatContext*& formatCtx, AVCodecContext*& codecCtx, int& audioStreamIndex, IAudioClient* audioClient, IAudioRenderClient* renderClient, UINT32 bufferFrameCount, WAVEFORMATEX* pwfx, SwrContext*& swr, const char* radioUrl, unsigned long playbackGeneration);

void ProcessAudioForShowCQT(const float* audio_data, int samples, int sample_rate, int channels);
//void update_stream_metadata(AVFormatContext* formatCtx);
void update_stream_metadata();
bool ReinitializeWASAPI(IAudioClient*& audioClient, IAudioRenderClient*& renderClient, WAVEFORMATEX*& pwfx, UINT32& bufferFrameCount);
//showcqt
void InitShowCQT();
void ShowCQTThread();
void CleanupShowCQT();
void UpdateFilterSettings();

void PostFfmpegStatus(const std::wstring& status);
void StopPlayback();

#endif // MAIN_H
