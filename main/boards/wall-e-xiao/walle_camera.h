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

    // WALL-E: this OV3660's auto white balance never engages on its own after start-up - its gains
    // stay at 1.0 forever (measured: auto and off give identical frames) and every picture keeps a
    // magenta/purple cast. One write to ISP register 0x5183 (bit 7, the value Omnivision's
    // reference AWB tables use) wakes it: the gains then adapt to the scene by themselves and
    // recover from wrong values. Safe to call again; boot calls it once at construction and once
    // more after the first frame is confirmed.
    void EnableAutoWhiteBalance();

    // Detected at every startup (see the constructor): whether esp_video found a camera, and which
    // sensor it reported. Shown on the settings page and logged at boot.
    bool Detected() const { return CameraPresent(); }
    const std::string& SensorName() const { return sensor_name_; }

private:
    std::string sensor_name_;
    uint8_t* collage_ = nullptr;
    size_t collage_len_ = 0;
    uint16_t collage_width_ = 0;
    uint16_t collage_height_ = 0;
    v4l2_pix_fmt_t collage_format_ = 0;
};
