#include "audio_dsp.h"
#include "IRPFFmpeg.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
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

constexpr double PI = 3.14159265358979323846;

struct Biquad {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    double z1 = 0.0;
    double z2 = 0.0;

    double Process(double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

constexpr int LUFS_NORMALIZER_MAX_CHANNELS = 8;
constexpr size_t LUFS_NORMALIZER_MAX_BLOCKS = 150;
constexpr float g_referenceLufs = -8.10f;

struct LufsBlock {
    double energy = 0.0;
    uint64_t frames = 0;
};

struct LufsGainNormalizerState {
    int sample_rate = 0;
    int channels = 0;
    uint64_t block_frames = 0;
    uint64_t frames_in_block = 0;
    uint64_t frames_seen = 0;
    double block_energy_sum = 0.0;
    double rolling_energy_sum = 0.0;
    uint64_t rolling_frames = 0;
    size_t block_pos = 0;
    size_t block_count = 0;
    float current_lufs = -std::numeric_limits<float>::infinity();
    float desired_gain_db = 0.0f;
    float smoothed_gain_db = 0.0f;
    Biquad prefilter[LUFS_NORMALIZER_MAX_CHANNELS];
    Biquad rlb_highpass[LUFS_NORMALIZER_MAX_CHANNELS];
    LufsBlock blocks[LUFS_NORMALIZER_MAX_BLOCKS];
};

LufsGainNormalizerState g_lufs_normalizer;
std::chrono::steady_clock::time_point g_lufsNormalizerLastPost =
    std::chrono::steady_clock::now() - std::chrono::milliseconds(500);

void SetHighPass(Biquad& biquad, double sample_rate, double freq_hz, double q)
{
    const double w0 = 2.0 * PI * freq_hz / sample_rate;
    const double cos_w0 = std::cos(w0);
    const double sin_w0 = std::sin(w0);
    const double alpha = sin_w0 / (2.0 * q);
    const double a0 = 1.0 + alpha;

    biquad.b0 = ((1.0 + cos_w0) * 0.5) / a0;
    biquad.b1 = (-(1.0 + cos_w0)) / a0;
    biquad.b2 = ((1.0 + cos_w0) * 0.5) / a0;
    biquad.a1 = (-2.0 * cos_w0) / a0;
    biquad.a2 = (1.0 - alpha) / a0;
    biquad.z1 = 0.0;
    biquad.z2 = 0.0;
}

void SetHighShelf(Biquad& biquad, double sample_rate, double freq_hz, double gain_db, double q)
{
    const double A = std::pow(10.0, gain_db / 40.0);
    const double w0 = 2.0 * PI * freq_hz / sample_rate;
    const double cos_w0 = std::cos(w0);
    const double sin_w0 = std::sin(w0);
    const double alpha = sin_w0 / (2.0 * q);
    const double sqrt_A = std::sqrt(A);

    const double b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + 2.0 * sqrt_A * alpha);
    const double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0);
    const double b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - 2.0 * sqrt_A * alpha);
    const double a0 = (A + 1.0) - (A - 1.0) * cos_w0 + 2.0 * sqrt_A * alpha;
    const double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0);
    const double a2 = (A + 1.0) - (A - 1.0) * cos_w0 - 2.0 * sqrt_A * alpha;

    biquad.b0 = b0 / a0;
    biquad.b1 = b1 / a0;
    biquad.b2 = b2 / a0;
    biquad.a1 = a1 / a0;
    biquad.a2 = a2 / a0;
    biquad.z1 = 0.0;
    biquad.z2 = 0.0;
}

void ResetLufsGainNormalizerState()
{
    g_lufs_normalizer = LufsGainNormalizerState();
    g_lufsNormalizerLastPost =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(500);
}

void InitLufsGainNormalizerState(int channels, int sample_rate)
{
    g_lufs_normalizer = LufsGainNormalizerState();
    g_lufs_normalizer.sample_rate = sample_rate;
    g_lufs_normalizer.channels = channels;
    g_lufs_normalizer.block_frames = static_cast<uint64_t>(sample_rate) * 400ULL / 1000ULL;
    if (g_lufs_normalizer.block_frames == 0) {
        g_lufs_normalizer.block_frames = 1;
    }

    const int active_ch = (channels <= LUFS_NORMALIZER_MAX_CHANNELS)
        ? channels
        : LUFS_NORMALIZER_MAX_CHANNELS;

    for (int ch = 0; ch < active_ch; ++ch) {
        SetHighShelf(g_lufs_normalizer.prefilter[ch], sample_rate, 1681.974450955533, 4.0, 0.7071752369554196);
        SetHighPass(g_lufs_normalizer.rlb_highpass[ch], sample_rate, 38.13547087602444, 0.5);
    }
}

double LufsChannelWeight(int ch, int channels)
{
    if (channels > 3 && ch >= 3) {
        return 1.41;
    }
    return 1.0;
}

float ClampFloat(float value, float min_value, float max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

void PushLufsNormalizerBlock(double energy_sum, uint64_t frames)
{
    if (frames == 0) {
        return;
    }

    constexpr double kAbsoluteGateLufs = -70.0;
    const double block_mean_square = energy_sum / static_cast<double>(frames);
    const double block_lufs = (block_mean_square > 0.0)
        ? (-0.691 + 10.0 * std::log10(block_mean_square))
        : -std::numeric_limits<double>::infinity();

    if (block_lufs < kAbsoluteGateLufs) {
        return;
    }

    LufsBlock& slot = g_lufs_normalizer.blocks[g_lufs_normalizer.block_pos];
    if (g_lufs_normalizer.block_count == LUFS_NORMALIZER_MAX_BLOCKS) {
        g_lufs_normalizer.rolling_energy_sum -= slot.energy;
        g_lufs_normalizer.rolling_frames -= slot.frames;
    }
    else {
        ++g_lufs_normalizer.block_count;
    }

    slot.energy = energy_sum;
    slot.frames = frames;
    g_lufs_normalizer.rolling_energy_sum += energy_sum;
    g_lufs_normalizer.rolling_frames += frames;
    g_lufs_normalizer.block_pos =
        (g_lufs_normalizer.block_pos + 1) % LUFS_NORMALIZER_MAX_BLOCKS;

    if (g_lufs_normalizer.rolling_frames == 0 || g_lufs_normalizer.rolling_energy_sum <= 0.0) {
        return;
    }

    const double rolling_mean_square =
        g_lufs_normalizer.rolling_energy_sum / static_cast<double>(g_lufs_normalizer.rolling_frames);
    const double rolling_lufs = -0.691 + 10.0 * std::log10(rolling_mean_square);

    constexpr uint64_t kMinAnalysisMs = 3000;
    const uint64_t min_frames =
        static_cast<uint64_t>(g_lufs_normalizer.sample_rate) * kMinAnalysisMs / 1000ULL;
    if (g_lufs_normalizer.rolling_frames < min_frames) {
        return;
    }

    g_lufs_normalizer.current_lufs = static_cast<float>(rolling_lufs);
    g_lufs_normalizer.desired_gain_db =
        ClampFloat(g_referenceLufs - g_lufs_normalizer.current_lufs, -12.0f, 8.0f);
}

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

void PostLufsGainNormalizerStatus()
{
    if (!g_hMainWnd) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - g_lufsNormalizerLastPost < std::chrono::milliseconds(500)) {
        return;
    }
    g_lufsNormalizerLastPost = now;

    float gain_db = g_lufs_normalizer.smoothed_gain_db;
    if (std::fabs(gain_db) < 0.05f) {
        gain_db = 0.0f;
    }

    wchar_t text[96] = {};
    swprintf_s(text, L"LG %c%3.1f", gain_db < 0.0f ? L'-' : L'+', std::fabs(gain_db));

    auto* payload = new std::wstring(text);
    if (!PostMessageW(g_hMainWnd, WM_APP_LUFS_NORMALIZER_STATUS, 0, reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

} // namespace

void ResetRealtimeAudioDspState()
{
    g_dynamic_autovol = DynamicAutoVolState();
    ResetLufsGainNormalizerState();
    ResetDCBlocker();
    ResetExciterState();
    ResetDeepBassState();
}

void AnalyzeLufsGainNormalizer(const float* buffer, size_t frames, int channels, int sample_rate)
{
    if (!buffer || frames == 0 || channels <= 0 || sample_rate <= 0) {
        return;
    }
    if (g_lufs_normalizer.sample_rate != sample_rate ||
        g_lufs_normalizer.channels != channels) {
        InitLufsGainNormalizerState(channels, sample_rate);
    }

    const int active_ch = (channels <= LUFS_NORMALIZER_MAX_CHANNELS)
        ? channels
        : LUFS_NORMALIZER_MAX_CHANNELS;

    for (size_t frame = 0; frame < frames; ++frame) {
        double frame_energy = 0.0;

        for (int ch = 0; ch < active_ch; ++ch) {
            const size_t index = frame * static_cast<size_t>(channels) + static_cast<size_t>(ch);
            double sample = static_cast<double>(buffer[index]);
            if (!std::isfinite(sample)) {
                sample = 0.0;
            }

            double filtered = g_lufs_normalizer.prefilter[ch].Process(sample);
            filtered = g_lufs_normalizer.rlb_highpass[ch].Process(filtered);
            frame_energy += LufsChannelWeight(ch, channels) * filtered * filtered;
        }

        g_lufs_normalizer.block_energy_sum += frame_energy;
        ++g_lufs_normalizer.frames_in_block;
        ++g_lufs_normalizer.frames_seen;

        if (g_lufs_normalizer.frames_in_block >= g_lufs_normalizer.block_frames) {
            PushLufsNormalizerBlock(
                g_lufs_normalizer.block_energy_sum,
                g_lufs_normalizer.frames_in_block);
            g_lufs_normalizer.block_energy_sum = 0.0;
            g_lufs_normalizer.frames_in_block = 0;
        }
    }
}

void ApplyLufsGainNormalizer(float* buffer, size_t frames, int channels, int sample_rate)
{
    if (!buffer || frames == 0 || channels <= 0 || sample_rate <= 0) {
        return;
    }
    if (g_lufs_normalizer.sample_rate != sample_rate ||
        g_lufs_normalizer.channels != channels) {
        return;
    }

    const float old_gain_db = g_lufs_normalizer.smoothed_gain_db;
    const float seconds = static_cast<float>(frames) / static_cast<float>(sample_rate);
    const float max_step_db = 0.50f * seconds;
    const float diff_db = g_lufs_normalizer.desired_gain_db - old_gain_db;

    if (diff_db > max_step_db) {
        g_lufs_normalizer.smoothed_gain_db += max_step_db;
    }
    else if (diff_db < -max_step_db) {
        g_lufs_normalizer.smoothed_gain_db -= max_step_db;
    }
    else {
        g_lufs_normalizer.smoothed_gain_db = g_lufs_normalizer.desired_gain_db;
    }

    const float new_gain_db = g_lufs_normalizer.smoothed_gain_db;
    const float old_gain = std::pow(10.0f, old_gain_db / 20.0f);
    const float new_gain = std::pow(10.0f, new_gain_db / 20.0f);
    const float gain_step = (frames > 1)
        ? ((new_gain - old_gain) / static_cast<float>(frames - 1))
        : 0.0f;
    float gain = old_gain;

    for (size_t frame = 0; frame < frames; ++frame) {
        float* frame_ptr = buffer + frame * static_cast<size_t>(channels);
        for (int ch = 0; ch < channels; ++ch) {
            float sample = frame_ptr[ch];
            if (!std::isfinite(sample)) {
                sample = 0.0f;
            }
            frame_ptr[ch] = sample * gain;
        }
        gain += gain_step;
    }

    PostLufsGainNormalizerStatus();
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
