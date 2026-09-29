#include "walle_display.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <esp_log.h>
#include <esp_lcd_panel_ops.h>
#include <esp_random.h>

#include "application.h"
#include "config.h"
#include "lvgl_theme.h"

#define TAG "WalleDisplay"

namespace {

// Idle clock palette (port of the polunzh idle-clock idea to 240x240)
constexpr uint32_t kClockBg = 0x061418;
constexpr uint32_t kClockOn = 0x2ee6d6;
constexpr uint32_t kClockGhost = 0x0d2a2c;
constexpr uint32_t kClockText = 0x7ee7df;

// 7-segment layout: digit box and segment thickness
constexpr int kDigitW = 44;
constexpr int kDigitH = 84;
constexpr int kSegT = 9;
constexpr int kDigitsTop = 70;
// x of the four digits: [d0] 8 [d1] 24 (colon) [d2] 8 [d3], centred on 240 px
constexpr int kDigitX[4] = {12, 64, 132, 184};

// Segments a..g as bits 0..6
constexpr uint8_t kDigitSegments[10] = {
    0x3F,  // 0: a b c d e f
    0x06,  // 1: b c
    0x5B,  // 2: a b d e g
    0x4F,  // 3: a b c d g
    0x66,  // 4: b c f g
    0x6D,  // 5: a c d f g
    0x7D,  // 6: a c d e f g
    0x07,  // 7: a b c
    0x7F,  // 8: all
    0x6F,  // 9: a b c d f g
};

lv_obj_t* MakeBox(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* text) {
    lv_obj_t* label = lv_label_create(parent);
    // A null font (e.g. the theme's font asset hadn't finished loading yet when this label was
    // created) must never reach LVGL: on a style-stripped parent like lv_layer_top()'s boxes,
    // there's no cascaded font to fall back on, and a later full-layout relayout (e.g. entering
    // power save) crashes jumping through the null font's glyph-dispatch table (PC=0 panic).
    lv_obj_set_style_text_font(label, font != nullptr ? font : LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

void DeleteObjTimer(lv_timer_t* timer) {
    auto* obj = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
    if (obj != nullptr) {
        lv_obj_delete(obj);
    }
    lv_timer_delete(timer);
}

}  // namespace

WalleDisplay::WalleDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x,
                           bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {
    esp_timer_create_args_t args = {};
    args.callback = [](void* arg) {
        auto* self = static_cast<WalleDisplay*>(arg);
        // Emotion changes touch LVGL images; run them in the main task, not the timer task.
        Application::GetInstance().Schedule([self]() {
            std::string emotion;
            {
                std::lock_guard<std::mutex> lock(self->emotion_mutex_);
                if (esp_timer_get_time() < self->hold_until_us_) {
                    return;  // a newer hold is still running
                }
                self->hold_until_us_ = 0;
                emotion = self->auto_emotion_;
            }
            self->ApplyEmotionRecolor(emotion);
            self->SpiLcdDisplay::SetEmotion(emotion.c_str());
        });
    };
    args.arg = this;
    args.name = "emotion_hold";
    esp_timer_create(&args, &hold_timer_);

    esp_timer_create_args_t subtitle_args = {};
    subtitle_args.callback = [](void* arg) {
        auto* self = static_cast<WalleDisplay*>(arg);
        const int64_t generation = self->subtitle_generation_;
        // SetChatMessage touches LVGL; run the actual clear on the main task.
        Application::GetInstance().Schedule([self, generation]() {
            if (generation == self->subtitle_generation_) {
                self->SpiLcdDisplay::SetChatMessage("assistant", "");
            }
        });
    };
    subtitle_args.arg = this;
    subtitle_args.name = "subtitle_clear";
    esp_timer_create(&subtitle_args, &subtitle_clear_timer_);
}

void WalleDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();
    // The eye GIFs (recolored Otto set) are drawn on black, so start in the dark theme,
    // as upstream's Otto board does.
    if (auto* dark = LvglThemeManager::GetInstance().GetTheme("dark"); dark != nullptr) {
        SetTheme(dark);
    }
    DisplayLockGuard lock(this);
    // Order matters on the top layer: bars under the clock, status dot above everything.
    CreateRibbon();
    CreateClock();
    status_dot_ = MakeBox(lv_layer_top(), width_ - 16, 6, 10, 10, 0xf59e0b, LV_RADIUS_CIRCLE);
    lv_timer_create(
        [](lv_timer_t* t) { static_cast<WalleDisplay*>(lv_timer_get_user_data(t))->UpdateRibbon(); },
        40, this);
}

void WalleDisplay::SetLevelSources(std::function<int()> output_rms,
                                   std::function<int()> input_rms) {
    output_rms_ = std::move(output_rms);
    input_rms_ = std::move(input_rms);
}

void WalleDisplay::CreateRibbon() {
    constexpr int kBarPitch = 9;  // 6 px bar + 3 px gap
    constexpr int kRibbonHeight = 26;
    const int total_width = kRibbonBars * kBarPitch - 3;
    ribbon_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ribbon_);
    // Between the face and the subtitle bar (which takes the bottom ~34 px).
    lv_obj_set_pos(ribbon_, (width_ - total_width) / 2, height_ - 57 - kRibbonHeight / 2);
    lv_obj_set_size(ribbon_, total_width, kRibbonHeight);
    lv_obj_remove_flag(ribbon_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(ribbon_, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < kRibbonBars; ++i) {
        ribbon_bars_[i] = MakeBox(ribbon_, i * kBarPitch, kRibbonHeight / 2 - 2, 6, 4, 0x2ee6d6, 2);
    }
    lv_obj_add_flag(ribbon_, LV_OBJ_FLAG_HIDDEN);

    // Same strip, same vertical position: 32 mirrored bars + peak-hold dots, shown instead of
    // the plain ribbon while speaking when visualizer_mode is winamp (see UpdateRibbon).
    constexpr int kSpectrumBarWidth = 4;
    constexpr int kSpectrumPitch = 6;  // 4 px bar + 2 px gap
    constexpr int kSpectrumWidth = walle_spectrum::kBands * kSpectrumPitch - 2;
    constexpr int kPeakHeight = 2;
    spectrum_root_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(spectrum_root_);
    lv_obj_set_pos(spectrum_root_, (width_ - kSpectrumWidth) / 2, height_ - 57 - kRibbonHeight / 2);
    lv_obj_set_size(spectrum_root_, kSpectrumWidth, kRibbonHeight);
    lv_obj_remove_flag(spectrum_root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(spectrum_root_, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < walle_spectrum::kBands; ++i) {
        spectrum_bars_[i] = MakeBox(spectrum_root_, i * kSpectrumPitch, kRibbonHeight / 2 - 1,
                                    kSpectrumBarWidth, 2, 0x2ee6d6, 1);
        spectrum_peaks_[i] =
            MakeBox(spectrum_root_, i * kSpectrumPitch, 0, kSpectrumBarWidth, kPeakHeight,
                   0xffffff, 1);
    }
    lv_obj_add_flag(spectrum_root_, LV_OBJ_FLAG_HIDDEN);
}

void WalleDisplay::UpdateSpectrumBars(lv_color_t color) {
    constexpr int kRibbonHeight = 26;
    if (!spectrum_source_) {
        return;
    }
    const walle_spectrum::Frame frame = spectrum_source_();
    for (int i = 0; i < walle_spectrum::kBands; ++i) {
        const int half = frame.bands[i] * (kRibbonHeight / 2) / 255;
        const int h = std::max(2, half * 2);
        lv_obj_set_height(spectrum_bars_[i], h);
        lv_obj_set_y(spectrum_bars_[i], (kRibbonHeight - h) / 2);
        lv_obj_set_style_bg_color(spectrum_bars_[i], color, 0);
        const int peak_half = frame.peaks[i] * (kRibbonHeight / 2) / 255;
        lv_obj_set_y(spectrum_peaks_[i], std::max(0, kRibbonHeight / 2 - peak_half - 1));
    }
}

void WalleDisplay::UpdateRibbon() {
    constexpr int kRibbonHeight = 26;
    const Ribbon mode = ribbon_mode_.load();
    if (ribbon_ == nullptr || spectrum_root_ == nullptr) {
        return;
    }
    if (mode == Ribbon::kOff || blank_ || clock_shown_) {
        if (ribbon_visible_) {
            lv_obj_add_flag(ribbon_, LV_OBJ_FLAG_HIDDEN);
            ribbon_visible_ = false;
            for (auto& h : ribbon_history_) {
                h = 2;
            }
        }
        if (spectrum_visible_) {
            lv_obj_add_flag(spectrum_root_, LV_OBJ_FLAG_HIDDEN);
            spectrum_visible_ = false;
        }
        return;
    }

    // Teal on the light theme, cyan on the dark one; shared by both the plain ribbon and the
    // spectrum bars so switching visualizer_mode never changes the strip's color.
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    const bool dark_background =
        theme != nullptr && lv_color_brightness(theme->background_color()) < 128;
    const lv_color_t color = lv_color_hex(dark_background ? 0x2ee6d6 : 0x1d9e75);

    const bool winamp =
        mode == Ribbon::kOutput && visualizer_mode_.load() == walle_spectrum::Mode::kWinamp;
    if (winamp) {
        if (ribbon_visible_) {
            lv_obj_add_flag(ribbon_, LV_OBJ_FLAG_HIDDEN);
            ribbon_visible_ = false;
        }
        UpdateSpectrumBars(color);
        if (!spectrum_visible_) {
            lv_obj_remove_flag(spectrum_root_, LV_OBJ_FLAG_HIDDEN);
            spectrum_visible_ = true;
        }
        return;
    }
    if (spectrum_visible_) {
        lv_obj_add_flag(spectrum_root_, LV_OBJ_FLAG_HIDDEN);
        spectrum_visible_ = false;
    }

    int rms = 0;
    if (mode == Ribbon::kOutput && output_rms_) {
        rms = output_rms_();
    } else if (mode == Ribbon::kInput && input_rms_) {
        rms = input_rms_();
    }
    const float db = rms > 0 ? 20.0f * log10f(rms / 32768.0f) : -90.0f;
    int height = static_cast<int>((db + 55.0f) * kRibbonHeight / 45.0f);
    height = height < 2 ? 2 : (height > kRibbonHeight ? kRibbonHeight : height);

    // Scroll left: the newest level enters on the right.
    for (int i = 0; i < kRibbonBars - 1; ++i) {
        ribbon_history_[i] = ribbon_history_[i + 1];
    }
    ribbon_history_[kRibbonBars - 1] = height;

    for (int i = 0; i < kRibbonBars; ++i) {
        const int h = ribbon_history_[i] < 2 ? 2 : ribbon_history_[i];
        lv_obj_set_height(ribbon_bars_[i], h);
        lv_obj_set_y(ribbon_bars_[i], (kRibbonHeight - h) / 2);
        lv_obj_set_style_bg_color(ribbon_bars_[i], color, 0);
    }
    if (!ribbon_visible_) {
        lv_obj_remove_flag(ribbon_, LV_OBJ_FLAG_HIDDEN);
        ribbon_visible_ = true;
    }
}

void WalleDisplay::ApplyEmotionRecolor(const std::string& emotion) {
    if (emoji_image_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    if (emotion == "surprised") {
        static constexpr uint32_t kSurprisedColors[] = {
            0xff3b30,  // red
            0xff9500,  // orange
            0xffcc00,  // yellow
            0x34c759,  // green
            0x00c7be,  // teal
            0x007aff,  // blue
            0xaf52de,  // purple
            0xff2d92,  // pink
            0xffffff,  // white
        };
        constexpr size_t kCount = sizeof(kSurprisedColors) / sizeof(kSurprisedColors[0]);
        const uint32_t color = kSurprisedColors[esp_random() % kCount];
        lv_obj_set_style_image_recolor(emoji_image_, lv_color_hex(color), 0);
        lv_obj_set_style_image_recolor_opa(emoji_image_, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_image_recolor_opa(emoji_image_, LV_OPA_TRANSP, 0);
    }
}

void WalleDisplay::SetEmotion(const char* emotion) {
    {
        std::lock_guard<std::mutex> lock(emotion_mutex_);
        auto_emotion_ = emotion != nullptr ? emotion : "neutral";
        if (esp_timer_get_time() < hold_until_us_) {
            return;
        }
    }
    ApplyEmotionRecolor(auto_emotion_);
    SpiLcdDisplay::SetEmotion(emotion);
}

void WalleDisplay::HoldEmotion(const std::string& emotion, int hold_ms) {
    {
        std::lock_guard<std::mutex> lock(emotion_mutex_);
        hold_until_us_ = esp_timer_get_time() + static_cast<int64_t>(hold_ms) * 1000;
    }
    ApplyEmotionRecolor(emotion);
    SpiLcdDisplay::SetEmotion(emotion.c_str());
    esp_timer_stop(hold_timer_);
    esp_timer_start_once(hold_timer_, static_cast<uint64_t>(hold_ms) * 1000);
}

void WalleDisplay::SetChatMessage(const char* role, const char* content) {
    // Only Jarvis's words show under the face; the user's transcript is not shown.
    if (role != nullptr && strcmp(role, "user") == 0) {
        SpiLcdDisplay::SetChatMessage(role, "");
        return;
    }
    SpiLcdDisplay::SetChatMessage(role, content);

    // WALL-E: each sentence clears itself a little after it would have been read, instead of
    // sitting there (scrolling, if it doesn't fit) until the next one arrives or forever after
    // the last one. Scoped to "assistant" only; system status text manages itself elsewhere.
    ++subtitle_generation_;
    esp_timer_stop(subtitle_clear_timer_);
    const size_t length = (role != nullptr && strcmp(role, "assistant") == 0 && content != nullptr)
                             ? strlen(content)
                             : 0;
    if (length > 0) {
        const int64_t duration_ms = std::clamp<int64_t>(70LL * static_cast<int64_t>(length), 2500, 9000);
        esp_timer_start_once(subtitle_clear_timer_, duration_ms * 1000);
    }
}

void WalleDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    // WALL-E: hiding the preview (nullptr) is plain cleanup; reuse upstream's version of that
    // regardless of source (look_around's own suppression already returns before this point).
    if (image == nullptr) {
        SpiLcdDisplay::SetPreviewImage(nullptr);
        return;
    }
    if (suppress_preview_.exchange(false)) {
        return;  // one of look_around's 4 per-frame shots: no flash, only the final description
    }
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        return;
    }
    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // Fit within the bottom half (full width, half height) preserving aspect ratio: scale
        // by whichever axis is tighter, same technique upstream uses, just a smaller target box.
        const int box_w = width_;
        const int box_h = height_ / 2;
        const int zoom_w = 256 * box_w / img_dsc->header.w;
        const int zoom_h = 256 * box_h / img_dsc->header.h;
        lv_image_set_scale(preview_image_, std::min(zoom_w, zoom_h));
    }
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(preview_image_, LV_ALIGN_BOTTOM_MID, 0, 0);
    esp_timer_stop(preview_timer_);
    esp_timer_start_once(preview_timer_, 2000 * 1000);  // 2 s, not upstream's 5 s
}

void WalleDisplay::SetBlank(bool blank) {
    if (blank == blank_) {
        return;
    }
    DisplayLockGuard lock(this);
    if (blank) {
        esp_lcd_panel_disp_on_off(panel_, false);
        esp_lcd_panel_disp_sleep(panel_, true);
    } else {
        esp_lcd_panel_disp_sleep(panel_, false);
        vTaskDelay(pdMS_TO_TICKS(10));
        esp_lcd_panel_disp_on_off(panel_, true);
        lv_obj_invalidate(lv_screen_active());
        lv_obj_invalidate(lv_layer_top());
    }
    blank_ = blank;
    ESP_LOGI(TAG, "Screen %s", blank ? "blank (nap)" : "on");
}

void WalleDisplay::SetStatusDot(Dot dot) {
    static constexpr uint32_t kColors[] = {0xf59e0b, 0x22c55e, 0xef4444};
    DisplayLockGuard lock(this);
    if (status_dot_ != nullptr) {
        lv_obj_set_style_bg_color(status_dot_, lv_color_hex(kColors[static_cast<int>(dot)]), 0);
    }
}

void WalleDisplay::CreateClock() {
    // Deliberately NOT theme->text_font(): that raw lv_font_t* can be swapped out from under us
    // later (LvglDisplay::SetTextFont() rebinds every style IT tracks when the font changes, but
    // these clock labels live outside that system) - the resulting dangling pointer, next
    // dereferenced by a full relayout (e.g. entering power save), is what caused a real
    // "PC: 0x00000000" panic in lv_font_get_glyph_width. LV_FONT_DEFAULT is a static compiled-in
    // font that's never swapped or freed, so it can't go stale.
    const lv_font_t* text_font = LV_FONT_DEFAULT;

    clock_root_ = MakeBox(lv_layer_top(), 0, 0, width_, height_, kClockBg, 0);

    clock_date_ = MakeLabel(clock_root_, text_font, kClockText, "");
    lv_obj_align(clock_date_, LV_ALIGN_TOP_MID, 0, 28);

    for (int d = 0; d < 4; ++d) {
        const int x = kDigitX[d];
        const int y = kDigitsTop;
        const int half = kDigitH / 2;
        const int vertical = half - kSegT - kSegT / 2;
        struct {
            int x, y, w, h;
        } seg[7] = {
            {x + kSegT, y, kDigitW - 2 * kSegT, kSegT},                        // a
            {x + kDigitW - kSegT, y + kSegT, kSegT, vertical},                 // b
            {x + kDigitW - kSegT, y + half + kSegT / 2, kSegT, vertical},      // c
            {x + kSegT, y + kDigitH - kSegT, kDigitW - 2 * kSegT, kSegT},      // d
            {x, y + half + kSegT / 2, kSegT, vertical},                        // e
            {x, y + kSegT, kSegT, vertical},                                   // f
            {x + kSegT, y + half - kSegT / 2, kDigitW - 2 * kSegT, kSegT},     // g
        };
        for (int s = 0; s < 7; ++s) {
            clock_segments_[d][s] =
                MakeBox(clock_root_, seg[s].x, seg[s].y, seg[s].w, seg[s].h, kClockGhost, 3);
        }
    }
    const int colon_x = kDigitX[1] + kDigitW + (kDigitX[2] - kDigitX[1] - kDigitW - kSegT) / 2;
    clock_colon_[0] = MakeBox(clock_root_, colon_x, kDigitsTop + 22, kSegT, kSegT, kClockOn, 2);
    clock_colon_[1] =
        MakeBox(clock_root_, colon_x, kDigitsTop + kDigitH - 22 - kSegT, kSegT, kSegT, kClockOn, 2);

    clock_weather_ = MakeLabel(clock_root_, text_font, kClockText, "");
    lv_obj_align(clock_weather_, LV_ALIGN_TOP_MID, 0, 170);
    lv_obj_add_flag(clock_weather_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* hint = MakeLabel(clock_root_, text_font, 0x5fd3c9, "Say \"" WAKE_WORD_NAME "\"");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    lv_obj_add_flag(clock_root_, LV_OBJ_FLAG_HIDDEN);
}

void WalleDisplay::SetDigit(int index, int value) {
    const uint8_t bits = (value >= 0 && value <= 9) ? kDigitSegments[value] : 0x40;  // "-"
    for (int s = 0; s < 7; ++s) {
        lv_obj_set_style_bg_color(clock_segments_[index][s],
                                  lv_color_hex((bits >> s) & 1 ? kClockOn : kClockGhost), 0);
    }
}

void WalleDisplay::ShowIdleClock(bool show) {
    if (clock_root_ == nullptr || show == clock_shown_) {
        return;
    }
    {
        DisplayLockGuard lock(this);
        if (show) {
            lv_obj_remove_flag(clock_root_, LV_OBJ_FLAG_HIDDEN);
            if (status_dot_ != nullptr) {
                lv_obj_move_foreground(status_dot_);
            }
        } else {
            lv_obj_add_flag(clock_root_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    clock_shown_ = show;
    clock_last_minute_ = -1;
    if (show) {
        RefreshIdleClock();
    }
}

void WalleDisplay::RefreshIdleClock() {
    if (!clock_shown_) {
        return;
    }
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    const bool synced = local.tm_year >= 2025 - 1900;
    const int minute_of_day = synced ? local.tm_hour * 60 + local.tm_min : -2;
    if (minute_of_day == clock_last_minute_) {
        return;
    }
    clock_last_minute_ = minute_of_day;

    DisplayLockGuard lock(this);
    if (synced) {
        SetDigit(0, local.tm_hour / 10);
        SetDigit(1, local.tm_hour % 10);
        SetDigit(2, local.tm_min / 10);
        SetDigit(3, local.tm_min % 10);
        char date[32];
        strftime(date, sizeof(date), "%a %d %b", &local);
        lv_label_set_text(clock_date_, date);
    } else {
        for (int d = 0; d < 4; ++d) {
            SetDigit(d, -1);
        }
        lv_label_set_text(clock_date_, "waiting for time");
    }
    if (weather_line_.empty()) {
        lv_obj_add_flag(clock_weather_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(clock_weather_, weather_line_.c_str());
        lv_obj_remove_flag(clock_weather_, LV_OBJ_FLAG_HIDDEN);
    }
}

void WalleDisplay::SetWeatherLine(const std::string& line) {
    weather_line_ = line;
    clock_last_minute_ = -1;  // redraw on the next refresh
    RefreshIdleClock();
}

void WalleDisplay::RemoveOverlayLater(lv_obj_t* obj, int delay_ms) {
    lv_timer_t* timer = lv_timer_create(DeleteObjTimer, delay_ms, obj);
    lv_timer_set_repeat_count(timer, -1);  // deleted by the callback itself
}

void WalleDisplay::ShowTestPattern(int duration_ms) {
    DisplayLockGuard lock(this);
    // See CreateClock(): theme->text_font() can go stale later and must not be cached in a
    // long-lived (or even short-lived, on lv_layer_top()) label outside the theme's own tracking.
    const lv_font_t* font = LV_FONT_DEFAULT;

    lv_obj_t* root = MakeBox(lv_layer_top(), 0, 0, width_, height_, 0x000000, 0);
    lv_obj_set_style_border_color(root, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_width(root, 1, 0);

    struct Corner {
        lv_align_t align;
        uint32_t color;
        const char* text;
        uint32_t text_color;
    } corners[] = {
        {LV_ALIGN_TOP_LEFT, 0xff0000, "RED", 0xffffff},
        {LV_ALIGN_TOP_RIGHT, 0x00ff00, "GREEN", 0x000000},
        {LV_ALIGN_BOTTOM_LEFT, 0x0000ff, "BLUE", 0xffffff},
        {LV_ALIGN_BOTTOM_RIGHT, 0xffffff, "WHITE", 0x000000},
    };
    for (const auto& corner : corners) {
        lv_obj_t* box = MakeBox(root, 0, 0, 72, 44, corner.color, 0);
        lv_obj_align(box, corner.align, 0, 0);
        lv_obj_t* label = MakeLabel(box, font, corner.text_color, corner.text);
        lv_obj_center(label);
    }
    lv_obj_t* up = MakeLabel(root, font, 0xffffff, "^ UP ^");
    lv_obj_align(up, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_t* info = MakeLabel(root, font, 0xffffff, "TOP-LEFT = RED");
    lv_obj_align(info, LV_ALIGN_CENTER, 0, 0);

    RemoveOverlayLater(root, duration_ms);
}

void WalleDisplay::FlashColors() {
    DisplayLockGuard lock(this);
    lv_obj_t* root = MakeBox(lv_layer_top(), 0, 0, width_, height_, 0xff0000, 0);
    lv_obj_set_user_data(root, reinterpret_cast<void*>(0));
    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            static constexpr uint32_t kSequence[] = {0x00ff00, 0x0000ff, 0xffffff};
            auto* obj = static_cast<lv_obj_t*>(lv_timer_get_user_data(t));
            auto step = reinterpret_cast<intptr_t>(lv_obj_get_user_data(obj));
            if (step >= 3) {
                lv_obj_delete(obj);
                lv_timer_delete(t);
                return;
            }
            lv_obj_set_style_bg_color(obj, lv_color_hex(kSequence[step]), 0);
            lv_obj_set_user_data(obj, reinterpret_cast<void*>(step + 1));
        },
        400, root);
    (void)timer;
}
