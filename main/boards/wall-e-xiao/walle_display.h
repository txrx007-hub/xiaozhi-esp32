#pragma once

#include <atomic>
#include <functional>
#include <memory>
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
    // A single take_photo/diagnostics capture shows the real picture, fit into the screen and
    // centred, for 3.5 s (upstream default: 5 s). look_around's
    // per-frame shots stay suppressed via SuppressNextPreview(), unaffected by this.
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

    // Shows an emotion for hold_ms, then returns to the latest automatic emotion.
    void HoldEmotion(const std::string& emotion, int hold_ms);
    const std::string& auto_emotion() const { return auto_emotion_; }

    // The next camera preview is dropped (the surprised face is the photo cue).
    void SuppressNextPreview(bool on = true) { suppress_preview_ = on; }

    // Screen redraws per second (counts LVGL render cycles that actually drew something), averaged
    // over the time since the previous call. Static screens read 0.
    int Fps();

    void SetBlank(bool blank);  // nap: panel off, backlight stays on (wired to 3V3)
    bool IsBlank() const { return blank_; }

    void SetStatusDot(Dot dot);
    // Small red crossed-out microphone at the top left, slowly blinking, while the microphone is muted; on the eyes and on the idle clock,
    // level with the status dot (on the eyes just right of the Wi-Fi icon).
    void SetMuteIcon(bool on);

    // Idle clock: big HH:MM, date, optional weather line and the wake word hint.
    void ShowIdleClock(bool show);
    bool IsIdleClockShown() const { return clock_shown_; }
    void RefreshIdleClock();  // call about once per second while shown
    void SetWeatherLine(const std::string& line);

    // QR page: a full-screen white page with a QR code, a title and the address under it, drawn over
    // everything (like the idle clock). Hidden by HideQr(), and automatically by ShowIdleClock(false),
    // which every wake-up goes through.
    struct QrPage {
        std::string title;
        std::string url;
    };
    void ShowQr(const QrPage& page, int index, int count);
    void HideQr();
    bool IsQrShown() const { return qr_shown_; }

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
    // While music or radio plays (the server marks its "tts start"), the classic Winamp bars show whatever visualizer_mode is: the setting only picks
    // the look for Jarvis's own voice.
    // The same mark also swaps the eyes for a headphones picture (setting music_icon: 0 = music icon 1, 1 = music icon 2) while the speaking audio is music.
    void SetMusicVisual(bool on) override {
        music_visual_ = on;
        if (music_hook_) {
            music_hook_(on);
        }
    }
    void SetMusicIcon(int icon) { music_icon_ = icon; }
    // While the music screen shows, the status line at the top ("Speaking") stays blank.
    void SetStatus(const char* status) override;
    // Called when the music mark changes, so the board can switch the microphone's spectrum analysis on even if the speaking visualizer is off.
    void SetMusicHook(std::function<void(bool)> hook) { music_hook_ = std::move(hook); }

    static constexpr const char* kListeningEmotion = "relaxed";

private:
    static constexpr int kRibbonBars = 20;
    void CreateRibbon();
    void UpdateRibbon();  // LVGL timer, ~25 fps; also drives the spectrum bars when active
    void UpdateSpectrumBars();
    // The other speaking visualizers (scope, mouth, radial, vu, orb). kOff/kWinamp hide them all.
    void CreateVisuals();
    void UpdateMusicScreen(bool want);  // eyes out, headphones picture in (and back)
    bool EnsureMusicCanvas();
    void DrawMusicIcon(int icon);
    void UpdateVisual(walle_spectrum::Mode mode);
    bool EnsureCanvasBuffer();
    void DrawRadial();
    void DrawVu();
    void DrawOrb();
    void CreateClock();
    void SetDigit(int index, int value);
    void RemoveOverlayLater(lv_obj_t* obj, int delay_ms);
    // "surprised" gets a fresh random tint each time it's shown (the GIF's real alpha
    // transparency means recoloring only tints the eye shape, not the black background);
    // every other emotion clears the tint so its own baked color shows unmodified.
    void ApplyEmotionRecolor(const std::string& emotion);
    // Boot screen: boot.gif (made by tools/make_boot_gif.py, not in git, built into the app) plays full screen, on top of everything, from the end of
    // SetupUI - right after power-on - until Jarvis shows its first real face: at least kBootMinMs, at most kBootMaxMs, once per power-up.
    // Without a boot.gif in the build nothing changes.
    void ShowBootSplash();
    void HideBootSplash();
    static constexpr int kBootMinMs = 3800;    // one full pulse of the eye
    static constexpr int kBootMaxMs = 15000;   // never longer (no Wi-Fi: the setup screen must show)
    std::unique_ptr<LvglGif> boot_gif_;
    lv_obj_t* boot_img_ = nullptr;
    bool boot_done_ = false;
    int64_t boot_min_until_us_ = 0;
    esp_timer_handle_t boot_timer_ = nullptr;

    std::atomic<uint32_t> frame_count_{0};
    std::mutex fps_mutex_;
    int64_t fps_last_us_ = 0;
    uint32_t fps_last_count_ = 0;
    int fps_value_ = 0;

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
    lv_obj_t* mute_icon_ = nullptr;
    bool mute_on_ = false;
    void CreateMuteIcon();
    void PlaceMuteIcon();  // left edge on the clock, right of the Wi-Fi icon on the eyes; on top of the layer
    lv_obj_t* ribbon_ = nullptr;
    lv_obj_t* ribbon_bars_[kRibbonBars] = {};
    int ribbon_history_[kRibbonBars] = {};
    std::atomic<Ribbon> ribbon_mode_{Ribbon::kOff};
    bool ribbon_visible_ = false;
    std::function<int()> output_rms_;
    std::function<int()> input_rms_;

    std::atomic<walle_spectrum::Mode> visualizer_mode_{walle_spectrum::Mode::kOff};
    std::atomic<bool> music_visual_{false};
    std::atomic<int> music_icon_{0};
    std::function<void(bool)> music_hook_;
    lv_obj_t* music_bg_ = nullptr;
    lv_obj_t* music_canvas_ = nullptr;
    uint8_t* music_buf_ = nullptr;  // PSRAM, allocated the first time music plays
    int music_icon_drawn_ = -1;
    std::atomic<bool> music_screen_shown_{false};
    bool eyes_hidden_by_music_ = false;
    bool status_hidden_by_music_ = false;
    walle_spectrum::Mode EffectiveVisualMode() const {
        return music_visual_.load() ? walle_spectrum::Mode::kWinamp : visualizer_mode_.load();
    }
    std::function<walle_spectrum::Frame()> spectrum_source_;
    lv_obj_t* spectrum_root_ = nullptr;
    lv_obj_t* spectrum_bars_[walle_spectrum::kBands] = {};
    lv_obj_t* spectrum_peaks_[walle_spectrum::kBands] = {};
    bool spectrum_visible_ = false;
    int spectrum_height_ = 26;  // becomes 50 on first use (winamp and rainbow; grows upward, same baseline)

    lv_obj_t* scope_line_ = nullptr;
    lv_point_precise_t scope_points_[walle_spectrum::kWavePoints] = {};
    lv_obj_t* mouth_root_ = nullptr;
    lv_obj_t* mouth_teeth_[4] = {};
    lv_obj_t* viz_canvas_ = nullptr;  // radial / vu / orb, square, over the eyes
    uint8_t* viz_canvas_buf_ = nullptr;  // PSRAM, allocated the first time it's needed
    float viz_bands_[walle_spectrum::kBands] = {};
    float viz_level_ = 0.0f;  // 0..1, smoothed speaker level
    float vu_needle_ = 0.0f;

    lv_obj_t* clock_root_ = nullptr;
    lv_obj_t* clock_date_ = nullptr;
    lv_obj_t* clock_weather_ = nullptr;
    lv_obj_t* clock_segments_[4][7] = {};
    lv_obj_t* clock_colon_[2] = {};
    bool clock_shown_ = false;
    int clock_last_minute_ = -1;
    std::string weather_line_;

    void CreateQr();  // lazily, the first time a QR page is shown (the code canvas needs memory)
    bool DrawQr(const std::string& text);  // false if the text is too long for a version-10 code
    uint8_t* qr_buf_ = nullptr;            // PSRAM canvas buffer for the code
    lv_obj_t* qr_root_ = nullptr;
    lv_obj_t* qr_title_ = nullptr;
    lv_obj_t* qr_code_ = nullptr;
    lv_obj_t* qr_url_ = nullptr;
    std::atomic<bool> qr_shown_{false};
};
