#pragma once

#include <cstddef>

struct FinalLimiterActivity {
    int softLimitedSamples = 0;
    int overLimitSamples = 0;
    float maxInputAbs = 0.0f;
};

void ResetRealtimeAudioDspState();
void RemoveDCOffset(float* buffer, size_t frames, int channels, int sample_rate);
void ProcessDynamicAutoVolume(float* buffer, size_t frames, int channels, int sample_rate);
void ProcessDeepBass(float* buffer, size_t frames, int channels, int sample_rate, float amount);
void ProcessStereoWidth(float* buffer, size_t frames, int channels, float width);
void ProcessExciter(float* buffer, size_t frames, int channels, int sample_rate, float amount);
void ApplyLimiterGainRider(float* audio_data, int total_samples);
FinalLimiterActivity ApplyFinalLimiter(float* audio_data, int total_samples, float limit);
void ResetLimiterGainRider(bool postStatus = true);
void UpdateLimiterGainRider(const FinalLimiterActivity& activity, int total_samples);
