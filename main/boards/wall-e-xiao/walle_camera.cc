// videodev2.h must come before esp_video.h (via walle_camera.h): image_to_jpeg.h only defines
// the V4L2 pixel formats when they are missing, the same order upstream's esp_video.cc uses.
#include "linux/videodev2.h"

#include "walle_camera.h"

#include <algorithm>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "application.h"
#include "board.h"
#include "jpg/image_to_jpeg.h"
#include "walle_display.h"
#include "walle_settings.h"
#include "walle_sounds.h"

#define TAG "WalleCamera"

namespace {

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

WalleCamera::WalleCamera(const esp_video_init_config_t& config) : EspVideo(config) {}

WalleCamera::~WalleCamera() { CollageEnd(); }

void WalleCamera::CorrectColorCast() {
    // YUYV byte order: Y0 U Y1 V, repeating every 4 bytes (2 pixels). Only touch U/V; Y (luma)
    // carries no color and must be left alone.
    if (frame_.data == nullptr || frame_.format != V4L2_PIX_FMT_YUYV) {
        return;
    }
    // WALL-E: a single shared bias for U and V left a residual magenta cast (measured on
    // /debug/photo.jpg?format=yuyv: G channel ~11 points low, B channel ~12 points high relative
    // to the image average). A first attempt at independent biases (U -30, V -15) overshot the
    // other way (a wall/ceiling patch measured G +11, B -20 - clearly green/cyan); these values
    // were computed from that overshoot to land closer to neutral.
    constexpr int kChromaBiasU = -18;  // U (blue-difference)
    constexpr int kChromaBiasV = -22;  // V (red-difference)
    for (size_t i = 0; i + 3 < frame_.len; i += 4) {
        frame_.data[i + 1] =
            static_cast<uint8_t>(std::clamp(static_cast<int>(frame_.data[i + 1]) + kChromaBiasU, 0, 255));
        frame_.data[i + 3] =
            static_cast<uint8_t>(std::clamp(static_cast<int>(frame_.data[i + 3]) + kChromaBiasV, 0, 255));
    }
}

bool WalleCamera::Capture() {
    // WALL-E: a single take_photo/diagnostics shot shows the real picture (bottom-half preview,
    // 2 s, see EspVideo::Capture -> WalleDisplay::SetPreviewImage), no screen effect beforehand -
    // just the shutter sound (if enabled) and the picture itself. Only CollageAdd()'s own
    // per-frame shots for look_around stay suppressed.
    if (WalleSettings::GetInstance().GetBool("shutter_sound")) {
        Application::GetInstance().PlaySound(walle_sounds::Shutter());
    }
    if (!EspVideo::Capture()) {
        return false;
    }
    CorrectColorCast();
    return true;
}

std::string WalleCamera::LastFrameInfo() const {
    if (frame_.data == nullptr) {
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
    CorrectColorCast();
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

std::vector<uint8_t> WalleCamera::CaptureJpegAs(v4l2_pix_fmt_t as_format) {
    if (!Capture()) {
        return {};
    }
    uint8_t* out = nullptr;
    size_t out_len = 0;
    const bool ok =
        image_to_jpeg(frame_.data, frame_.len, frame_.width, frame_.height, as_format, 85, &out, &out_len);
    if (!ok || out == nullptr) {
        return {};
    }
    std::vector<uint8_t> result(out, out + out_len);
    heap_caps_free(out);  // image_to_jpeg allocates *out with heap_caps_malloc
    return result;
}

std::vector<uint8_t> WalleCamera::DebugCaptureJpeg(bool raw) {
    if (!EspVideo::Capture()) {
        return {};
    }
    if (!raw) {
        CorrectColorCast();
    }
    uint8_t* out = nullptr;
    size_t out_len = 0;
    const bool ok = image_to_jpeg(frame_.data, frame_.len, frame_.width, frame_.height,
                                  frame_.format, 85, &out, &out_len);
    if (!ok || out == nullptr) {
        return {};
    }
    std::vector<uint8_t> result(out, out + out_len);
    heap_caps_free(out);
    return result;
}

std::string WalleCamera::DebugCaptureStatsJson(bool raw) {
    if (!EspVideo::Capture()) {
        return "{\"error\":\"capture failed\"}";
    }
    if (!raw) {
        CorrectColorCast();
    }
    if (frame_.data == nullptr || frame_.format != V4L2_PIX_FMT_YUYV) {
        return "{\"error\":\"frame is not YUYV\"}";
    }
    // YUYV: Y0 U Y1 V per 2 pixels. Sums over the whole frame, the center third, and pixel pairs
    // whose luma is bright (Y >= 170: whites and light surfaces, where a color cast shows most).
    struct Acc {
        uint64_t y = 0, u = 0, v = 0, n = 0;
        void Add(int yy, int uu, int vv) { y += yy; u += uu; v += vv; ++n; }
        std::string Json() const {
            char buf[96];
            if (n == 0) {
                return "{\"n\":0}";
            }
            snprintf(buf, sizeof(buf), "{\"n\":%llu,\"y\":%.1f,\"u\":%.1f,\"v\":%.1f}",
                     (unsigned long long)n, (double)y / n, (double)u / n, (double)v / n);
            return buf;
        }
    } all, center, bright;
    const int w = frame_.width;
    const int h = frame_.height;
    for (int row = 0; row < h; ++row) {
        const uint8_t* p = frame_.data + static_cast<size_t>(row) * w * 2;
        for (int x = 0; x + 1 < w; x += 2, p += 4) {
            const int yy = (p[0] + p[2]) / 2;
            all.Add(yy, p[1], p[3]);
            if (row >= h / 3 && row < 2 * h / 3 && x >= w / 3 && x < 2 * w / 3) {
                center.Add(yy, p[1], p[3]);
            }
            if (yy >= 170) {
                bright.Add(yy, p[1], p[3]);
            }
        }
    }
    static constexpr uint16_t kRegs[] = {
        0x3400, 0x3401, 0x3402, 0x3403, 0x3404, 0x3405, 0x3406,  // AWB manual gains / control
        0x5001, 0x5180, 0x5183, 0x5196, 0x5197, 0x5198, 0x5199,  // ISP ctrl, AWB ctrl, AWB state
        0x519a, 0x519b, 0x519c, 0x519d, 0x519e, 0x519f, 0x51a0,
        0x3503, 0x3a0f, 0x3a10, 0x3820, 0x3821, 0x4300, 0x501f,  // AE, flip, output format
        0x303d, 0x3824, 0x3814, 0x3815};
    std::string regs;
    for (uint16_t reg : kRegs) {
        char item[24];
        snprintf(item, sizeof(item), "%s\"%04x\":%d", regs.empty() ? "" : ",", reg,
                 DebugReadSensorReg(reg));
        regs += item;
    }
    char head[160];
    snprintf(head, sizeof(head), "{\"sensor_format\":\"%s\",\"width\":%d,\"height\":%d,\"raw\":%s,",
             DebugSensorFormatName().c_str(), w, h, raw ? "true" : "false");
    return std::string(head) + "\"all\":" + all.Json() + ",\"center\":" + center.Json() +
           ",\"bright\":" + bright.Json() + ",\"regs\":{" + regs + "}}";
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
