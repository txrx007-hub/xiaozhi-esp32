#pragma once

#include <expected>
#include <string>
#include <vector>

#include "esp_video.h"

// OV3660 through upstream's esp_video path, plus Wall-E's photo cue and the look_around
// collage (four quarter-size frames in one image, so the vision model is asked only once).
class WalleCamera : public EspVideo {
public:
    explicit WalleCamera(const esp_video_init_config_t& config);
    ~WalleCamera() override;

    // Photo cue: brief surprised face (and a click if shutter_sound = 1), no preview.
    bool Capture() override;

    bool CollageBegin();
    bool CollageAdd(int slot);  // 0 = top-left ... 3 = bottom-right
    std::expected<std::string, std::string> CollageExplain(const std::string& question);
    void CollageEnd();

    std::string LastFrameInfo() const;

    // WALL-E: debugging the purple-hue report (see walle_web.cc /debug/photo.jpg). Captures one
    // frame, then encodes THAT SAME frame as JPEG under an explicitly chosen pixel format,
    // ignoring whatever tag Capture() applied - so the two color-order hypotheses can be
    // compared on one identical frame instead of two separate (and slightly different) shots.
    // Empty result on failure. Not used by any normal voice/diagnostics path.
    std::vector<uint8_t> CaptureJpegAs(v4l2_pix_fmt_t as_format);

private:
    // WALL-E: this sensor/module's raw YUYV output carries a uniform magenta/purple color cast
    // (confirmed via /debug/photo.jpg: the byte order itself is correct - retagging it as UYVY,
    // tried first, produced a much worse banded corruption, not a fix). esp_video/esp_cam_sensor
    // define a white-balance control (ESP_CAM_SENSOR_WB) but no code in this stack actually wires
    // it through a V4L2 control, so there is no hardware AWB knob to turn from here. Correcting it
    // in software instead: pulls every U and V byte back toward neutral (128), each by its own
    // fixed, by-eye-tuned amount (a single shared amount left a residual tint - U and V needed
    // different corrections). Applied right after each raw capture, in our own code only.
    void CorrectColorCast();

    uint8_t* collage_ = nullptr;
    size_t collage_len_ = 0;
    uint16_t collage_width_ = 0;
    uint16_t collage_height_ = 0;
    v4l2_pix_fmt_t collage_format_ = 0;
};
