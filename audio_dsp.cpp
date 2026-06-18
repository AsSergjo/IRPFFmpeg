#include "audio_dsp.h"
#include "IRPFFmpeg.h"

#include <chrono>
#include <cmath>
#include <string>

namespace {

struct DynamicAutoVolState {
    float current_gain = 1.0f;
};

DynamicAutoVolState g_dynamic_autovol;

constexpr int EXCITER_MAX_CHANNELS = 8;
constexpr int DEEP_BASS_MAX_CHANNELS = 8;

struct ExciterState {
    float lowpass = 0.0f;
};

ExciterState g_exciter[EXCITER_MAX_CHANNELS];

struct DeepBassState {
    float lp1 = 0.0f;
    float lp2 = 0.0f;
    float hp_prev_x = 0.0f;
    float hp_prev_y = 0.0f;
};

DeepBassState g_deep_bass[DEEP_BASS_MAX_CHANNELS];

constexpr int DC_BLOCKER_MAX_CHANNELS = 8;

struct DCBlocker {
    float x_prev = 0.0f;
    float y_prev = 0.0f;
};

DCBlocker g_dc_blocker[DC_BLOCKER_MAX_CHANNELS];

void ResetDCBlocker()
{
    for (int ch = 0; ch < DC_BLOCKER_MAX_CHANNELS; ++ch) {
        g_dc_blocker[ch].x_prev = 0.0f;
        g_dc_blocker[ch].y_prev = 0.0f;
    }
}

void ResetExciterState()
{
    for (int ch = 0; ch < EXCITER_MAX_CHANNELS; ++ch) {
        g_exciter[ch].lowpass = 0.0f;
    }
}

void ResetDeepBassState()
{
    for (int ch = 0; ch < DEEP_BASS_MAX_CHANNELS; ++ch) {
        g_deep_bass[ch] = DeepBassState();
    }
}

float DCBlockerCoeff(float fc_hz, int sample_rate)
{
    if (sample_rate <= 0 || fc_hz <= 0.0f) return 0.9997f;

    float R = expf(-2.0f * 3.14159265f * fc_hz / static_cast<float>(sample_rate));
    if (R < 0.9f) R = 0.9f;
    if (R > 0.99999f) R = 0.99999f;
    return R;
}

float ProcessDeepBassSample(float x, DeepBassState& state,
    float lp_coeff, float hp_r, float drive, float norm)
{
    if (!std::isfinite(x)) x = 0.0f;

    state.lp1 += lp_coeff * (x - state.lp1);
    state.lp2 += lp_coeff * (state.lp1 - state.lp2);

    float low = state.lp2;
    float cleaned = low - state.hp_prev_x + hp_r * state.hp_prev_y;
    state.hp_prev_x = low;
    state.hp_prev_y = cleaned;

    if (!std::isfinite(cleaned)) {
        state.hp_prev_y = 0.0f;
        return 0.0f;
    }

    float shaped = tanhf(cleaned * drive);
    if (norm > 0.0f) {
        shaped /= norm;
    }

    return std::isfinite(shaped) ? shaped : 0.0f;
}

float g_limiterGainRiderGain = 1.0f;
std::chrono::steady_clock::time_point g_limiterGainRiderLastPost =
    std::chrono::steady_clock::now() - std::chrono::milliseconds(200);

void PostLimiterGainRiderStatus(const FinalLimiterActivity& activity, int total_samples)
{
    (void)activity;

    if (!g_hMainWnd || total_samples <= 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - g_limiterGainRiderLastPost < std::chrono::milliseconds(200)) {
        return;
    }
    g_limiterGainRiderLastPost = now;

    wchar_t text[80] = {};
    swprintf_s(text, L"GR %4.2f", g_limiterGainRiderGain);

    auto* payload = new std::wstring(text);
    if (!PostMessageW(g_hMainWnd, WM_APP_LIMITER_RIDER_STATUS, 0, reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

} // namespace

void ResetRealtimeAudioDspState()
{
    g_dynamic_autovol = DynamicAutoVolState();
    ResetDCBlocker();
    ResetExciterState();
    ResetDeepBassState();
}

void RemoveDCOffset(float* buffer, size_t frames, int channels, int sample_rate)
{
    if (!buffer || frames == 0 || channels <= 0) return;

    const int active_ch = (channels <= DC_BLOCKER_MAX_CHANNELS)
        ? channels
        : DC_BLOCKER_MAX_CHANNELS;

    const float R = DCBlockerCoeff(5.0f, sample_rate);
    const size_t total_samples = frames * static_cast<size_t>(channels);

    for (int ch = 0; ch < active_ch; ++ch) {
        float x_prev = g_dc_blocker[ch].x_prev;
        float y_prev = g_dc_blocker[ch].y_prev;

        for (size_t i = static_cast<size_t>(ch); i < total_samples; i += channels) {
            float x = buffer[i];
            if (!std::isfinite(x)) x = 0.0f;

            float y = x - x_prev + R * y_prev;
            if (!std::isfinite(y)) y = 0.0f;

            x_prev = x;
            y_prev = y;
            buffer[i] = y;
        }

        g_dc_blocker[ch].x_prev = x_prev;
        g_dc_blocker[ch].y_prev = y_prev;
    }
}

void ProcessDynamicAutoVolume(float* buffer, size_t frames, int channels, int sample_rate)
{
    if (!buffer || frames == 0) return;
    if (channels <= 0 || sample_rate <= 0) return;

    constexpr float kTargetRms = 0.55f;
    constexpr float kSilenceGate = 0.006f;
    constexpr float kMinGain = 0.30f;
    constexpr float kMaxGain = 2.0f;
    constexpr float kPeakCeiling = 0.96f;
    constexpr float kAttackMs = 40.0f;
    constexpr float kReleaseMs = 1600.0f;

    size_t subblock_frames = static_cast<size_t>(sample_rate / 100);
    if (subblock_frames == 0) {
        subblock_frames = 1;
    }

    const size_t channel_count = static_cast<size_t>(channels);
    size_t processed_frames = 0;

    while (processed_frames < frames) {
        size_t block_frames = frames - processed_frames;
        if (block_frames > subblock_frames) {
            block_frames = subblock_frames;
        }

        float* block = buffer + processed_frames * channel_count;
        double max_channel_sum_sq = 0.0;
        float peak_abs = 0.0f;

        for (int ch = 0; ch < channels; ++ch) {
            double channel_sum_sq = 0.0;

            for (size_t frame = 0; frame < block_frames; ++frame) {
                const float sample = block[frame * channel_count + static_cast<size_t>(ch)];
                channel_sum_sq += static_cast<double>(sample) * static_cast<double>(sample);

                const float abs_sample = fabsf(sample);
                if (abs_sample > peak_abs) {
                    peak_abs = abs_sample;
                }
            }

            if (channel_sum_sq > max_channel_sum_sq) {
                max_channel_sum_sq = channel_sum_sq;
            }
        }

        const double rms = sqrt(max_channel_sum_sq / static_cast<double>(block_frames));
        const bool silence = (!(rms > 0.0) || rms < static_cast<double>(kSilenceGate));

        float desired_gain = silence ? 1.0f : static_cast<float>(static_cast<double>(kTargetRms) / rms);
        if (desired_gain < kMinGain) desired_gain = kMinGain;
        if (desired_gain > kMaxGain) desired_gain = kMaxGain;

        float peak_safe_gain = kMaxGain;
        if (peak_abs > 0.0f) {
            peak_safe_gain = kPeakCeiling / peak_abs;
            if (desired_gain > peak_safe_gain) {
                desired_gain = peak_safe_gain;
            }
        }

        float old_gain = g_dynamic_autovol.current_gain;
        if (!(old_gain > 0.0f)) {
            old_gain = 1.0f;
        }

        const float block_ms = 1000.0f * static_cast<float>(block_frames) / static_cast<float>(sample_rate);
        const float time_ms = silence ? kReleaseMs
            : ((desired_gain < old_gain) ? kAttackMs : kReleaseMs);
        const float coeff = expf(-block_ms / time_ms);
        float new_gain = desired_gain + (old_gain - desired_gain) * coeff;
        if (new_gain > peak_safe_gain) {
            new_gain = peak_safe_gain;
        }
        g_dynamic_autovol.current_gain = new_gain;

        const float gain_step = (block_frames > 1)
            ? ((new_gain - old_gain) / static_cast<float>(block_frames - 1))
            : 0.0f;
        float gain = old_gain;

        for (size_t frame = 0; frame < block_frames; ++frame) {
            float* frame_ptr = block + frame * channel_count;

            for (int ch = 0; ch < channels; ++ch) {
                frame_ptr[ch] *= gain;
                if (frame_ptr[ch] > 1.0f) frame_ptr[ch] = 1.0f;
                if (frame_ptr[ch] < -1.0f) frame_ptr[ch] = -1.0f;
            }

            gain += gain_step;
        }

        processed_frames += block_frames;
    }
}

void ProcessDeepBass(float* buffer, size_t frames, int channels, int sample_rate, float amount)
{
    if (!buffer || frames == 0) return;
    if (channels <= 0 || sample_rate <= 0) return;
    if (amount <= 0.0f) return;

    const float fs = static_cast<float>(sample_rate);
    const float low_cutoff_hz = 105.0f;
    const float subsonic_cutoff_hz = 20.0f;
    const float lp_coeff = 1.0f - expf(-2.0f * 3.14159265358979f * low_cutoff_hz / fs);
    const float hp_r = expf(-2.0f * 3.14159265358979f * subsonic_cutoff_hz / fs);
    const float drive = 1.55f;
    const float norm = tanhf(drive);
    const size_t total_samples = frames * static_cast<size_t>(channels);

    if (channels == 2) {
        DeepBassState& state = g_deep_bass[0];

        for (size_t i = 0; i + 1 < total_samples; i += 2) {
            float left = buffer[i];
            float right = buffer[i + 1];
            if (!std::isfinite(left)) left = 0.0f;
            if (!std::isfinite(right)) right = 0.0f;

            const float mono = 0.5f * (left + right);
            const float silk = ProcessDeepBassSample(mono, state, lp_coeff, hp_r, drive, norm) * amount;

            buffer[i] = left + silk;
            buffer[i + 1] = right + silk;
        }

        return;
    }

    const int active_ch = (channels <= DEEP_BASS_MAX_CHANNELS)
        ? channels
        : DEEP_BASS_MAX_CHANNELS;

    for (int ch = 0; ch < active_ch; ++ch) {
        DeepBassState& state = g_deep_bass[ch];

        for (size_t i = static_cast<size_t>(ch); i < total_samples; i += channels) {
            float x = buffer[i];
            const float silk = ProcessDeepBassSample(x, state, lp_coeff, hp_r, drive, norm) * amount;
            buffer[i] = (std::isfinite(x) ? x : 0.0f) + silk;
        }
    }
}

void ProcessStereoWidth(float* buffer, size_t frames, int channels, float width)
{
    if (!buffer || frames == 0) return;
    if (channels != 2) return;
    if (width < 0.0f) width = 0.0f;

    const size_t total_samples = frames * static_cast<size_t>(channels);

    for (size_t i = 0; i + 1 < total_samples; i += 2) {
        float L = buffer[i];
        float R = buffer[i + 1];

        float mid = 0.5f * (L + R);
        float side = 0.5f * (L - R);

        side *= width;

        float nl = mid + side;
        float nr = mid - side;

        if (!std::isfinite(nl)) nl = 0.0f;
        if (!std::isfinite(nr)) nr = 0.0f;

        buffer[i] = nl;
        buffer[i + 1] = nr;
    }
}

void ProcessExciter(float* buffer, size_t frames, int channels, int sample_rate, float amount)
{
    if (!buffer || frames == 0) return;
    if (channels <= 0 || sample_rate <= 0) return;
    if (amount <= 0.0f) return;

    const int active_ch = (channels <= EXCITER_MAX_CHANNELS)
        ? channels
        : EXCITER_MAX_CHANNELS;

    const float cutoff_hz = 3500.0f;
    const float alpha = expf(-2.0f * 3.14159265358979f * cutoff_hz /
        static_cast<float>(sample_rate));
    const float drive = 2.4f;
    const float norm = tanhf(drive);
    const size_t total_samples = frames * static_cast<size_t>(channels);

    for (int ch = 0; ch < active_ch; ++ch) {
        float low = g_exciter[ch].lowpass;

        for (size_t i = static_cast<size_t>(ch); i < total_samples; i += channels) {
            float x = buffer[i];
            if (!std::isfinite(x)) x = 0.0f;

            low = (1.0f - alpha) * x + alpha * low;
            float high = x - low;
            float excited = tanhf(high * drive);
            if (norm > 0.0f) {
                excited /= norm;
            }

            float y = x + excited * amount * 0.25f;
            if (!std::isfinite(y)) y = x;
            buffer[i] = y;
        }

        g_exciter[ch].lowpass = low;
    }
}

void ApplyLimiterGainRider(float* audio_data, int total_samples)
{
    if (!audio_data || total_samples <= 0) {
        return;
    }

    if (!g_enableLimiterGainRider) {
        g_limiterGainRiderGain = 1.0f;
        return;
    }

    const float gain = g_limiterGainRiderGain;
    if (fabsf(gain - 1.0f) < 0.0001f) {
        return;
    }

    for (int i = 0; i < total_samples; ++i) {
        float v = audio_data[i];
        if (!std::isfinite(v)) {
            audio_data[i] = 0.0f;
            continue;
        }

        audio_data[i] = v * gain;
    }
}

FinalLimiterActivity ApplyFinalLimiter(float* audio_data, int total_samples, float limit)
{
    FinalLimiterActivity activity;

    if (!audio_data || total_samples <= 0 || limit <= 0.0f) {
        return activity;
    }

    const float threshold = limit * 0.8f;
    const float scale = 1.0f / threshold;

    for (int i = 0; i < total_samples; ++i) {
        float v = audio_data[i];
        if (!std::isfinite(v)) {
            audio_data[i] = 0.0f;
            continue;
        }

        const float abs_v = fabsf(v);
        if (abs_v > activity.maxInputAbs) {
            activity.maxInputAbs = abs_v;
        }

        if (abs_v > threshold) {
            ++activity.softLimitedSamples;
        }
        if (abs_v > limit) {
            ++activity.overLimitSamples;
        }

        if (v > threshold) {
            float overshoot = (v - threshold) * scale;
            audio_data[i] = threshold + (limit - threshold) * tanhf(overshoot);
        }
        else if (v < -threshold) {
            float overshoot = (-v - threshold) * scale;
            audio_data[i] = -threshold - (limit - threshold) * tanhf(overshoot);
        }
    }

    return activity;
}

void ResetLimiterGainRider(bool postStatus)
{
    g_limiterGainRiderGain = 1.0f;
    g_limiterGainRiderLastPost =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(200);

    if (postStatus) {
        FinalLimiterActivity activity;
        PostLimiterGainRiderStatus(activity, 1);
    }
}

void UpdateLimiterGainRider(const FinalLimiterActivity& activity, int total_samples)
{
    if (!g_enableLimiterGainRider) {
        g_limiterGainRiderGain = 1.0f;
        return;
    }

    if (total_samples <= 0) {
        return;
    }

    constexpr float kMinRiderGain = 0.70f;
    constexpr float kStrongAttackMultiplier = 0.99f;
    constexpr float kSoftAttackMultiplier = 0.995f;
    constexpr float kReleaseCoeff = 0.0015f;
    constexpr float kSoftLimitedRatioForAttack = 0.08f;

    const float soft_limited_ratio =
        static_cast<float>(activity.softLimitedSamples) / static_cast<float>(total_samples);

    if (activity.overLimitSamples > 0) {
        g_limiterGainRiderGain *= kStrongAttackMultiplier;
        if (g_limiterGainRiderGain < kMinRiderGain) {
            g_limiterGainRiderGain = kMinRiderGain;
        }
    }
    else if (soft_limited_ratio > kSoftLimitedRatioForAttack) {
        g_limiterGainRiderGain *= kSoftAttackMultiplier;
        if (g_limiterGainRiderGain < kMinRiderGain) {
            g_limiterGainRiderGain = kMinRiderGain;
        }
    }
    else if (g_limiterGainRiderGain < 1.0f) {
        g_limiterGainRiderGain += (1.0f - g_limiterGainRiderGain) * kReleaseCoeff;
        if (g_limiterGainRiderGain > 0.9999f) {
            g_limiterGainRiderGain = 1.0f;
        }
    }

    PostLimiterGainRiderStatus(activity, total_samples);
}
