#include "walle_display.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lcd_panel_ops.h>
#include <esp_random.h>

#include "application.h"
#include "assets/lang_config.h"
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

// Classic Winamp EQ coloring: green for most of the range, yellow near the top, red only right at
// the peak - not a smooth gradient the whole way, which reads as muddy on a strip this short.
lv_color_t WinampBandColor(uint8_t value) {
    if (value >= 224) {
        return lv_color_hex(0xff0000);
    }
    if (value >= 160) {
        return lv_color_hex(0xffff00);
    }
    return lv_color_hex(0x00ff00);
}

// Speaking visualizer geometry. The strip modes (winamp, scope, mouth) sit where the level bars
// are, between the eyes and the subtitle bar; the canvas modes cover the eyes with a square,
// leaving the status line on top and the subtitle bar at the bottom visible.
constexpr int kStripCenterY = 240 - 57;
constexpr int kScopeWidth = 192;
constexpr int kScopeHeight = 34;
constexpr int kMouthWidth = 110;
constexpr int kCanvasSize = 176;
constexpr int kCanvasTop = 28;
constexpr int kCanvasCenter = kCanvasSize / 2;
constexpr uint32_t kVizCyan = 0x20e0e0;
constexpr float kPi = 3.14159265f;

void SetShown(lv_obj_t* obj, bool shown) {
    if (obj == nullptr || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == !shown) {
        return;
    }
    if (shown) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void DrawLine(lv_layer_t* layer, lv_draw_line_dsc_t* dsc, float x1, float y1, float x2, float y2) {
    dsc->p1.x = static_cast<int32_t>(lroundf(x1));
    dsc->p1.y = static_cast<int32_t>(lroundf(y1));
    dsc->p2.x = static_cast<int32_t>(lroundf(x2));
    dsc->p2.y = static_cast<int32_t>(lroundf(y2));
    lv_draw_line(layer, dsc);
}

// Filled circle (an arc as wide as its radius) or a ring (thinner width).
void DrawCircle(lv_layer_t* layer, int cx, int cy, int radius, int width, uint32_t color,
                int start_deg = 0, int end_deg = 360) {
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center.x = cx;
    arc.center.y = cy;
    arc.radius = static_cast<uint16_t>(std::max(1, radius));
    arc.width = std::max(1, width);
    arc.start_angle = start_deg;
    arc.end_angle = end_deg;
    arc.color = lv_color_hex(color);
    lv_draw_arc(layer, &arc);
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
    CreateVisuals();
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

    // Same strip, same vertical position: 32 classic-Winamp-style bars (grow up from the bottom,
    // not mirrored around the center - and colored green/yellow/red by how loud each band is,
    // not a single flat color) + peak-hold dots, shown instead of the plain ribbon while speaking
    // when visualizer_mode is winamp (see UpdateRibbon).
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
        spectrum_bars_[i] = MakeBox(spectrum_root_, i * kSpectrumPitch, kRibbonHeight - 2,
                                    kSpectrumBarWidth, 2, 0x00ff00, 1);
        spectrum_peaks_[i] =
            MakeBox(spectrum_root_, i * kSpectrumPitch, kRibbonHeight - kPeakHeight,
                   kSpectrumBarWidth, kPeakHeight, 0xffffff, 1);
    }
    lv_obj_add_flag(spectrum_root_, LV_OBJ_FLAG_HIDDEN);
}

void WalleDisplay::UpdateSpectrumBars() {
    constexpr int kStripHeight = 50;   // winamp and rainbow (the plain ribbon below stays 26 px)
    constexpr int kRibbonBaseHeight = 26;
    constexpr int kPeakHeight = 2;
    if (!spectrum_source_) {
        return;
    }
    const bool rainbow = visualizer_mode_.load() == walle_spectrum::Mode::kRainbow;
    // The spectrum strips are 50 px tall (the plain level ribbon is 26). They keep the same
    // baseline - just above the subtitle bar - so the taller strip grows upward into the lower
    // part of the eyes.
    const int strip_h = kStripHeight;
    if (strip_h != spectrum_height_) {
        spectrum_height_ = strip_h;
        const int baseline = height_ - 57 + kRibbonBaseHeight / 2;
        lv_obj_set_height(spectrum_root_, strip_h);
        lv_obj_set_y(spectrum_root_, baseline - strip_h);
    }
    const walle_spectrum::Frame frame = spectrum_source_();
    for (int i = 0; i < walle_spectrum::kBands; ++i) {
        const int h = std::max(2, frame.bands[i] * strip_h / 255);
        lv_obj_set_height(spectrum_bars_[i], h);
        lv_obj_set_y(spectrum_bars_[i], strip_h - h);  // grows up from the bottom, not mirrored
        if (rainbow) {
            // Same bars and peak-hold as winamp, but every bar keeps its own hue, sweeping once
            // around the wheel from green (bar 0) through cyan, blue, violet, magenta, red,
            // orange, yellow and lime - brighter at the bottom, dimmer at the top, with a pale
            // tint of the bar's own color as the peak dot (like the classic rainbow equalizer).
            const uint16_t hue = static_cast<uint16_t>((120 + i * 360 / walle_spectrum::kBands) % 360);
            lv_obj_set_style_bg_color(spectrum_bars_[i], lv_color_hsv_to_rgb(hue, 100, 50), 0);
            lv_obj_set_style_bg_grad_color(spectrum_bars_[i], lv_color_hsv_to_rgb(hue, 100, 100), 0);
            lv_obj_set_style_bg_grad_dir(spectrum_bars_[i], LV_GRAD_DIR_VER, 0);
            lv_obj_set_style_bg_color(spectrum_peaks_[i], lv_color_hsv_to_rgb(hue, 35, 100), 0);
        } else {
            lv_obj_set_style_bg_color(spectrum_bars_[i], WinampBandColor(frame.bands[i]), 0);
            lv_obj_set_style_bg_grad_dir(spectrum_bars_[i], LV_GRAD_DIR_NONE, 0);
            lv_obj_set_style_bg_color(spectrum_peaks_[i], lv_color_hex(0xffffff), 0);
        }

        const int peak_h = frame.peaks[i] * strip_h / 255;
        lv_obj_set_y(spectrum_peaks_[i],
                    std::clamp(strip_h - peak_h - kPeakHeight, 0, strip_h - kPeakHeight));
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
        UpdateVisual(walle_spectrum::Mode::kOff);
        return;
    }

    // Teal on the light theme, cyan on the dark one; the plain ribbon's color. The Winamp
    // spectrum bars use their own green/yellow/red coloring instead (see WinampBandColor), so
    // switching visualizer_mode changes the strip's look on purpose.
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    const bool dark_background =
        theme != nullptr && lv_color_brightness(theme->background_color()) < 128;
    const lv_color_t color = lv_color_hex(dark_background ? 0x2ee6d6 : 0x1d9e75);

    const walle_spectrum::Mode visual_mode = visualizer_mode_.load();
    if (mode == Ribbon::kOutput && visual_mode != walle_spectrum::Mode::kOff) {
        if (ribbon_visible_) {
            lv_obj_add_flag(ribbon_, LV_OBJ_FLAG_HIDDEN);
            ribbon_visible_ = false;
        }
        const bool strip = visual_mode == walle_spectrum::Mode::kWinamp ||
                           visual_mode == walle_spectrum::Mode::kRainbow;
        if (strip) {
            UpdateSpectrumBars();
        }
        if (strip != spectrum_visible_) {
            SetShown(spectrum_root_, strip);
            spectrum_visible_ = strip;
        }
        UpdateVisual(visual_mode);
        return;
    }
    if (spectrum_visible_) {
        lv_obj_add_flag(spectrum_root_, LV_OBJ_FLAG_HIDDEN);
        spectrum_visible_ = false;
    }
    UpdateVisual(walle_spectrum::Mode::kOff);

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

void WalleDisplay::CreateVisuals() {
    // Oscilloscope: one polyline across the strip.
    scope_line_ = lv_line_create(lv_layer_top());
    lv_obj_set_pos(scope_line_, (width_ - kScopeWidth) / 2, kStripCenterY - kScopeHeight / 2);
    lv_obj_set_style_line_width(scope_line_, 2, 0);
    lv_obj_set_style_line_color(scope_line_, lv_color_hex(kVizCyan), 0);
    lv_obj_set_style_line_rounded(scope_line_, true, 0);
    for (int j = 0; j < walle_spectrum::kWavePoints; ++j) {
        scope_points_[j].x = j * (kScopeWidth - 1) / (walle_spectrum::kWavePoints - 1);
        scope_points_[j].y = kScopeHeight / 2;
    }
    lv_line_set_points(scope_line_, scope_points_, walle_spectrum::kWavePoints);
    lv_obj_remove_flag(scope_line_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(scope_line_, LV_OBJ_FLAG_HIDDEN);

    // Robot mouth: an outlined pill under the eyes that opens with the voice, with 4 teeth.
    mouth_root_ = MakeBox(lv_layer_top(), (width_ - kMouthWidth) / 2, kStripCenterY - 3,
                          kMouthWidth, 6, kVizCyan, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(mouth_root_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mouth_root_, 4, 0);
    lv_obj_set_style_border_color(mouth_root_, lv_color_hex(kVizCyan), 0);
    lv_obj_set_style_clip_corner(mouth_root_, true, 0);
    for (int k = 0; k < 4; ++k) {
        mouth_teeth_[k] = MakeBox(mouth_root_, (k + 1) * kMouthWidth / 5 - 1, 0, 2, 2, kVizCyan, 0);
    }
    lv_obj_add_flag(mouth_root_, LV_OBJ_FLAG_HIDDEN);

    // Square canvas for radial / vu / orb; its pixel buffer is allocated on first use.
    viz_canvas_ = lv_canvas_create(lv_layer_top());
    lv_obj_set_pos(viz_canvas_, (width_ - kCanvasSize) / 2, kCanvasTop);
    lv_obj_remove_flag(viz_canvas_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(viz_canvas_, LV_OBJ_FLAG_HIDDEN);
}

bool WalleDisplay::EnsureCanvasBuffer() {
    if (viz_canvas_buf_ != nullptr) {
        return true;
    }
    const size_t size =
        LV_CANVAS_BUF_SIZE(kCanvasSize, kCanvasSize, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    viz_canvas_buf_ = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (viz_canvas_buf_ == nullptr) {
        ESP_LOGE(TAG, "No memory for the %u byte visualizer canvas", (unsigned)size);
        return false;
    }
    lv_canvas_set_buffer(viz_canvas_, viz_canvas_buf_, kCanvasSize, kCanvasSize,
                         LV_COLOR_FORMAT_RGB565);
    return true;
}

void WalleDisplay::UpdateVisual(walle_spectrum::Mode mode) {
    using walle_spectrum::Mode;
    const bool canvas_mode = mode == Mode::kRadial || mode == Mode::kVu || mode == Mode::kOrb;
    const bool canvas_ok = canvas_mode && EnsureCanvasBuffer();
    SetShown(scope_line_, mode == Mode::kScope);
    SetShown(mouth_root_, mode == Mode::kMouth);
    SetShown(viz_canvas_, canvas_ok);
    if (mode == Mode::kOff || mode == Mode::kWinamp || mode == Mode::kRainbow ||
        (canvas_mode && !canvas_ok)) {
        viz_level_ = 0.0f;
        vu_needle_ = 0.0f;
        for (auto& b : viz_bands_) {
            b = 0.0f;
        }
        return;
    }

    // Inputs: the 32 bands (fast attack, slower fall) and the overall speaker level, -50..-5 dBFS
    // mapped to 0..1 (same idea as the plain ribbon's scale).
    walle_spectrum::Frame frame;
    if (spectrum_source_) {
        frame = spectrum_source_();
    }
    for (int i = 0; i < walle_spectrum::kBands; ++i) {
        const float v = frame.bands[i];
        viz_bands_[i] = v > viz_bands_[i] ? v : viz_bands_[i] * 0.8f + v * 0.2f;
    }
    const int rms = output_rms_ ? output_rms_() : 0;
    const float db = rms > 0 ? 20.0f * log10f(rms / 32768.0f) : -90.0f;
    const float target = std::clamp((db + 50.0f) / 45.0f, 0.0f, 1.0f);
    viz_level_ += (target - viz_level_) * (target > viz_level_ ? 0.6f : 0.2f);

    switch (mode) {
        case Mode::kScope:
            for (int j = 0; j < walle_spectrum::kWavePoints; ++j) {
                scope_points_[j].y = kScopeHeight / 2 - frame.wave[j] * (kScopeHeight / 2 - 1) / 127;
            }
            lv_line_set_points(scope_line_, scope_points_, walle_spectrum::kWavePoints);
            break;
        case Mode::kMouth: {
            const int h = 6 + static_cast<int>(viz_level_ * 26.0f);
            lv_obj_set_height(mouth_root_, h);
            lv_obj_set_y(mouth_root_, kStripCenterY - h / 2);
            for (auto* tooth : mouth_teeth_) {
                SetShown(tooth, h > 14);
                lv_obj_set_height(tooth, std::max(2, h - 8));
            }
            break;
        }
        case Mode::kRadial:
            DrawRadial();
            break;
        case Mode::kVu:
            DrawVu();
            break;
        case Mode::kOrb:
            DrawOrb();
            break;
        default:
            break;
    }
}

void WalleDisplay::DrawRadial() {
    lv_canvas_fill_bg(viz_canvas_, lv_color_black(), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(viz_canvas_, &layer);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.width = 3;
    line.round_start = 1;
    line.round_end = 1;
    constexpr int kSpokes = walle_spectrum::kBands * 2;  // mirrored: low bands at the top and bottom
    for (int i = 0; i < kSpokes; ++i) {
        const int band = i < walle_spectrum::kBands ? i : kSpokes - 1 - i;
        const float b = viz_bands_[band] / 255.0f;
        const float a = i * 2.0f * kPi / kSpokes - kPi / 2.0f;
        const float r0 = 30.0f;
        const float r1 = r0 + 4.0f + b * 50.0f;
        line.color = lv_color_hex(i % 2 ? kVizCyan : 0x20a8e0);
        DrawLine(&layer, &line, kCanvasCenter + cosf(a) * r0, kCanvasCenter + sinf(a) * r0,
                 kCanvasCenter + cosf(a) * r1, kCanvasCenter + sinf(a) * r1);
    }
    const int core = 12 + static_cast<int>(viz_level_ * 12.0f);
    DrawCircle(&layer, kCanvasCenter, kCanvasCenter, core, core, kVizCyan);
    lv_canvas_finish_layer(viz_canvas_, &layer);
}

void WalleDisplay::DrawVu() {
    // Classic analog meter: cream face, scale arc with a red zone, ticks, a needle with ballistics
    // (rises quickly, falls slowly). LVGL angles: 0 = right, clockwise; 270 = straight up.
    constexpr int kPivotX = kCanvasCenter;
    constexpr int kPivotY = 150;
    constexpr int kStartDeg = 215;
    constexpr int kSweepDeg = 110;
    vu_needle_ += (viz_level_ - vu_needle_) * (viz_level_ > vu_needle_ ? 0.35f : 0.12f);

    lv_canvas_fill_bg(viz_canvas_, lv_color_hex(0x0b0b0b), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(viz_canvas_, &layer);

    lv_draw_rect_dsc_t face;
    lv_draw_rect_dsc_init(&face);
    face.bg_color = lv_color_hex(0xe8dcb0);
    face.radius = 12;
    lv_area_t face_area = {8, 22, kCanvasSize - 9, 140};
    lv_draw_rect(&layer, &face, &face_area);

    DrawCircle(&layer, kPivotX, kPivotY, 104, 2, 0x222222, kStartDeg, kStartDeg + kSweepDeg);
    DrawCircle(&layer, kPivotX, kPivotY, 104, 5, 0xc02020, kStartDeg + 85, kStartDeg + kSweepDeg);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.width = 2;
    for (int k = 0; k <= 10; ++k) {
        const float a = (kStartDeg + k * kSweepDeg / 10.0f) * kPi / 180.0f;
        const float r_in = k % 5 == 0 ? 86.0f : 92.0f;
        line.color = lv_color_hex(k >= 8 ? 0xc02020 : 0x222222);
        DrawLine(&layer, &line, kPivotX + cosf(a) * r_in, kPivotY + sinf(a) * r_in,
                 kPivotX + cosf(a) * 100.0f, kPivotY + sinf(a) * 100.0f);
    }

    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.text = "VU";
    label.font = LV_FONT_DEFAULT;
    label.color = lv_color_hex(0x222222);
    label.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t label_area = {kPivotX - 30, 104, kPivotX + 30, 124};
    lv_draw_label(&layer, &label, &label_area);

    const float a = (kStartDeg + vu_needle_ * kSweepDeg) * kPi / 180.0f;
    line.width = 3;
    line.round_end = 1;
    line.color = lv_color_hex(0x111111);
    DrawLine(&layer, &line, kPivotX, kPivotY, kPivotX + cosf(a) * 108.0f,
             kPivotY + sinf(a) * 108.0f);
    DrawCircle(&layer, kPivotX, kPivotY, 8, 8, 0x333333);
    lv_canvas_finish_layer(viz_canvas_, &layer);
}

void WalleDisplay::DrawOrb() {
    // Three wobbling rings, outer (dim) to inner (bright); each ring's radius follows a different
    // slice of the spectrum, scaled by how loud Jarvis is right now.
    static constexpr uint32_t kRingColors[3] = {0x0c4048, 0x1aa8b8, kVizCyan};
    constexpr int kSegments = 48;
    const float t = esp_timer_get_time() / 1000000.0f;
    lv_canvas_fill_bg(viz_canvas_, lv_color_black(), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(viz_canvas_, &layer);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.width = 3;
    line.round_start = 1;
    line.round_end = 1;
    for (int ring = 0; ring < 3; ++ring) {
        const int k = 2 - ring;  // k = 2 is the outermost
        line.color = lv_color_hex(kRingColors[ring]);
        float prev_x = 0.0f, prev_y = 0.0f;
        for (int q = 0; q <= kSegments; ++q) {
            const float a = q * 2.0f * kPi / kSegments;
            const float band = viz_bands_[(q + k * 5) % walle_spectrum::kBands] / 255.0f;
            float r = 30.0f + k * 16.0f + band * 18.0f * (0.4f + viz_level_) +
                      sinf(a * 3.0f + t * (2.0f + k)) * 3.0f;
            r = std::min(r, kCanvasCenter - 3.0f);
            const float x = kCanvasCenter + cosf(a) * r;
            const float y = kCanvasCenter + sinf(a) * r;
            if (q > 0) {
                DrawLine(&layer, &line, prev_x, prev_y, x, y);
            }
            prev_x = x;
            prev_y = y;
        }
    }
    lv_canvas_finish_layer(viz_canvas_, &layer);
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
