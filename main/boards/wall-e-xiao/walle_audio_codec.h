#pragma once

#include <atomic>
#include <mutex>

#include "codecs/no_audio_codec.h"
#include "walle_spectrum.h"

// PDM microphone + MAX98357A, with Wall-E extras: software mic gain applied before the
// wake word engine and the upload, a level meter, mic mute, and a volume cap
// (100 unless night mode lowers it).
class WalleAudioCodec : public NoAudioCodecSimplexPdm {
public:
    struct Level {
        float peak_dbfs;
        float rms_dbfs;
        int clipped;
        int samples;
    };
    // The microphone over the last full minute (post-gain, what the wake word engine and the server hear), for /api/status.
    // Kept apart from TakeLevel(), which the console and the diagnostics reset whenever they read it.
    struct MicMinute {
        float peak_dbfs;
        float rms_dbfs;
        uint32_t clipped;         // samples that hit full scale in that minute
        uint64_t clipped_total;   // since boot
    };

    WalleAudioCodec(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk,
                    gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_clk,
                    gpio_num_t mic_data);

    int Read(int16_t* dest, int samples) override;
    void SetOutputVolume(int volume) override;
    void Start() override;

    void SetMicGainDb(int db);
    // High-pass filter in front of the gain (2nd order Butterworth, 0 = off): removes the PDM microphone's DC offset and the low rumble of the wheel motors and the table
    // that the wake word and the speech recognizer cannot use. Speech starts above about 100 Hz.
    void SetMicHighPassHz(int hz);
    int mic_hpf_hz() const { return hpf_hz_.load(); }
    int mic_gain_db() const { return gain_db_.load(); }
    // Mic mute: the microphone reads silence (the wake word and the server hear nothing). Lasts until unmuted; a restart or power-off unmutes
    // (nothing is saved). The speaker is NOT muted, so timers, alarms and music still sound.
    void SetMicMuted(bool muted) { muted_ = muted; }
    bool mic_muted() const { return muted_.load(); }

    // Effective volume = min(requested volume, cap). The requested volume is remembered.
    void SetVolumeCap(int cap);
    int volume_cap() const { return volume_cap_; }
    int requested_volume() const { return requested_volume_; }

    // Level since the previous call (post-gain, i.e. what the wake word engine hears).
    Level TakeLevel();
    // The last full minute (or the minute so far right after boot); reading it resets nothing.
    MicMinute mic_minute();

    // RMS (0-32767) of the latest chunk, for the level bars under the face.
    int input_rms() const { return input_rms_.load(); }
    int output_rms() const { return output_rms_.load(); }

    // Winamp-style visualizer: a 32-band magnitude spectrum of what's actually being sent to the
    // speaker, refreshed roughly every 43 ms (1024 samples at 24 kHz) inside Write() itself - see
    // the .cc for why that is safe to do inline on the audio task. Skipped entirely (zero extra
    // CPU) unless enabled, since Write() runs continuously whenever anything plays.
    void SetSpectrumEnabled(bool enabled) { spectrum_enabled_ = enabled; }
    // Analyse every n-th window only (n = 1: all, about 23 a second; music uses 2, the bars there move 12.5 times a second anyway). The peak dots hold and fall
    // in the same real time at any n.
    void SetSpectrumStride(int n) { spectrum_stride_ = n < 1 ? 1 : (n > 4 ? 4 : n); }
    walle_spectrum::Frame TakeSpectrum() const;
    // Audit: windows analysed since boot and the time they took (microseconds), for /api/status.
    uint64_t spectrum_us() const { return spectrum_us_.load(); }
    uint32_t spectrum_windows() const { return spectrum_n_.load(); }

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
    std::atomic<int> hpf_hz_{0};
    // biquad coefficients (normalised) and state; the coefficients are written by SetMicHighPassHz, the state only by Read()
    std::atomic<float> hpf_b0_{1.0f}, hpf_b1_{0.0f}, hpf_b2_{0.0f}, hpf_a1_{0.0f}, hpf_a2_{0.0f};
    std::atomic<bool> hpf_reset_{false};
    float hpf_x1_ = 0, hpf_x2_ = 0, hpf_y1_ = 0, hpf_y2_ = 0;
    std::atomic<bool> muted_{false};
    int volume_cap_ = 100;
    int requested_volume_ = 70;

    std::mutex level_mutex_;
    int32_t peak_ = 0;
    uint64_t sum_squares_ = 0;
    uint32_t count_ = 0;
    uint32_t clipped_ = 0;
    // one-minute window for mic_minute(), under level_mutex_ too
    int32_t win_peak_ = 0;
    uint64_t win_sum_squares_ = 0;
    uint32_t win_count_ = 0;
    uint32_t win_clipped_ = 0;
    int64_t win_start_us_ = 0;
    int64_t first_read_us_ = 0;   // the microphone clicks when it starts: the first seconds are left out of the minute figures
    bool have_minute_ = false;
    MicMinute last_minute_ = {-96.0f, -96.0f, 0, 0};
    uint64_t clipped_total_ = 0;

    // Spectrum analysis state: touched only from Write(), which always runs on the same audio
    // task, so none of this (other than the published spectrum_ below) needs its own lock.
    std::atomic<bool> spectrum_enabled_{false};
    std::atomic<int> spectrum_stride_{1};
    int spectrum_skip_ = 0;
    std::atomic<uint64_t> spectrum_us_{0};
    std::atomic<uint32_t> spectrum_n_{0};
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
