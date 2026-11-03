// videodev2.h must come before esp_video.h (via walle_camera.h): image_to_jpeg.h only defines
// the V4L2 pixel formats when they are missing, the same order upstream's esp_video.cc uses.
#include "linux/videodev2.h"

#include "walle_camera.h"

#include <algorithm>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "board.h"
#include "jpg/image_to_jpeg.h"
#include "walle_display.h"
#include "walle_ov5640_af_fw.h"
#include "walle_settings.h"
#include "walle_sounds.h"

#define TAG "WalleCamera"

namespace {

// OV5640 registers. The focus processor's: Omnivision's AF guide, in the order esp32-camera's ov5640_af.c uses them.
constexpr uint16_t kPadDriveReg = 0x302C;      // bits 7:6 = pad drive strength
constexpr uint16_t kAfReset = 0x3000;          // 0x20 holds the focus processor in reset, 0x00 lets it run
constexpr uint16_t kAfFirmwareBase = 0x8000;   // the processor's program memory
constexpr uint16_t kAfCmdMain = 0x3022;
constexpr uint16_t kAfCmdAck = 0x3023;
constexpr uint16_t kAfCmdPara0 = 0x3024;       // 0x3024..0x3028: command parameters
constexpr uint16_t kAfFwStatus = 0x3029;
constexpr uint8_t kAfStatusIdle = 0x70;
constexpr uint8_t kAfStatusFocusing = 0x00;
constexpr uint8_t kAfStatusFocused = 0x10;
constexpr uint8_t kAfCmdContinuous = 0x04;
constexpr uint8_t kAfCmdPause = 0x06;
constexpr uint8_t kAfCmdRelease = 0x08;

WalleDisplay* GetWalleDisplay() {
    return static_cast<WalleDisplay*>(Board::GetInstance().GetDisplay());
}

const char* FormatName(v4l2_pix_fmt_t format) {
    switch (format) {
        case V4L2_PIX_FMT_YUYV:
            return "YUYV";
        case V4L2_PIX_FMT_UYVY:
            return "UYVY";
        case V4L2_PIX_FMT_RGB565:
            return "RGB565";
        case V4L2_PIX_FMT_JPEG:
            return "JPEG";
        default:
            return "other";
    }
}

}  // namespace

WalleCamera::WalleCamera(const esp_video_init_config_t& config) : EspVideo(config) {
    sensor_name_ = DetectSensorName();
    if (CameraPresent()) {
        ESP_LOGI(TAG, "Camera detected: %s", sensor_name_.c_str());
        if (sensor_name_ == "OV5640") {
            af_state_ = AfState::kOff;
        }
        EnableAutoWhiteBalance();
    } else {
        ESP_LOGW(TAG, "No camera detected");
    }
}

WalleCamera::~WalleCamera() { CollageEnd(); }

void WalleCamera::EnableAutoWhiteBalance() {
    constexpr uint16_t kAwbControl3 = 0x5183;
    constexpr uint8_t kAwbControl3Value = 0x94;
    if (!WriteSensorReg(kAwbControl3, kAwbControl3Value)) {
        ESP_LOGW(TAG, "Could not enable the sensor's auto white balance");
    }
}

bool WalleCamera::SetSaturation(int level) {
    // From esp_cam_sensor's ov3660_settings.h (sensor_saturation_levels): one row per level -4..4; row 4 (level 0) is the
    // same matrix the sensor's start-up table writes.
    static constexpr uint8_t kLevels[9][11] = {
        {0x1d, 0x60, 0x03, 0x07, 0x48, 0x4f, 0x4b, 0x40, 0x0b, 0x01, 0x98},  // -4
        {0x1d, 0x60, 0x03, 0x08, 0x54, 0x5c, 0x58, 0x4b, 0x0d, 0x01, 0x98},  // -3
        {0x1d, 0x60, 0x03, 0x0a, 0x60, 0x6a, 0x64, 0x56, 0x0e, 0x01, 0x98},  // -2
        {0x1d, 0x60, 0x03, 0x0b, 0x6c, 0x77, 0x70, 0x60, 0x10, 0x01, 0x98},  // -1
        {0x1d, 0x60, 0x03, 0x0c, 0x78, 0x84, 0x7d, 0x6b, 0x12, 0x01, 0x98},  // 0
        {0x1d, 0x60, 0x03, 0x0d, 0x84, 0x91, 0x8a, 0x76, 0x14, 0x01, 0x98},  // +1
        {0x1d, 0x60, 0x03, 0x0e, 0x90, 0x9e, 0x96, 0x80, 0x16, 0x01, 0x98},  // +2
        {0x1d, 0x60, 0x03, 0x10, 0x9c, 0xac, 0xa2, 0x8b, 0x17, 0x01, 0x98},  // +3
        {0x1d, 0x60, 0x03, 0x11, 0xa8, 0xb9, 0xaf, 0x96, 0x19, 0x01, 0x98},  // +4
    };
    if (level < -4 || level > 4 || sensor_name_ != "OV3660") {
        return false;
    }
    for (int i = 0; i < 11; ++i) {
        if (!WriteSensorReg(0x5381 + i, kLevels[level + 4][i])) {
            ESP_LOGW(TAG, "Could not set the colour saturation (register 0x%04x)", 0x5381 + i);
            return false;
        }
    }
    return true;
}

bool WalleCamera::SetPadDrive(int level) {
    if (af_state_ == AfState::kNone || level < 0 || level > 3) {
        return false;
    }
    uint8_t value = 0;
    if (!ReadSensorReg(kPadDriveReg, &value)) {
        ESP_LOGW(TAG, "Could not read the sensor's pad drive register");
        return false;
    }
    const uint8_t wanted = static_cast<uint8_t>((value & ~0xC0) | (level << 6));
    if (wanted != value && !WriteSensorReg(kPadDriveReg, wanted)) {
        ESP_LOGW(TAG, "Could not set the sensor's pad drive strength");
        return false;
    }
    ESP_LOGI(TAG, "Sensor pad drive %dx (register 0x302C 0x%02x -> 0x%02x)", level + 1, value, wanted);
    return true;
}

void WalleCamera::SensorRestarted() {
    af_loaded_ = false;
    af_applied_ = false;
    if (af_state_ != AfState::kNone && af_state_ != AfState::kLoading) {
        af_state_ = AfState::kOff;
    }
}

bool WalleCamera::WaitAutofocusReg(uint16_t reg, uint8_t value, int timeout_ms) {
    const int64_t until = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    do {
        uint8_t now = 0;
        if (!ReadSensorReg(reg, &now)) {
            return false;
        }
        if (now == value) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10) > 0 ? pdMS_TO_TICKS(10) : 1);   // at least one tick, however fast the tick is
    } while (esp_timer_get_time() < until);
    return false;
}

bool WalleCamera::LoadAutofocusFirmware() {
    bool ok = WriteSensorReg(kAfReset, 0x20);   // stop the focus processor while its program is written
    for (size_t i = 0; ok && i < sizeof(ov5640_af_firmware); ++i) {
        ok = WriteSensorReg(static_cast<uint16_t>(kAfFirmwareBase + i), ov5640_af_firmware[i]);
        if ((i & 0xFF) == 0xFF) {
            vTaskDelay(1);   // the camera task and the web server get their turns in between
        }
    }
    for (uint16_t reg = kAfCmdMain; ok && reg <= kAfCmdPara0 + 4; ++reg) {
        ok = WriteSensorReg(reg, 0x00);   // command, acknowledge and the five parameters start empty
    }
    ok = ok && WriteSensorReg(kAfFwStatus, 0x7F) && WriteSensorReg(kAfReset, 0x00);   // run it
    return ok && WaitAutofocusReg(kAfFwStatus, kAfStatusIdle, 3000);
}

bool WalleCamera::AutofocusCommand(uint8_t command) {
    return WriteSensorReg(kAfCmdAck, 0x01) && WriteSensorReg(kAfCmdMain, command) && WaitAutofocusReg(kAfCmdAck, 0x00, 1000);
}

void WalleCamera::LogAutofocusRegs(const char* where) {
    uint8_t main = 0xEE, ack = 0xEE, status = 0xEE;
    ReadSensorReg(kAfCmdMain, &main);
    ReadSensorReg(kAfCmdAck, &ack);
    ReadSensorReg(kAfFwStatus, &status);
    ESP_LOGI(TAG, "Autofocus %s: command 0x%02x, ack 0x%02x, firmware status 0x%02x", where, main, ack, status);
}

bool WalleCamera::AutofocusStart() {
    LogAutofocusRegs("before start");
    // First the order esp32-camera's ov5640_af_start() uses: the focus is released, then continuous focus is requested.
    if (WriteSensorReg(kAfCmdMain, 0x01) && WriteSensorReg(kAfCmdMain, kAfCmdRelease) && WaitAutofocusReg(kAfCmdAck, 0x00, 2000) &&
        AutofocusCommand(kAfCmdContinuous)) {
        return true;
    }
    LogAutofocusRegs("after the first way failed");
    // In continuous mode this firmware takes the command (the command register goes back to 0) and starts focusing (status 0x00, then 0x10 when
    // sharp) but leaves the acknowledge flag set: that is a running autofocus, not a failure.
    uint8_t status = kAfStatusIdle;
    if (ReadSensorReg(kAfFwStatus, &status) && (status == kAfStatusFocusing || status == kAfStatusFocused)) {
        ESP_LOGI(TAG, "Autofocus: the focus processor is working (status 0x%02x), the acknowledge flag stays set", status);
        return true;
    }
    // Then the plain handshake of Omnivision's AF guide for both steps: ack = 1, command, wait for the ack to clear.
    if (AutofocusCommand(kAfCmdRelease) && AutofocusCommand(kAfCmdContinuous)) {
        ESP_LOGI(TAG, "Autofocus: the plain handshake worked");
        return true;
    }
    LogAutofocusRegs("after the second way failed");
    return false;
}

void WalleCamera::AutofocusWork() {
    for (;;) {
        const bool want = af_wanted_;
        if (want == af_applied_) {
            return;
        }
        if (want) {
            if (!af_loaded_) {
                af_state_ = AfState::kLoading;
                const int64_t start = esp_timer_get_time();
                if (!LoadAutofocusFirmware()) {
                    ESP_LOGW(TAG, "Autofocus: the focus processor did not start");
                    af_state_ = AfState::kFailed;
                    return;
                }
                af_loaded_ = true;
                ESP_LOGI(TAG, "Autofocus firmware loaded (%u bytes, %d ms)", static_cast<unsigned>(sizeof(ov5640_af_firmware)),
                         static_cast<int>((esp_timer_get_time() - start) / 1000));
            }
            if (!AutofocusStart()) {
                ESP_LOGW(TAG, "Autofocus: the focus processor did not accept the command");
                af_state_ = AfState::kFailed;
                return;
            }
            af_applied_ = true;
            af_state_ = AfState::kOn;
            ESP_LOGI(TAG, "Autofocus on (continuous)");
            // For the log: how the focus processor settles (0x00 focusing, 0x10 sharp), for the first few seconds only.
            uint8_t last = 0xEE;
            for (int i = 0; i < 24 && af_wanted_; ++i) {
                uint8_t now = last;
                if (ReadSensorReg(kAfFwStatus, &now) && now != last) {
                    ESP_LOGI(TAG, "Autofocus status 0x%02x after %d ms", now, i * 250);
                    last = now;
                }
                vTaskDelay(pdMS_TO_TICKS(250) > 0 ? pdMS_TO_TICKS(250) : 1);
            }
        } else {
            if (af_loaded_ && !AutofocusCommand(kAfCmdPause)) {
                ESP_LOGW(TAG, "Autofocus: pause was not acknowledged");
            }
            af_applied_ = false;
            af_state_ = AfState::kOff;
            ESP_LOGI(TAG, "Autofocus off (the lens stays where it is)");
        }
    }
}

void WalleCamera::AutofocusTask(void* arg) {
    auto* self = static_cast<WalleCamera*>(arg);
    do {
        self->AutofocusWork();
        self->af_busy_ = false;
        // A change that came in just as the work ended is picked up here (the caller saw "busy" and left it to this task).
    } while (self->af_wanted_ != self->af_applied_ && self->af_state_ != AfState::kFailed && !self->af_busy_.exchange(true));
    vTaskDelete(nullptr);
}

void WalleCamera::SetAutofocus(bool on) {
    if (af_state_ == AfState::kNone) {
        return;
    }
    af_wanted_ = on;
    if (af_state_ == AfState::kFailed) {
        af_state_ = AfState::kOff;   // switching it again is a new try
    }
    if (af_wanted_ == af_applied_ || af_busy_.exchange(true)) {
        return;
    }
    if (xTaskCreate(&WalleCamera::AutofocusTask, "cam_af", 4096, this, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Autofocus: no memory for its task");
        af_busy_ = false;
        af_state_ = AfState::kFailed;
    }
}

const char* WalleCamera::AutofocusState() const {
    switch (af_state_.load()) {
        case AfState::kOff:
            return "off";
        case AfState::kLoading:
            return "loading";
        case AfState::kOn:
            return "on";
        case AfState::kFailed:
            return "failed";
        default:
            return "none";
    }
}

bool WalleCamera::Capture() {
    // WALL-E: a single take_photo/diagnostics shot shows the real picture (centred preview,
    // 3.5 s, see EspVideo::Capture -> WalleDisplay::SetPreviewImage), no screen effect beforehand -
    // just the shutter sound (if enabled) and the picture itself. Only CollageAdd()'s own
    // per-frame shots for look_around stay suppressed.
    if (WalleSettings::GetInstance().GetBool("shutter_sound")) {
        Application::GetInstance().PlaySound(walle_sounds::Shutter());
    }
    if (!EspVideo::Capture()) {
        return false;
    }
    return true;
}

std::string WalleCamera::LastFrameInfo() const {
    if (frame_.format == 0) {  // the pixels are released after use (ReleaseFrame), the size and format stay
        return "no frame yet";
    }
    return std::to_string(frame_.width) + "x" + std::to_string(frame_.height) + " " +
           FormatName(frame_.format);
}

bool WalleCamera::CollageBegin() {
    CollageEnd();
    return true;
}

void WalleCamera::CollageEnd() {
    if (collage_ != nullptr) {
        heap_caps_free(collage_);
        collage_ = nullptr;
    }
    collage_len_ = 0;
}

bool WalleCamera::CollageAdd(int slot) {
    if (slot < 0 || slot > 3) {
        return false;
    }
    auto* display = GetWalleDisplay();
    if (display != nullptr) {
        display->SuppressNextPreview();
    }
    if (!EspVideo::Capture()) {  // no photo cue for the individual look_around shots
        return false;
    }
    const auto& f = frame_;
    const bool packed_yuv = f.format == V4L2_PIX_FMT_YUYV || f.format == V4L2_PIX_FMT_UYVY;
    if (f.data == nullptr || (!packed_yuv && f.format != V4L2_PIX_FMT_RGB565)) {
        ESP_LOGE(TAG, "Collage needs RGB565 or YUV422 frames, got %s", FormatName(f.format));
        return false;
    }

    if (collage_ == nullptr) {
        collage_width_ = f.width & ~3;
        collage_height_ = f.height & ~1;
        collage_format_ = f.format;
        collage_len_ = static_cast<size_t>(collage_width_) * collage_height_ * 2;
        collage_ = static_cast<uint8_t*>(
            heap_caps_malloc(collage_len_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (collage_ == nullptr) {
            ESP_LOGE(TAG, "No memory for a %u byte collage", (unsigned)collage_len_);
            return false;
        }
        if (packed_yuv) {
            // Black in YUV: Y = 16, U = V = 128
            const uint8_t black[4] = {
                static_cast<uint8_t>(f.format == V4L2_PIX_FMT_YUYV ? 16 : 128),
                static_cast<uint8_t>(f.format == V4L2_PIX_FMT_YUYV ? 128 : 16),
                static_cast<uint8_t>(f.format == V4L2_PIX_FMT_YUYV ? 16 : 128),
                static_cast<uint8_t>(f.format == V4L2_PIX_FMT_YUYV ? 128 : 16)};
            for (size_t i = 0; i + 4 <= collage_len_; i += 4) {
                memcpy(collage_ + i, black, 4);
            }
        } else {
            memset(collage_, 0, collage_len_);
        }
    }
    if (f.format != collage_format_ || f.width < collage_width_ || f.height < collage_height_) {
        ESP_LOGE(TAG, "Frame format changed during look_around");
        return false;
    }

    // Nearest-neighbour 2x downscale into one quadrant.
    const int quarter_w = collage_width_ / 2;
    const int quarter_h = collage_height_ / 2;
    const int origin_x = (slot % 2) * quarter_w;
    const int origin_y = (slot / 2) * quarter_h;
    const size_t src_stride = static_cast<size_t>(f.width) * 2;
    const size_t dst_stride = static_cast<size_t>(collage_width_) * 2;
    for (int y = 0; y < quarter_h; ++y) {
        const uint8_t* src = f.data + static_cast<size_t>(y * 2) * src_stride;
        uint8_t* dst = collage_ + static_cast<size_t>(origin_y + y) * dst_stride +
                       static_cast<size_t>(origin_x) * 2;
        if (!packed_yuv) {
            for (int x = 0; x < quarter_w; ++x) {
                dst[x * 2] = src[x * 4];
                dst[x * 2 + 1] = src[x * 4 + 1];
            }
            continue;
        }
        // 4 bytes = 2 pixels. Output pair k takes pixel 4k and 4k+2 from the source.
        for (int k = 0; k < quarter_w / 2; ++k) {
            const uint8_t* m0 = src + static_cast<size_t>(k) * 8;
            const uint8_t* m1 = m0 + 4;
            uint8_t* out = dst + static_cast<size_t>(k) * 4;
            if (f.format == V4L2_PIX_FMT_YUYV) {  // Y0 U Y1 V
                out[0] = m0[0];
                out[1] = m0[1];
                out[2] = m1[0];
                out[3] = m0[3];
            } else {  // U Y0 V Y1
                out[0] = m0[0];
                out[1] = m0[1];
                out[2] = m0[2];
                out[3] = m1[1];
            }
        }
    }
    return true;
}

std::expected<std::string, std::string> WalleCamera::CollageExplain(const std::string& question) {
    if (collage_ == nullptr) {
        return std::unexpected("No photos were taken");
    }
    // Hand the collage to upstream's Explain() through frame_. The previous frame is no
    // longer needed; EspVideo frees frame_.data on the next Capture().
    if (frame_.data != nullptr) {
        heap_caps_free(frame_.data);
    }
    frame_.data = collage_;
    frame_.len = collage_len_;
    frame_.width = collage_width_;
    frame_.height = collage_height_;
    frame_.format = collage_format_;
    collage_ = nullptr;
    collage_len_ = 0;
    return EspVideo::Explain(question);
}
