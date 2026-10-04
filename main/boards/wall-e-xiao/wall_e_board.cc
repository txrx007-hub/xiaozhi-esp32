#include "wall_e_board.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_sleep.h>

#include "application.h"
#include "config.h"
#include "walle_console.h"
#include "walle_mcp_tools.h"
#include "walle_settings.h"
#include "walle_sounds.h"
#include "walle_spectrum.h"
#include "walle_timers.h"
#include "walle_weather.h"
#include "walle_web.h"
#include "ssid_manager.h"
#include "wifi_manager.h"

#define TAG "WallEBoard"

namespace {

constexpr int kNightVolumeCap = 40;
constexpr int64_t kPendingTimeoutUs = 25LL * 1000 * 1000;  // nap/sleep even if no reply comes
constexpr int64_t kRingDurationUs = 60LL * 1000 * 1000;
constexpr int64_t kRingIntervalUs = 2500LL * 1000;

int64_t NowUs() { return esp_timer_get_time(); }

std::string MinutesText(int minutes) {
    if (minutes % 60 == 0) {
        const int hours = minutes / 60;
        return std::to_string(hours) + (hours == 1 ? " hour" : " hours");
    }
    if (minutes < 60) {
        return std::to_string(minutes) + (minutes == 1 ? " minute" : " minutes");
    }
    return std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60) + " min";
}

}  // namespace

WallEBoard::WallEBoard() : boot_button_(BOOT_BUTTON_GPIO) {
    // Keep the microSD card slot (it shares GPIO7/8/9 with the display) deselected. This also
    // turns the orange user LED off.
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << SD_CS_USER_LED_GPIO;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(SD_CS_USER_LED_GPIO, 1);

    // WALL-E: esp_sleep_get_wakeup_cause() (singular) is deprecated in IDF 6.1; the plural form
    // returns a bitmask of every cause instead of just the first one.
    const uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
    if (wakeup_causes & (1u << ESP_SLEEP_WAKEUP_EXT1)) {
        ESP_LOGI(TAG, "Woke from deep sleep: BOOT button");
    } else if (wakeup_causes & (1u << ESP_SLEEP_WAKEUP_TIMER)) {
        ESP_LOGI(TAG, "Woke from deep sleep: timer");
    }

    motors_ = new MotorDriver();  // IN1-IN4 were already driven low before app_main
    InitializeSpi();
    InitializeDisplay();
    InitializeCamera();
    InitializeButtons();

    ApplyWakeThreshold(false);  // AudioService keeps it until its engine exists

    WalleSettings::GetInstance().OnChanged([this](const std::string& key) {
        Application::GetInstance().Schedule([this, key]() { ApplySetting(key); });
    });
    WalleTimers::GetInstance().OnFire([this](const std::string& label) { StartRinging(label); });

    RegisterWalleTools(*this);
    InitializeIdleClock();

    esp_timer_create_args_t tick = {};
    tick.callback = [](void* arg) {
        auto* self = static_cast<WallEBoard*>(arg);
        Application::GetInstance().Schedule([self]() { self->Tick(); });
    };
    tick.arg = this;
    tick.name = "walle_tick";
    tick.skip_unhandled_events = true;
    ESP_ERROR_CHECK(esp_timer_create(&tick, &tick_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer_, 1000 * 1000));

    // After the chirp, voice processing starts; that is when the listening face appears.
    esp_timer_create_args_t poll = {};
    poll.callback = [](void* arg) {
        auto* self = static_cast<WallEBoard*>(arg);
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateListening) {
            esp_timer_stop(self->listen_poll_timer_);
            return;
        }
        if (app.GetAudioService().IsAudioProcessorRunning()) {
            esp_timer_stop(self->listen_poll_timer_);
            app.Schedule([self]() {
                if (Application::GetInstance().GetDeviceState() == kDeviceStateListening) {
                    self->display_->SetEmotion(WalleDisplay::kListeningEmotion);
                }
            });
        }
    };
    poll.arg = this;
    poll.name = "listen_face";
    ESP_ERROR_CHECK(esp_timer_create(&poll, &listen_poll_timer_));

    // Cold-start camera recovery: after a true power-up the OV3660 sometimes never delivers a
    // frame (the ESP32 supplies its clock and its PWDN/RESET pins are not wired, so the power-up
    // sequence can't be controlled), while any software restart brings it up fine. Probe once a
    // few seconds after boot; see CameraBootProbe().
    esp_timer_create_args_t probe = {};
    probe.callback = [](void* arg) {
        auto* self = static_cast<WallEBoard*>(arg);
        Application::GetInstance().Schedule([self]() { self->CameraBootProbe(); });
    };
    probe.arg = this;
    probe.name = "cam_probe";
    ESP_ERROR_CHECK(esp_timer_create(&probe, &camera_probe_timer_));
    ESP_ERROR_CHECK(esp_timer_start_once(camera_probe_timer_, 6 * 1000 * 1000));

    walle_console::Start();
    ESP_LOGI(TAG, "Jarvis board ready (wake word: %s)", WAKE_WORD_NAME);
}

void WallEBoard::InitializeSpi() {
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
    buscfg.miso_io_num = GPIO_NUM_NC;
    buscfg.sclk_io_num = DISPLAY_SCLK_PIN;
    buscfg.quadwp_io_num = GPIO_NUM_NC;
    buscfg.quadhd_io_num = GPIO_NUM_NC;
    buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
}

void WallEBoard::InitializeDisplay() {
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = DISPLAY_CS_PIN;
    io_config.dc_gpio_num = DISPLAY_DC_PIN;
    io_config.spi_mode = DISPLAY_SPI_MODE;
    io_config.pclk_hz = DISPLAY_SPI_CLOCK_HZ;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io));

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = DISPLAY_RST_PIN;
    panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
    esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

    display_ = new WalleDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X,
                                DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
                                DISPLAY_SWAP_XY);
}

void WallEBoard::InitializeCamera() {
    static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .data_io =
            {
                [0] = CAMERA_PIN_D0,
                [1] = CAMERA_PIN_D1,
                [2] = CAMERA_PIN_D2,
                [3] = CAMERA_PIN_D3,
                [4] = CAMERA_PIN_D4,
                [5] = CAMERA_PIN_D5,
                [6] = CAMERA_PIN_D6,
                [7] = CAMERA_PIN_D7,
            },
        .vsync_io = CAMERA_PIN_VSYNC,
        .de_io = CAMERA_PIN_HREF,
        .pclk_io = CAMERA_PIN_PCLK,
        .xclk_io = CAMERA_PIN_XCLK,
    };

    esp_video_init_sccb_config_t sccb_config = {
        .init_sccb = true,
        .i2c_config =
            {
                .port = 1,
                .scl_pin = CAMERA_PIN_SIOC,
                .sda_pin = CAMERA_PIN_SIOD,
            },
        .freq = 100000,
    };

    esp_video_init_dvp_config_t dvp_config = {
        .sccb_config = sccb_config,
        .reset_pin = CAMERA_PIN_RESET,
        .pwdn_pin = CAMERA_PIN_PWDN,
        .dvp_pin = dvp_pin_config,
        .xclk_freq = XCLK_FREQ_HZ,
    };

    esp_video_init_config_t video_config = {
        .dvp = &dvp_config,
    };

    camera_ = new WalleCamera(video_config);
    camera_->SetVFlip(CAMERA_VFLIP);
    camera_->SetHMirror(CAMERA_HMIRROR);
}

void WallEBoard::InitializeButtons() {
    boot_button_.OnClick([this]() {
        Application::GetInstance().Schedule([this]() {
            auto& app = Application::GetInstance();
            pending_nap_ = false;
            if (ringing_) {
                StopRinging();
                return;
            }
            if (napping_) {
                ExitNap();
                return;
            }
            if (idle_timer_ != nullptr) {
                idle_timer_->WakeUp();
            }
            const auto state = app.GetDeviceState();
            if (state == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (state == kDeviceStateSpeaking) {
                // Stop the reply at once (upstream only tells the server to stop).
                app.GetAudioService().ResetDecoder();
                app.AbortSpeaking(kAbortReasonNone);
                return;
            }
            app.ToggleChatState();
        });
    });
    boot_button_.OnLongPress([this]() {
        Application::GetInstance().Schedule([this]() { RequestNap("BOOT button"); });
    });
}

void WallEBoard::InitializeIdleClock() {
    if (idle_timer_ != nullptr) {
        idle_timer_->SetEnabled(false);
        delete idle_timer_;
        idle_timer_ = nullptr;
    }
    const int minutes = WalleSettings::GetInstance().GetInt("idle_clock_min");
    if (minutes <= 0) {
        display_->ShowIdleClock(false);
        return;
    }
    // Upstream's idle timer (cpu_max_freq = -1: no CPU or wake word changes, just timing).
    idle_timer_ = new PowerSaveTimer(-1, minutes * 60, -1);
    idle_timer_->OnEnterSleepMode([this]() {
        Application::GetInstance().Schedule([this]() {
            if (!napping_ && !ringing_) {
                display_->ShowIdleClock(true);
                WalleWeather::GetInstance().RefreshInBackground();
            }
        });
    });
    idle_timer_->OnExitSleepMode([this]() {
        Application::GetInstance().Schedule([this]() { display_->ShowIdleClock(false); });
    });
    idle_timer_->SetEnabled(true);
}

WalleAudioCodec* WallEBoard::walle_codec() {
    static WalleAudioCodec* codec = [this]() {
        auto* c = new WalleAudioCodec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                      AUDIO_SPK_BCLK, AUDIO_SPK_WS, AUDIO_SPK_DOUT,
                                      AUDIO_MIC_PDM_CLK, AUDIO_MIC_PDM_DATA);
        auto& settings = WalleSettings::GetInstance();
        c->SetMicGainDb(settings.GetInt("mic_gain_db"));
        c->SetVolumeCap(settings.GetInt("max_volume"));  // night mode is applied by Tick()
        display_->SetLevelSources([c]() { return c->output_rms(); },
                                  [c]() { return c->input_rms(); });
        display_->SetSpectrumSource([c]() { return c->TakeSpectrum(); });
        // visualizer_mode is an index into walle_spectrum::kModeNames (0 = off).
        const int mode = settings.GetInt("visualizer_mode");
        c->SetSpectrumEnabled(mode != 0);
        display_->SetVisualizerMode(static_cast<walle_spectrum::Mode>(mode));
        return c;
    }();
    return codec;
}

AudioCodec* WallEBoard::GetAudioCodec() { return walle_codec(); }

bool WallEBoard::OnWakeWordDetected(const std::string& wake_word) {
    if (pending_nap_ || (napping_ && NowUs() < nap_wake_guard_until_us_)) {
        // WALL-E: this board has no echo cancellation (no reference signal for the amp - see the
        // README's "interrupt by saying Jarvis" note, the other side of this same trade-off), and
        // the nap confirmation ("okay, going to nap now") reliably self-triggers WakeNet - heard
        // through the mic, not said by the user (confirmed live: every nap request logs "Wake word
        // detected" with the device still in the Speaking state) - either while still pending, or
        // for a few seconds after EnterNapNow() itself already ran (buffered/trailing playback).
        // That cancelled the pending nap or immediately exited an active one, played the chirp,
        // and popped back to the idle eyes right after it had just confirmed napping. A genuine
        // "wake up" this soon after asking to nap is also an unusual pattern anyway - waiting a
        // few seconds and saying the wake word normally still works fine - so detections in this
        // window are treated as self-echo instead of a real interrupt.
        ESP_LOGI(TAG, "Wake word ignored: nap self-echo guard active (likely self-echo, no AEC)");
        return true;
    }
    ESP_LOGI(TAG, "Wake word: %s", wake_word.c_str());
    pending_nap_ = false;
    StopRinging();
    ExitNap();
    if (idle_timer_ != nullptr) {
        idle_timer_->WakeUp();
    }
    display_->ShowIdleClock(false);
    if (WalleSettings::GetInstance().GetBool("wake_chirp") && !QuietHoursActive()) {
        Application::GetInstance().PlaySound(walle_sounds::WakeChirp());
    }
    return true;  // Jarvis handles the cue (chirp, or silence in night mode / chirp off)
}

void WallEBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    if (level != PowerSaveLevel::LOW_POWER) {
        if (idle_timer_ != nullptr) {
            idle_timer_->WakeUp();
        }
        ExitNap();
    }
    WifiBoard::SetPowerSaveLevel(level);
}

void WallEBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    WifiBoard::SetNetworkEventCallback(
        [this, callback](NetworkEvent event, const std::string& data) {
            if (event == NetworkEvent::WifiConfigModeEnter) {
                Application::GetInstance().Schedule([this]() {
                    wifi_setup_active_ = true;
                    wifi_setup_since_us_ = wifi_setup_client_us_ = NowUs();
                });
            } else if (event == NetworkEvent::WifiConfigModeExit ||
                       event == NetworkEvent::Connected) {
                Application::GetInstance().Schedule([this]() { wifi_setup_active_ = false; });
            }
            if (event == NetworkEvent::Connected) {
                network_connected_ = true;
                walle_web::Start();  // http://<ip>/ : settings page, same LAN only
            } else if (event == NetworkEvent::Disconnected ||
                       event == NetworkEvent::WifiConfigModeEnter) {
                network_connected_ = false;
                walle_web::Stop();
            }
            Application::GetInstance().Schedule([this]() { UpdateStatusDot(); });
            if (callback) {
                callback(event, data);
            }
        });
}

void WallEBoard::SetCpuMhz(int mhz) {
    esp_pm_config_t config = {};
    config.max_freq_mhz = mhz;
    config.min_freq_mhz = mhz;
    config.light_sleep_enable = false;  // the wake word needs the microphone running
    if (esp_err_t err = esp_pm_configure(&config); err != ESP_OK) {
        ESP_LOGW(TAG, "CPU clock change to %d MHz failed: %s", mhz, esp_err_to_name(err));
    }
}

void WallEBoard::RequestScreensaver() {
    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        display_->ShowIdleClock(true);
        WalleWeather::GetInstance().RefreshInBackground();
        return;
    }
    // The reply that confirms it is usually still playing, and afterwards the app re-enters
    // listening (continuous conversation) rather than idle - so without help the clock only
    // appeared ~100 s later, when the server finally closed the session. Tick() ends the
    // conversation once the reply is done; the idle state then shows the clock.
    screensaver_requested_ = true;
    screensaver_deadline_us_ = NowUs() + kPendingTimeoutUs;
    screensaver_spoke_ = Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
}

void WallEBoard::RequestNap(const char* reason) {
    ESP_LOGI(TAG, "Nap requested (%s)", reason);
    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        EnterNapNow();
        return;
    }
    pending_nap_ = true;
    pending_deadline_us_ = NowUs() + kPendingTimeoutUs;
    spoke_since_request_ = false;
}

void WallEBoard::EnterNapNow() {
    pending_nap_ = false;
    if (napping_) {
        return;
    }
    // WALL-E: forcing the listening turn to end early (Tick()'s app.StopListening() call, taken
    // when the reply is done but the app auto-re-entered listening) makes the server treat it as
    // an end-of-turn and occasionally send one more reply ("okay, I'm napping now...") - which
    // then played right after the screen had already gone blank for nap. AbortSpeaking() tells
    // the server to stop generating/sending anything further, so nap is actually silent.
    Application::GetInstance().AbortSpeaking(kAbortReasonNone);
    motors_->Stop();
    display_->ShowIdleClock(false);
    display_->SetBlank(true);
    SetCpuMhz(160);
    napping_ = true;
    // WALL-E: AbortSpeaking() stops the server sending more, but whatever was already buffered
    // for playback (or its acoustic decay in the room) can still reach the mic for a moment after
    // this point - confirmed live: self-triggering the wake word still happened here even with the
    // pending_nap_ guard in OnWakeWordDetected(), meaning it landed after that guard had already
    // cleared. A few seconds of ignoring the wake word right as nap begins covers that tail.
    constexpr int64_t kNapWakeGuardUs = 4LL * 1000 * 1000;
    nap_wake_guard_until_us_ = NowUs() + kNapWakeGuardUs;
    ESP_LOGI(TAG, "Napping: screen off, wake word and Wi-Fi on");
}

void WallEBoard::ExitNap() {
    if (!napping_) {
        return;
    }
    napping_ = false;
    SetCpuMhz(240);
    display_->SetBlank(false);
    display_->SetEmotion("neutral");
    if (idle_timer_ != nullptr) {
        idle_timer_->WakeUp();
    }
    ESP_LOGI(TAG, "Nap over");
}

void WallEBoard::RequestNapNow() {
    RequestNap("settings page");
    if (pending_nap_) {
        pending_deadline_us_ = NowUs() + 2LL * 1000 * 1000;
    }
}

std::expected<std::string, std::string> WallEBoard::RequestDeepSleepNow(int minutes) {
    auto result = RequestDeepSleep(minutes, "");
    if (result) {
        pending_deadline_us_ = NowUs() + 2LL * 1000 * 1000;
    }
    return result;
}

std::expected<std::string, std::string> WallEBoard::RequestDeepSleep(int minutes,
                                                                     const std::string& wake_at) {
    int64_t seconds = 0;
    std::string when;
    if (!wake_at.empty()) {
        int hours = -1;
        int mins = -1;
        char extra = 0;
        if (sscanf(wake_at.c_str(), "%d:%d%c", &hours, &mins, &extra) != 2 || hours < 0 ||
            hours > 23 || mins < 0 || mins > 59) {
            return std::unexpected("wake_at must be HH:MM in 24 hour time, like 07:00.");
        }
        if (!WalleTimers::ClockSynced()) {
            return std::unexpected(
                "I don't know the time yet, so I can't wake at a set time. Use duration_minutes.");
        }
        time_t now = time(nullptr);
        struct tm local;
        localtime_r(&now, &local);
        seconds = static_cast<int64_t>(hours * 60 + mins) * 60 -
                  (local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
        if (seconds <= 0) {
            seconds += 24 * 3600;
        }
        when = "until " + wake_at;
    } else if (minutes > 0) {
        seconds = static_cast<int64_t>(minutes) * 60;
        when = "for " + MinutesText(minutes);
    } else {
        when = "until the BOOT button is pressed";
    }

    std::string note;
    const int64_t next_timer = WalleTimers::GetInstance().SecondsUntilNext();
    if (next_timer >= 0 && (seconds == 0 || next_timer < seconds)) {
        seconds = std::max<int64_t>(next_timer - 20, 5);
        note = " I will wake up early for a timer.";
    }

    pending_deep_sleep_ = true;
    pending_deep_sleep_s_ = seconds;
    pending_deadline_us_ = NowUs() + kPendingTimeoutUs;
    spoke_since_request_ = false;
    ESP_LOGI(TAG, "Deep sleep requested: %s (%lld s)", when.c_str(),
             static_cast<long long>(seconds));
    return "Deep sleep starts after this reply, " + when + "." + note +
           " Wi-Fi and the wake word will be off; only the BOOT button" +
           (seconds > 0 ? " or the timer" : "") + " can wake Jarvis. Tell the user this now.";
}

void WallEBoard::EnterDeepSleepNow() {
    ESP_LOGW(TAG, "Entering deep sleep (%lld s timer)",
             static_cast<long long>(pending_deep_sleep_s_));
    motors_->Stop();
    MotorDriver::HoldLowForDeepSleep();
    display_->ShowIdleClock(false);
    display_->SetBlank(true);  // panel sleeps; the backlight is wired to 3V3 and stays lit

    // A held BOOT button would wake the chip at once.
    for (int i = 0; i < 30 && gpio_get_level(BOOT_BUTTON_GPIO) == 0; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    rtc_gpio_pullup_en(BOOT_BUTTON_GPIO);
    rtc_gpio_pulldown_dis(BOOT_BUTTON_GPIO);
    esp_sleep_enable_ext1_wakeup_io(1ULL << BOOT_BUTTON_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
    if (pending_deep_sleep_s_ > 0) {
        esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(pending_deep_sleep_s_) * 1000000ULL);
    }
    vTaskDelay(pdMS_TO_TICKS(200));  // let the log drain
    esp_deep_sleep_start();
}

bool WallEBoard::QuietHoursActive() const {
    auto& settings = WalleSettings::GetInstance();
    const int start = settings.GetInt("quiet_start");
    const int end = settings.GetInt("quiet_end");
    if (start < 0 || end < 0 || start == end || !WalleTimers::ClockSynced()) {
        return false;
    }
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    const int minute = local.tm_hour * 60 + local.tm_min;
    return start < end ? (minute >= start && minute < end) : (minute >= start || minute < end);
}

void WallEBoard::ApplyVolumeCap() {
    int cap = WalleSettings::GetInstance().GetInt("max_volume");
    if (QuietHoursActive()) {
        cap = std::min(cap, kNightVolumeCap);
    }
    if (cap != applied_volume_cap_) {
        walle_codec()->SetVolumeCap(cap);
        applied_volume_cap_ = cap;
    }
}

void WallEBoard::ApplyWakeThreshold(bool speaking) {
    auto& audio = Application::GetInstance().GetAudioService();
    if (speaking) {
        // While Jarvis talks it can hear itself (no echo cancellation): use the model's
        // own threshold so its voice does not wake it.
        audio.ResetWakeWordThreshold();
    } else {
        audio.SetWakeWordThreshold(WalleSettings::GetInstance().GetHundredths("wake_threshold"));
    }
}

void WallEBoard::ApplySetting(const std::string& key) {
    auto& settings = WalleSettings::GetInstance();
    if (key == "mic_gain_db") {
        walle_codec()->SetMicGainDb(settings.GetInt("mic_gain_db"));
    } else if (key == "wake_threshold") {
        ApplyWakeThreshold(state_ == kDeviceStateSpeaking);
    } else if (key == "max_volume" || key == "quiet_start" || key == "quiet_end") {
        applied_volume_cap_ = -1;
        ApplyVolumeCap();
    } else if (key == "idle_clock_min") {
        InitializeIdleClock();
    } else if (key == "weather_city") {
        WalleWeather::GetInstance().Forget();
        weather_line_.clear();
        display_->SetWeatherLine("");
        WalleWeather::GetInstance().RefreshInBackground(0);
    } else if (key == "visualizer_mode") {
        // index into walle_spectrum::kModeNames (0 = off).
        const int mode = settings.GetInt("visualizer_mode");
        walle_codec()->SetSpectrumEnabled(mode != 0);
        display_->SetVisualizerMode(static_cast<walle_spectrum::Mode>(mode));
    } else if (key == "log_level") {
        static const esp_log_level_t kLevels[] = {ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO,
                                                  ESP_LOG_DEBUG};
        esp_log_level_set("*", kLevels[std::clamp(settings.GetInt("log_level"), 0, 3)]);
    }
    // Motor settings, shutter_sound and wake_chirp are read at the moment they are used.
}

void WallEBoard::StartRinging(const std::string& label) {
    ESP_LOGI(TAG, "Alarm: %s", label.c_str());
    ringing_ = true;
    ring_until_us_ = NowUs() + kRingDurationUs;
    next_ring_us_ = 0;
    pending_nap_ = false;
    ExitNap();
    if (idle_timer_ != nullptr) {
        idle_timer_->WakeUp();
    }
    display_->ShowIdleClock(false);
    display_->ShowNotification("Timer: " + label, 60000);
    display_->HoldEmotion("surprised", 60000);
}

void WallEBoard::StopRinging() {
    if (!ringing_.exchange(false)) {
        return;
    }
    display_->ShowNotification("Alarm stopped", 1500);
    display_->HoldEmotion("happy", 800);
}

void WallEBoard::UpdateStatusDot() {
    WalleDisplay::Dot dot = WalleDisplay::Dot::kAmber;
    switch (state_) {
        case kDeviceStateIdle:
        case kDeviceStateListening:
        case kDeviceStateSpeaking:
        case kDeviceStateNotifying:
            dot = network_connected_ ? WalleDisplay::Dot::kGreen : WalleDisplay::Dot::kRed;
            break;
        case kDeviceStateFatalError:
            dot = WalleDisplay::Dot::kRed;
            break;
        default:  // starting, Wi-Fi setup, connecting, activating, upgrading
            break;
    }
    display_->SetStatusDot(dot);
}

void WallEBoard::OnDeviceStateChanged(DeviceState previous, DeviceState now) {
    state_ = now;

    if (now == kDeviceStateSpeaking) {
        ApplyWakeThreshold(true);
        spoke_since_request_ = true;
        screensaver_spoke_ = true;
    } else if (previous == kDeviceStateSpeaking) {
        ApplyWakeThreshold(false);
    }

    display_->SetRibbon(now == kDeviceStateSpeaking    ? WalleDisplay::Ribbon::kOutput
                        : now == kDeviceStateListening ? WalleDisplay::Ribbon::kInput
                                                       : WalleDisplay::Ribbon::kOff);

    esp_timer_stop(listen_poll_timer_);
    if (now == kDeviceStateListening) {
        esp_timer_start_periodic(listen_poll_timer_, 50 * 1000);
    }

    if (now != kDeviceStateIdle) {
        if (idle_timer_ != nullptr) {
            idle_timer_->WakeUp();
        }
        display_->ShowIdleClock(false);
    }

    if (now == kDeviceStateIdle) {
        if (!log_level_applied_) {
            log_level_applied_ = true;  // full log during boot, then the chosen level
            ESP_LOGI(TAG, "Jarvis is ready; log level now %s",
                     WalleSettings::GetInstance()
                         .FormatCurrent(*WalleSettings::GetInstance().Find("log_level"))
                         .c_str());
            ApplySetting("log_level");
        }
        if (screensaver_requested_) {
            screensaver_requested_ = false;
            display_->ShowIdleClock(true);
            WalleWeather::GetInstance().RefreshInBackground();
        }
        if (pending_nap_) {
            EnterNapNow();
        }
    }
    UpdateStatusDot();
}

void WallEBoard::CameraBootProbe() {
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool cold = reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT;
    if (camera_ == nullptr || camera_->ProbeFrame()) {
        ESP_LOGI(TAG, "Camera boot probe: frames OK (reset reason %d)", (int)reason);
        if (camera_ != nullptr) {
            camera_->EnableAutoWhiteBalance();  // frames are flowing now; see walle_camera.h
        }
        return;
    }
    if (!cold) {
        // Already a software/USB/watchdog restart: restarting again would not help and could loop.
        ESP_LOGE(TAG, "Camera boot probe: NO frames after a warm start (reset reason %d); "
                      "not rebooting - camera likely needs a hardware check", (int)reason);
        return;
    }
    ESP_LOGW(TAG, "Camera boot probe: no frames after a cold power-up (reset reason %d); "
                  "restarting once to recover the sensor", (int)reason);
    Application::GetInstance().Reboot();
}

void WallEBoard::CheckWifiSetupTimeout(int64_t now) {
    // Upstream keeps the setup hotspot open until a network is saved or Exit is tapped. Leave it
    // after 2 minutes with nobody connected to it, or 10 minutes in total, and go back to the
    // saved networks - the same call the setup page's Exit button makes (no reboot).
    constexpr int64_t kIdleUs = 120LL * 1000 * 1000;
    constexpr int64_t kMaxUs = 600LL * 1000 * 1000;
    if (!wifi_setup_active_) {
        return;
    }
    wifi_sta_list_t clients = {};
    if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0) {
        wifi_setup_client_us_ = now;
    }
    const bool idle = now - wifi_setup_client_us_ >= kIdleUs;
    const bool too_long = now - wifi_setup_since_us_ >= kMaxUs;
    if (!idle && !too_long) {
        return;
    }
    if (SsidManager::GetInstance().GetSsidList().empty()) {
        return;  // nothing to go back to
    }
    ESP_LOGI(TAG, "WiFi setup %s; returning to the saved networks",
             idle ? "idle for 2 minutes" : "open for 10 minutes");
    wifi_setup_active_ = false;
    WifiManager::GetInstance().StopConfigAp();
}

void WallEBoard::Tick() {
    WalleTimers::GetInstance().Tick();
    ApplyVolumeCap();
    const int64_t now = NowUs();
    CheckWifiSetupTimeout(now);

    if (ringing_) {
        if (now > ring_until_us_) {
            StopRinging();
        } else if (now >= next_ring_us_) {
            Application::GetInstance().PlaySound(walle_sounds::Alarm());
            next_ring_us_ = now + kRingIntervalUs;
        }
    }

    if (display_->IsIdleClockShown()) {
        display_->RefreshIdleClock();
        WalleWeather::GetInstance().RefreshInBackground();
        const std::string line = WalleWeather::GetInstance().ClockLine();
        if (line != weather_line_) {
            weather_line_ = line;
            display_->SetWeatherLine(line);
        }
    }

    // Screensaver requested by voice while talking: same wait, then end the conversation.
    if (screensaver_requested_ && state_ == kDeviceStateListening && !pending_nap_ &&
        !pending_deep_sleep_) {
        auto& app = Application::GetInstance();
        const bool reply_done = screensaver_spoke_ && app.GetAudioService().IsPlaybackIdle();
        if (reply_done || now > screensaver_deadline_us_) {
            app.AbortSpeaking(kAbortReasonNone);  // no follow-up reply over the clock
            app.StopListening();                  // -> idle, which shows the clock
        }
    }

    // Nap / deep sleep requested by voice: wait until the reply has been spoken.
    if (pending_deep_sleep_ || pending_nap_) {
        auto& app = Application::GetInstance();
        const bool reply_done = spoke_since_request_ && state_ != kDeviceStateSpeaking &&
                                app.GetAudioService().IsPlaybackIdle();
        const bool timed_out = now > pending_deadline_us_;
        if (pending_deep_sleep_ && (reply_done || timed_out)) {
            pending_deep_sleep_ = false;
            EnterDeepSleepNow();
        } else if (pending_nap_ && (reply_done || timed_out)) {
            if (state_ == kDeviceStateIdle) {
                EnterNapNow();
            } else if (state_ == kDeviceStateListening) {
                app.StopListening();  // back to idle; the idle state starts the nap
            }
        }
    }
}

DECLARE_BOARD(WallEBoard);
