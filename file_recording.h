#pragma once

#include <atomic>
#include <vector>

extern std::atomic<bool> g_is_recording;
extern std::atomic<bool> g_rec_semafor;

void StartRecording();
void StopRecording();
void PushRecordingAudio(std::vector<float>&& audioData, int sampleRate, int channels);
