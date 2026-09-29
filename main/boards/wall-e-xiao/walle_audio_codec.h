#pragma once

#include <atomic>
#include <mutex>

#include "codecs/no_audio_codec.h"
#include "walle_spectrum.h"

// PDM microphone + MAX98357A, with Wall-E extras: software mic gain applied before the
// wake word engine and the upload, a level meter, mic mute, and a volume cap
// (max_volume, lowered further in night mode).
class WalleAudioCodec : public NoAudioCodecSimplexPdm {
public:
    struct Level {
        float peak_dbfs;
        float rms_dbfs;
        int clipped;
        int samples;
    };

    WalleAudioCodec(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk,
                    gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_clk,
                    gpio_num_t mic_data);

    int Read(int16_t* dest, int samples) override;
    void SetOutputVolume(int volume) override;
    void Start() override;

    void SetMicGainDb(int db);
    int mic_gain_db() const { return gain_db_.load(); }
    void SetMicMuted(bool muted) { muted_ = muted; }
    bool mic_muted() const { return muted_.load(); }

    // Effective volume = min(requested volume, cap). The requested volume is remembered.
    void SetVolumeCap(int cap);
    int volume_cap() const { return volume_cap_; }
    int requested_volume() const { return requested_volume_; }

    // Level since the previous call (post-gain, i.e. what the wake word engine hears).
    Level TakeLevel();

    // RMS (0-32767) of the latest chunk, for the level bars under the face.
    int input_rms() const { return input_rms_.load(); }
    int output_rms() const { return output_rms_.load(); }

    // Winamp-style visualizer: a 32-band magnitude spectrum of what's actually being sent to the
    // speaker, refreshed roughly every 43 ms (1024 samples at 24 kHz) inside Write() itself - see
    // the .cc for why that is safe to do inline on the audio task. Skipped entirely (zero extra
    // CPU) unless enabled, since Write() runs continuously whenever anything plays.
    void SetSpectrumEnabled(bool enabled) { spectrum_enabled_ = enabled; }
    walle_spectrum::Frame TakeSpectrum() const;

protected:
    int Write(const int16_t* data, int samples) override;

private:
    static constexpr int kFftSize = 1024;

    void InitSpectrumBands();
    void AnalyzeSpectrumWindow();

    std::atomic<int> input_rms_{0};
    std::atomic<int> output_rms_{0};

    std::atomic<int> gain_db_{0};
    std::atomic<int> gain_q8_{256};
    std::atomic<bool> muted_{false};
    int volume_cap_ = 100;
    int requested_volume_ = 70;

    std::mutex level_mutex_;
    int32_t peak_ = 0;
    uint64_t sum_squares_ = 0;
    uint32_t count_ = 0;
    uint32_t clipped_ = 0;

    // Spectrum analysis state: touched only from Write(), which always runs on the same audio
    // task, so none of this (other than the published spectrum_ below) needs its own lock.
    std::atomic<bool> spectrum_enabled_{false};
    int16_t fft_accum_[kFftSize];
    int fft_accum_count_ = 0;
    float fft_buffer_[kFftSize * 2];  // interleaved re/im for dsps_fft2r_fc32
    float hann_window_[kFftSize];
    struct BandRange {
        int lo;
        int hi;
    };
    BandRange band_ranges_[walle_spectrum::kBands];
    // Windows left before a band's peak-hold dot starts falling again (classic Winamp behavior:
    // it sits at the peak for a beat before dropping, rather than falling continuously).
    uint8_t peak_hold_windows_[walle_spectrum::kBands] = {};

    mutable std::mutex spectrum_mutex_;
    walle_spectrum::Frame spectrum_;
};
