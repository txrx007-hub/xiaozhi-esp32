#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include <esp_timer.h>

#include "lcd_display.h"
#include "walle_spectrum.h"

// Upstream's SPI LCD face display plus Wall-E extras. Everything visual that is not a
// plain emotion lives here: held emotions, the idle clock, the status dot, the blank
// nap screen, the orientation test pattern and the diagnostics color flash.
class WalleDisplay : public SpiLcdDisplay {
public:
    enum class Dot { kAmber, kGreen, kRed };

    WalleDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                 int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                 bool swap_xy);

    // Upstream hooks
    void SetupUI() override;
    void SetEmotion(const char* emotion) override;  // automatic emotions (wait during a hold)
    // Jarvis's words only (the user's transcript is dropped). Each sentence clears itself a
    // few seconds after being shown, instead of sitting there scrolling indefinitely until the
    // next one arrives.
    void SetChatMessage(const char* role, const char* content) override;
    // The closed eyes already say "listening" - the top status text doesn't need to repeat it.
    // Every other status text (connecting, speaking, standby, ...) still shows as normal.
    void SetStatus(const char* status) override;
    // A single take_photo/diagnostics capture shows the real picture, fit into the bottom half
    // of the screen, for 2 s (upstream default: full-ish size, centered, 5 s). look_around's
    // per-frame shots stay suppressed via SuppressNextPreview(), unaffected by this.
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

    // Shows an emotion for hold_ms, then returns to the latest automatic emotion.
    void HoldEmotion(const std::string& emotion, int hold_ms);
    const std::string& auto_emotion() const { return auto_emotion_; }

    // The next camera preview is dropped (the surprised face is the photo cue).
    void SuppressNextPreview() { suppress_preview_ = true; }

    void SetBlank(bool blank);  // nap: panel off, backlight stays on (wired to 3V3)
    bool IsBlank() const { return blank_; }

    void SetStatusDot(Dot dot);

    // Idle clock: big HH:MM, date, optional weather line and the wake word hint.
    void ShowIdleClock(bool show);
    bool IsIdleClockShown() const { return clock_shown_; }
    void RefreshIdleClock();  // call about once per second while shown
    void SetWeatherLine(const std::string& line);

    void ShowTestPattern(int duration_ms);
    void FlashColors();  // red, green, blue, white, 400 ms each

    // Level bars between the face and the subtitle: Wall-E's voice while speaking,
    // the microphone while listening, hidden otherwise. While speaking, visualizer_mode
    // (see below) can replace the plain bars with the 32-band spectrum instead.
    enum class Ribbon { kOff, kOutput, kInput };
    void SetLevelSources(std::function<int()> output_rms, std::function<int()> input_rms);
    void SetRibbon(Ribbon mode) { ribbon_mode_ = mode; }

    // Winamp-style spectrum in place of the ribbon while speaking (self.display.set_visualizer /
    // setting visualizer_mode). kOff leaves the plain RMS ribbon exactly as it was.
    void SetSpectrumSource(std::function<walle_spectrum::Frame()> source) {
        spectrum_source_ = std::move(source);
    }
    void SetVisualizerMode(walle_spectrum::Mode mode) { visualizer_mode_ = mode; }

    static constexpr const char* kListeningEmotion = "relaxed";

private:
    static constexpr int kRibbonBars = 20;
    void CreateRibbon();
    void UpdateRibbon();  // LVGL timer, ~25 fps; also drives the spectrum bars when active
    void UpdateSpectrumBars();
    void CreateClock();
    void SetDigit(int index, int value);
    void RemoveOverlayLater(lv_obj_t* obj, int delay_ms);
    // "surprised" gets a fresh random tint each time it's shown (the GIF's real alpha
    // transparency means recoloring only tints the eye shape, not the black background);
    // every other emotion clears the tint so its own baked color shows unmodified.
    void ApplyEmotionRecolor(const std::string& emotion);

    std::mutex emotion_mutex_;
    std::string auto_emotion_ = "neutral";
    int64_t hold_until_us_ = 0;
    esp_timer_handle_t hold_timer_ = nullptr;
    // WALL-E: clears the subtitle a few seconds after each sentence, rather than leaving it
    // scrolling on screen until the next one supersedes it (or forever, after the last one).
    int64_t subtitle_generation_ = 0;
    esp_timer_handle_t subtitle_clear_timer_ = nullptr;
    std::atomic<bool> suppress_preview_{false};
    bool blank_ = false;

    lv_obj_t* status_dot_ = nullptr;
    lv_obj_t* ribbon_ = nullptr;
    lv_obj_t* ribbon_bars_[kRibbonBars] = {};
    int ribbon_history_[kRibbonBars] = {};
    std::atomic<Ribbon> ribbon_mode_{Ribbon::kOff};
    bool ribbon_visible_ = false;
    std::function<int()> output_rms_;
    std::function<int()> input_rms_;

    std::atomic<walle_spectrum::Mode> visualizer_mode_{walle_spectrum::Mode::kOff};
    std::function<walle_spectrum::Frame()> spectrum_source_;
    lv_obj_t* spectrum_root_ = nullptr;
    lv_obj_t* spectrum_bars_[walle_spectrum::kBands] = {};
    lv_obj_t* spectrum_peaks_[walle_spectrum::kBands] = {};
    bool spectrum_visible_ = false;

    lv_obj_t* clock_root_ = nullptr;
    lv_obj_t* clock_date_ = nullptr;
    lv_obj_t* clock_weather_ = nullptr;
    lv_obj_t* clock_segments_[4][7] = {};
    lv_obj_t* clock_colon_[2] = {};
    bool clock_shown_ = false;
    int clock_last_minute_ = -1;
    std::string weather_line_;
};
