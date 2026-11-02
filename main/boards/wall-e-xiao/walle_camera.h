#pragma once

#include <atomic>
#include <expected>
#include <string>
#include <vector>

#include "esp_video.h"

// OV3660 or OV5640 (autofocus) through upstream's esp_video path, plus Wall-E's photo cue and the look_around
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

    // Colour saturation, -4 (pale) .. 4 (strong), 0 = the sensor's own default: the OV3660's colour matrix (0x5381-0x538B), written
    // directly from the sensor driver's own table, since esp_video passes no saturation control to a DVP sensor. Other sensors: false.
    bool SetSaturation(int level);

    // OV5640 only (the autofocus module): every call below does nothing on another sensor.
    //
    // Pad drive strength, 0 = 1x (the coolest), 1 = 2x, 2 = 3x, 3 = 4x (the chip's own default). Seeed's wiki ("OV5640 Heat Optimization")
    // lowers it to 1x after esp_camera_init() and measured the chip dropping from over 70 C to about 54 C while streaming; the price is slower
    // signal edges, so a noisy wire could show as speckles - set camera_drive back up if that ever happens. Read-modify-write of register
    // 0x302C bits 7:6, so the other bits keep their values. A sensor restart puts the default back: write it again (SensorRestarted + this).
    bool SetPadDrive(int level);

    // Call after the sensor was (re)started: the focus firmware lives in the sensor's RAM and is gone after a reset.
    void SensorRestarted();

    // Continuous autofocus on or off. The first time on after a start-up the 4 KB focus firmware is loaded (about 2 s, in a task of its own,
    // pictures keep working meanwhile); off pauses the focus where it is. Returns at once; AutofocusState() says how it went.
    void SetAutofocus(bool on);

    // "none" (not an OV5640), "off", "loading", "on" or "failed" (the focus processor did not answer).
    const char* AutofocusState() const;

    // Detected at every startup (see the constructor): whether esp_video found a camera, and which
    // sensor it reported. Shown on the settings page and logged at boot.
    bool Detected() const { return CameraPresent(); }
    const std::string& SensorName() const { return sensor_name_; }

private:
    enum class AfState : uint8_t { kNone, kOff, kLoading, kOn, kFailed };
    bool LoadAutofocusFirmware();
    bool AutofocusStart();                   // continuous focus, as esp32-camera's ov5640_af does it
    bool AutofocusCommand(uint8_t command);  // handshake: ack = 1, command, wait for the ack to clear
    bool WaitAutofocusReg(uint16_t reg, uint8_t value, int timeout_ms);
    void AutofocusWork();
    static void AutofocusTask(void* arg);

    std::atomic<AfState> af_state_{AfState::kNone};
    std::atomic<bool> af_wanted_{false};
    std::atomic<bool> af_applied_{false};    // what the sensor is doing now (on = continuous focus running)
    std::atomic<bool> af_loaded_{false};     // the firmware is in the sensor
    std::atomic<bool> af_busy_{false};       // a task is working on it
    std::string sensor_name_;
    uint8_t* collage_ = nullptr;
    size_t collage_len_ = 0;
    uint16_t collage_width_ = 0;
    uint16_t collage_height_ = 0;
    v4l2_pix_fmt_t collage_format_ = 0;
};
