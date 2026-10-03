#include "walle_audio_codec.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <dsps_fft2r.h>
#include <dsps_wind_hann.h>
#include <esp_err.h>
#include <esp_log.h>

#include "settings.h"

#define TAG "WalleAudioCodec"

namespace {
// dB range mapped to the 0-255 band/peak values. -60 dBFS is a quiet TTS passage, 0 dBFS is a
// loud peak; tuned by eye against the actual ST7789 once flashed, not derived from a spec sheet.
constexpr float kMagnitudeFloorDb = -60.0f;
constexpr float kMagnitudeCeilDb = 0.0f;
constexpr int kPeakDecayPerWindow = 6;  // ~0.18 s to fall from full scale to zero, once falling
constexpr int kPeakHoldWindows = 23;    // ~1 s at ~23 windows/s before a peak starts falling
}  // namespace

WalleAudioCodec::WalleAudioCodec(int input_sample_rate, int output_sample_rate,
                                 gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                                 gpio_num_t mic_clk, gpio_num_t mic_data)
    : NoAudioCodecSimplexPdm(input_sample_rate, output_sample_rate, spk_bclk, spk_ws, spk_dout,
                             mic_clk, mic_data) {
    if (dsps_fft2r_init_fc32(nullptr, kFftSize) != ESP_OK) {
        ESP_LOGE(TAG, "FFT init failed; the visualizer will stay blank");
    }
    dsps_wind_hann_f32(hann_window_, kFftSize);
    InitSpectrumBands();
}

// 32 log-spaced bands from 80 Hz to 8 kHz (below Nyquist at any of this board's output rates) -
// log spacing because that is what makes a voice spectrum look interesting: linear spacing would
// spend 31 of the 32 bars on frequencies above where most of the energy in speech actually is.
void WalleAudioCodec::InitSpectrumBands() {
    constexpr float kMinHz = 80.0f;
    constexpr float kMaxHz = 8000.0f;
    const float bin_hz = static_cast<float>(output_sample_rate()) / kFftSize;
    const float log_min = log10f(kMinHz);
    const float log_max = log10f(kMaxHz);
    int prev_hi = 0;
    for (int band = 0; band < walle_spectrum::kBands; ++band) {
        const float t0 = static_cast<float>(band) / walle_spectrum::kBands;
        const float t1 = static_cast<float>(band + 1) / walle_spectrum::kBands;
        const float f0 = powf(10.0f, log_min + t0 * (log_max - log_min));
        const float f1 = powf(10.0f, log_min + t1 * (log_max - log_min));
        int lo = std::max(1, static_cast<int>(f0 / bin_hz));  // bin 0 is DC, skip it
        lo = std::max(lo, prev_hi + 1);  // never overlap the previous band
        int hi = std::max(lo, static_cast<int>(f1 / bin_hz));
        hi = std::min(hi, kFftSize / 2 - 1);
        band_ranges_[band] = {lo, hi};
        prev_hi = hi;
    }
}

void WalleAudioCodec::SetMicGainDb(int db) {
    db = std::clamp(db, 0, 24);
    gain_db_ = db;
    // Q8 fixed point: 18 dB -> 7.94x -> 2033/256.
    gain_q8_ = static_cast<int>(std::lround(std::pow(10.0, db / 20.0) * 256.0));
    ESP_LOGI(TAG, "Mic gain %d dB", db);
}

int WalleAudioCodec::Read(int16_t* dest, int samples) {
    // input_gain_ stays 0, so the base class copies the PDM samples unchanged.
    int n = NoAudioCodecSimplexPdm::Read(dest, samples);
    const int32_t q8 = gain_q8_.load();
    const bool muted = muted_.load();

    int32_t peak = 0;
    uint64_t sum_squares = 0;
    uint32_t clipped = 0;
    for (int i = 0; i < n; ++i) {
        int32_t v = muted ? 0 : (static_cast<int32_t>(dest[i]) * q8) >> 8;
        if (v > INT16_MAX) {
            v = INT16_MAX;
            ++clipped;
        } else if (v < -INT16_MAX) {
            v = -INT16_MAX;
            ++clipped;
        }
        dest[i] = static_cast<int16_t>(v);
        const int32_t magnitude = std::abs(v);
        peak = std::max(peak, magnitude);
        sum_squares += static_cast<uint64_t>(magnitude) * magnitude;
    }

    input_rms_ = n > 0 ? static_cast<int>(std::sqrt(static_cast<double>(sum_squares) / n)) : 0;
    std::lock_guard<std::mutex> lock(level_mutex_);
    peak_ = std::max(peak_, peak);
    sum_squares_ += sum_squares;
    count_ += n;
    clipped_ += clipped;
    return n;
}

int WalleAudioCodec::Write(const int16_t* data, int samples) {
    uint64_t sum_squares = 0;
    for (int i = 0; i < samples; ++i) {
        const int32_t v = data[i];
        sum_squares += static_cast<uint64_t>(v * v);
    }
    output_rms_ =
        samples > 0 ? static_cast<int>(std::sqrt(static_cast<double>(sum_squares) / samples)) : 0;

    // Accumulate into fixed-size, non-overlapping windows; whichever Write() call happens to
    // fill one runs the analysis inline. Write() only runs while something is actually playing,
    // so this costs nothing while Jarvis is silent, and only a fraction of a millisecond per
    // ~43 ms window while it is not - nowhere near enough to risk an audio underrun.
    if (spectrum_enabled_.load()) {
        int offset = 0;
        while (offset < samples) {
            const int space = kFftSize - fft_accum_count_;
            const int take = std::min(space, samples - offset);
            std::memcpy(fft_accum_ + fft_accum_count_, data + offset,
                       static_cast<size_t>(take) * sizeof(int16_t));
            fft_accum_count_ += take;
            offset += take;
            if (fft_accum_count_ == kFftSize) {
                AnalyzeSpectrumWindow();
                fft_accum_count_ = 0;
            }
        }
    }
    return NoAudioCodecSimplexPdm::Write(data, samples);
}

// Windowed real FFT, packed as a same-size complex FFT with the imaginary half left at zero.
// That is twice the work a "true" real FFT needs, but at N=1024 it is still well under a
// millisecond on this core - simpler and harder to get wrong than the two-real-signals-in-one-
// complex-FFT trick, for a cost that is negligible next to the ~43 ms this window represents.
void WalleAudioCodec::AnalyzeSpectrumWindow() {
    // Oscilloscope snapshot first, from the raw window: every (kFftSize / kWavePoints)th sample,
    // scaled so the window's peak nearly fills the strip, but never boosted past a fixed floor so
    // near-silence stays a flat line instead of amplified hiss.
    constexpr int kWaveStep = kFftSize / walle_spectrum::kWavePoints;
    constexpr int kWaveFloor = 2500;
    int window_peak = kWaveFloor;
    for (int i = 0; i < kFftSize; ++i) {
        window_peak = std::max(window_peak, std::abs(static_cast<int>(fft_accum_[i])));
    }
    int8_t wave[walle_spectrum::kWavePoints];
    for (int j = 0; j < walle_spectrum::kWavePoints; ++j) {
        wave[j] = static_cast<int8_t>(
            std::clamp(fft_accum_[j * kWaveStep] * 127 / window_peak, -127, 127));
    }

    for (int i = 0; i < kFftSize; ++i) {
        fft_buffer_[2 * i] = fft_accum_[i] * hann_window_[i];
        fft_buffer_[2 * i + 1] = 0.0f;
    }
    dsps_fft2r_fc32(fft_buffer_, kFftSize);
    dsps_bit_rev_fc32(fft_buffer_, kFftSize);

    walle_spectrum::Frame frame;
    {
        std::lock_guard<std::mutex> lock(spectrum_mutex_);
        frame = spectrum_;  // keep the previous peaks to decay from
    }
    std::memcpy(frame.wave, wave, sizeof(wave));
    for (int band = 0; band < walle_spectrum::kBands; ++band) {
        float peak_mag = 0.0f;
        for (int bin = band_ranges_[band].lo; bin <= band_ranges_[band].hi; ++bin) {
            const float re = fft_buffer_[2 * bin];
            const float im = fft_buffer_[2 * bin + 1];
            peak_mag = std::max(peak_mag, sqrtf(re * re + im * im));
        }
        const float normalized = peak_mag / (kFftSize / 2.0f) / 32768.0f;
        const float db = 20.0f * log10f(normalized + 1e-6f);
        const float scaled =
            (db - kMagnitudeFloorDb) / (kMagnitudeCeilDb - kMagnitudeFloorDb) * 255.0f;
        const uint8_t value = static_cast<uint8_t>(std::clamp(scaled, 0.0f, 255.0f));
        frame.bands[band] = value;
        // Classic Winamp peak-hold: sit at the peak for a beat, then fall - not a continuous
        // decay from the moment it's set, which read as jittery rather than a held peak dot.
        if (value >= frame.peaks[band]) {
            frame.peaks[band] = value;
            peak_hold_windows_[band] = kPeakHoldWindows;
        } else if (peak_hold_windows_[band] > 0) {
            --peak_hold_windows_[band];
        } else {
            frame.peaks[band] =
                static_cast<uint8_t>(std::max(0, frame.peaks[band] - kPeakDecayPerWindow));
        }
    }
    std::lock_guard<std::mutex> lock(spectrum_mutex_);
    spectrum_ = frame;
}

walle_spectrum::Frame WalleAudioCodec::TakeSpectrum() const {
    std::lock_guard<std::mutex> lock(spectrum_mutex_);
    return spectrum_;
}

WalleAudioCodec::Level WalleAudioCodec::TakeLevel() {
    std::lock_guard<std::mutex> lock(level_mutex_);
    Level level;
    level.samples = count_;
    level.clipped = clipped_;
    level.peak_dbfs = peak_ > 0 ? 20.0f * std::log10(peak_ / 32768.0f) : -96.0f;
    const double rms = count_ > 0 ? std::sqrt(static_cast<double>(sum_squares_) / count_) : 0.0;
    level.rms_dbfs = rms > 0.5 ? static_cast<float>(20.0 * std::log10(rms / 32768.0)) : -96.0f;
    peak_ = 0;
    sum_squares_ = 0;
    count_ = 0;
    clipped_ = 0;
    return level;
}

void WalleAudioCodec::Start() {
    NoAudioCodecSimplexPdm::Start();  // restores the saved output volume
    Settings settings("walle", false);
    requested_volume_ = settings.GetInt("req_volume", output_volume_);
    const int effective = std::min(requested_volume_, volume_cap_);
    if (effective != output_volume_) {
        NoAudioCodecSimplexPdm::SetOutputVolume(effective);
    }
}

void WalleAudioCodec::SetOutputVolume(int volume) {
    requested_volume_ = std::clamp(volume, 0, 100);
    {
        Settings settings("walle", true);
        settings.SetInt("req_volume", requested_volume_);
    }
    const int effective = std::min(requested_volume_, volume_cap_);
    if (effective < requested_volume_) {
        ESP_LOGI(TAG, "Volume %d capped to %d", requested_volume_, effective);
    }
    NoAudioCodecSimplexPdm::SetOutputVolume(effective);
}

void WalleAudioCodec::SetVolumeCap(int cap) {
    volume_cap_ = std::clamp(cap, 0, 100);
    const int effective = std::min(requested_volume_, volume_cap_);
    if (effective != output_volume_) {
        NoAudioCodecSimplexPdm::SetOutputVolume(effective);
    }
}
