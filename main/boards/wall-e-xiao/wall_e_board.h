#pragma once

#include <atomic>
#include <expected>
#include <string>
#include <vector>

#include <esp_timer.h>

#include "button.h"
#include "device_state.h"
#include "motor_driver.h"
#include "power_save_timer.h"
#include "walle_audio_codec.h"
#include "walle_camera.h"
#include "walle_display.h"
#include "walle_state_hook.h"
#include "wifi_board.h"

// Wall-E: Seeed XIAO ESP32-S3 Sense + ST7789 240x240 + MAX98357A + Mini L298N + OV3660.
class WallEBoard : public WifiBoard {
public:
    WallEBoard();

    static WallEBoard& Get() { return static_cast<WallEBoard&>(Board::GetInstance()); }

    // Board interface
    AudioCodec* GetAudioCodec() override;
    Display* GetDisplay() override { return display_; }
    Camera* GetCamera() override { return camera_; }
    Led* GetLed() override { return &state_hook_; }
    bool OnWakeWordDetected(const std::string& wake_word) override;
    void SetPowerSaveLevel(PowerSaveLevel level) override;
    void SetNetworkEventCallback(NetworkEventCallback callback) override;

    // Wall-E parts
    MotorDriver& motors() { return *motors_; }
    WalleDisplay* walle_display() { return display_; }
    WalleCamera* walle_camera() { return camera_; }
    WalleAudioCodec* walle_codec();

    // Nap (light standby): blank screen, CPU 160 MHz, Wi-Fi and wake word stay on.
    // Requested during a conversation, it starts when the reply has finished.
    void RequestNap(const char* reason);
    void ExitNap();
    bool IsNapping() const { return napping_; }

    // Deep sleep: everything off; wakes on BOOT or the timer. Starts after the reply.
    std::expected<std::string, std::string> RequestDeepSleep(int minutes, const std::string& wake_at);
    void EnterDeepSleepNow();

    // From the settings page: nobody is speaking a confirmation, so don't wait for a reply -
    // start about 2 s from now (the page needs that long to receive its answer first).
    void RequestNapNow();
    std::expected<std::string, std::string> RequestDeepSleepNow(int minutes);

    bool QuietHoursActive() const;
    bool IsRinging() const { return ringing_; }
    void StopRinging();
    bool NetworkConnected() const { return network_connected_; }

    // Voice-triggered idle screen ("screensaver"): shows the idle clock right away, like
    // InitializeIdleClock()'s own timer does after idle_clock_min minutes, but on
    // demand. Deferred to the next time the device is actually idle (the same pattern as
    // RequestNap), since the tool call itself runs mid-conversation, before the confirmation
    // reply is even spoken. Leaves automatically the moment the device leaves kDeviceStateIdle
    // (wake word etc.) - the same existing exit path idle_clock_min already uses.
    void RequestScreensaver();

    // Voice-triggered QR pages (self.display.show_qr): target "settings" (Jarvis's own page), "backend"
    // (the control panel on the own server) or "both" (one after the other, 10 s each, about a minute
    // in all). Same deferral as the screensaver: shown once the spoken reply is done and Jarvis is
    // idle, and gone the moment someone says the wake word. Returns what to tell the user, or why not.
    std::expected<std::string, std::string> RequestQr(const std::string& target);

    // Shut the camera down and bring it up again in place, then check that frames arrive (CameraBootProbe, console "!camera restart").
    bool RestartCameraInPlace(int attempt);

    // From WalleStateHook (main task)
    void OnDeviceStateChanged(DeviceState previous, DeviceState now);

private:
    void InitializeSpi();
    void InitializeDisplay();
    void InitializeCamera();
    void InitializeButtons();
    void InitializeIdleClock();
    void ApplySetting(const std::string& key);
    void ApplyVolumeCap();
    void ApplyWakeThreshold(bool speaking);
    void StartRinging(const std::string& label);
    void Tick();
    void CheckWifiSetupTimeout(int64_t now);
    void CameraBootProbe();  // main task, once, 6 s after boot
    void ApplyCameraSettings();   // flip, mirror and brightness: after creating the camera and after restarting it
    void UpdateStatusDot();
    void SetCpuMhz(int mhz);
    void EnterNapNow();

    Button boot_button_;
    WalleDisplay* display_ = nullptr;
    WalleCamera* camera_ = nullptr;
    const esp_video_init_config_t* camera_config_ = nullptr;   // static in InitializeCamera(); reused by EspVideo::Restart
    MotorDriver* motors_ = nullptr;
    WalleStateHook<WallEBoard> state_hook_{this};
    PowerSaveTimer* idle_timer_ = nullptr;
    esp_timer_handle_t tick_timer_ = nullptr;
    esp_timer_handle_t listen_poll_timer_ = nullptr;
    esp_timer_handle_t camera_probe_timer_ = nullptr;
    bool screensaver_requested_ = false;
    bool screensaver_spoke_ = false;
    int64_t screensaver_deadline_us_ = 0;

    // QR pages (see RequestQr): shares screensaver_spoke_ / screensaver_deadline_us_ while waiting for
    // the reply to finish, since only one of the two can be pending at a time.
    void StartQr();
    void RefreshQrUrls();
    bool qr_requested_ = false;
    std::vector<WalleDisplay::QrPage> qr_pages_;
    std::vector<int> qr_kinds_;  // per page: 0 = settings page, 1 = backend
    int64_t qr_start_us_ = 0;
    int qr_index_ = -1;

    DeviceState state_ = kDeviceStateUnknown;
    bool napping_ = false;
    bool pending_nap_ = false;
    // Ignore a wake word detected before this time even once actually napping: this board has no
    // echo cancellation, and trailing/buffered playback of the nap confirmation can still reach
    // the mic for a moment after EnterNapNow() itself has already run.
    int64_t nap_wake_guard_until_us_ = 0;
    bool pending_deep_sleep_ = false;
    int64_t pending_deep_sleep_s_ = 0;
    int64_t pending_deadline_us_ = 0;
    bool spoke_since_request_ = false;
    bool log_level_applied_ = false;
    std::atomic<bool> ringing_{false};
    int64_t ring_until_us_ = 0;
    int64_t next_ring_us_ = 0;
    std::atomic<bool> network_connected_{false};
    // WiFi setup hotspot auto-exit (CheckWifiSetupTimeout, run from Tick()).
    bool wifi_setup_active_ = false;
    int64_t wifi_setup_since_us_ = 0;
    int64_t wifi_setup_client_us_ = 0;
    int applied_volume_cap_ = -1;
    std::string weather_line_;

    // Server link recovery (OnServerConnectFailed, CheckServerRetry from Tick()). A wake word that ends in
    // "Unable to connect to service" is tried again 5 s later, twice; when that fails too the Wi-Fi link is
    // probably half dead (the robot still looks connected, but replies never arrive), so Wi-Fi is dropped and
    // joined again, followed by one more try. Then it waits for the next wake word.
    void OnServerConnectFailed();
    void CheckServerRetry(int64_t now);
    int server_failures_ = 0;
    int64_t server_retry_at_us_ = 0;   // 0 = no retry planned
    int64_t wifi_rejoin_since_us_ = 0; // 0 = not rejoining
};
