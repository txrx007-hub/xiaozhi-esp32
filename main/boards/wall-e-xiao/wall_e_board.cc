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
#include "walle_health.h"
#include "walle_console.h"
#include "walle_mcp_tools.h"
#include "walle_server.h"
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
constexpr int64_t kQrPageUs = 10LL * 1000 * 1000;   // each QR code stays this long before the next
constexpr int64_t kQrTotalUs = 60LL * 1000 * 1000;  // then the screen goes back to normal by itself

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
    walle_health::Init();   // chip temperature and Wi-Fi disconnect counter for /api/status
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

    // static, like dvp_pin_config: CameraBootProbe() restarts the camera with the same config later (EspVideo::Restart)
    static esp_video_init_sccb_config_t sccb_config = {
        .init_sccb = true,
        .i2c_config =
            {
                .port = 1,
                .scl_pin = CAMERA_PIN_SIOC,
                .sda_pin = CAMERA_PIN_SIOD,
            },
        .freq = 100000,
    };

    static esp_video_init_dvp_config_t dvp_config = {
        .sccb_config = sccb_config,
        .reset_pin = CAMERA_PIN_RESET,
        .pwdn_pin = CAMERA_PIN_PWDN,
        .dvp_pin = dvp_pin_config,
        .xclk_freq = XCLK_FREQ_HZ,
    };

    static esp_video_init_config_t video_config = {
        .dvp = &dvp_config,
    };
    camera_config_ = &video_config;

    camera_ = new WalleCamera(video_config);
    ApplyCameraSettings();
}

void WallEBoard::ApplyCameraSettings() {
    camera_->SetVFlip(CAMERA_VFLIP);
    camera_->SetHMirror(CAMERA_HMIRROR);
    camera_->SetAeLevel(WalleSettings::GetInstance().GetInt("camera_brightness"));
    if (const int saturation = WalleSettings::GetInstance().GetInt("camera_saturation"); saturation != 0) {
        camera_->SetSaturation(saturation);   // 0 is what the sensor starts with: nothing to write
    }
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
        // no volume cap: 100% is 100%; only night mode (quiet hours) limits it, applied by Tick()
        display_->SetLevelSources([c]() { return c->output_rms(); },
                                  [c]() { return c->input_rms(); });
        display_->SetSpectrumSource([c]() { return c->TakeSpectrum(); });
        // visualizer_mode is an index into walle_spectrum::kModeNames (0 = off).
        const int mode = settings.GetInt("visualizer_mode");
        c->SetSpectrumEnabled(mode != 0);
        display_->SetVisualizerMode(static_cast<walle_spectrum::Mode>(mode));
        display_->SetMusicIcon(settings.GetInt("music_icon"));
        // Music needs the spectrum for its Winamp bars even when the speaking visualizer is off.
        display_->SetMusicHook([c](bool on) {
            c->SetSpectrumEnabled(on || WalleSettings::GetInstance().GetInt("visualizer_mode") != 0);
        });
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

std::expected<std::string, std::string> WallEBoard::RequestQr(const std::string& target_in) {
    const std::string target = target_in.empty() ? "both" : target_in;
    if (target != "settings" && target != "backend" && target != "both") {
        return std::unexpected("target must be settings, backend or both");
    }
    std::vector<WalleDisplay::QrPage> pages;
    std::vector<int> kinds;  // 0 = settings page, 1 = backend; addresses are re-read each time a code is shown
    std::string note;
    if (target != "backend") {
        if (*walle_web::Url() != 0) {  // the page's web server is running (so Wi-Fi is up)
            pages.push_back({"Jarvis settings page", ""});
            kinds.push_back(0);
        } else if (target == "settings") {
            return std::unexpected("The settings page is not running (no Wi-Fi), so there is nothing to show.");
        }
    }
    if (target != "settings") {
        const std::string panel_url = walle_server::PanelUrl();
        if (!panel_url.empty()) {
            pages.push_back({"Jarvis backend", panel_url});
            kinds.push_back(1);
            // The backend address comes from the saved server address. If the PC got a new address from the
            // router, the saved one is stale: check that something answers there right now and warn if not.
            std::string detail;
            if (!walle_server::Reachable(2000, &detail)) {
                note += " WARNING: " + detail + " just now, so the backend code may show an outdated address. "
                        "Tell the user that the PC may have a new IP address and that the server address can be "
                        "set again on the settings page under Voice server, or with the USB command !server.";
            }
        } else if (target == "backend") {
            return std::unexpected(
                "Jarvis is on the cloud server, not on the user's own server, so there is no backend page to show.");
        } else {
            note += " The backend code is left out because Jarvis is not using the user's own server.";
        }
    }
    if (pages.empty()) {
        return std::unexpected("There is nothing to show right now.");
    }
    qr_pages_ = std::move(pages);
    qr_kinds_ = std::move(kinds);
    RefreshQrUrls();

    std::string what;
    for (size_t i = 0; i < qr_pages_.size(); ++i) {
        what += (i ? ", then " : "") + qr_pages_[i].title;
    }
    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle && !napping_) {
        StartQr();
    } else {
        // Mid-conversation: wait for the spoken reply to finish, like the screensaver (see Tick()).
        qr_requested_ = true;
        screensaver_deadline_us_ = NowUs() + kPendingTimeoutUs;
        screensaver_spoke_ = Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
    }
    return "QR codes will appear on the screen as soon as this reply is finished: " + what +
           ". Each stays 10 seconds, all gone after a minute or when the user says the wake word. "
           "Tell the user to point the phone camera at the screen (phone on the same Wi-Fi). The backend "
           "page asks for its PIN once." + note;
}

// The addresses are read again every time a code is (re)shown, so a new address from the router's DHCP is
// picked up even while the codes are on screen: the settings page is this robot's current IP, the backend
// is the host of the saved server address.
void WallEBoard::RefreshQrUrls() {
    for (size_t i = 0; i < qr_pages_.size() && i < qr_kinds_.size(); ++i) {
        if (qr_kinds_[i] == 0) {
            auto& wifi = WifiManager::GetInstance();
            if (wifi.IsConnected() && !wifi.GetIpAddress().empty()) {
                qr_pages_[i].url = "http://" + wifi.GetIpAddress() + "/";
            } else if (*walle_web::Url() != 0) {
                qr_pages_[i].url = walle_web::Url();
            }
        } else {
            const std::string panel_url = walle_server::PanelUrl();
            if (!panel_url.empty()) {
                qr_pages_[i].url = panel_url;
            }
        }
    }
}

void WallEBoard::StartQr() {
    qr_requested_ = false;
    if (qr_pages_.empty()) {
        return;
    }
    RefreshQrUrls();
    qr_start_us_ = NowUs();
    qr_index_ = 0;
    display_->ShowQr(qr_pages_[0], 0, static_cast<int>(qr_pages_.size()));
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
    int cap = 100;  // no user-set cap any more
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
    } else if (key == "quiet_start" || key == "quiet_end") {
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
    } else if (key == "music_icon") {
        display_->SetMusicIcon(settings.GetInt("music_icon"));
    } else if (key == "camera_brightness") {
        if (camera_ != nullptr) {
            camera_->SetAeLevel(settings.GetInt("camera_brightness"));
        }
    } else if (key == "camera_saturation") {
        if (camera_ != nullptr) {
            camera_->SetSaturation(settings.GetInt("camera_saturation"));
        }
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

    if (previous == kDeviceStateConnecting) {
        if (now == kDeviceStateIdle) {
            OnServerConnectFailed();   // the application went back to idle: the server was not reached
        } else {
            if (server_failures_ > 0) {
                ESP_LOGI(TAG, "Server reached again (after %d failed attempts)", server_failures_);
            }
            server_failures_ = 0;
            server_retry_at_us_ = 0;
        }
    }

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
        if (qr_requested_) {
            StartQr();
        }
        if (pending_nap_) {
            EnterNapNow();
        }
    }
    UpdateStatusDot();
}

bool WallEBoard::RestartCameraInPlace(int attempt) {
    if (camera_ == nullptr || camera_config_ == nullptr) {
        return false;
    }
    if (!camera_->Restart(*camera_config_, attempt * 200)) {   // a longer pause on the second try
        ESP_LOGE(TAG, "Camera restart in place: the camera did not start again (attempt %d)", attempt);
        return false;
    }
    ApplyCameraSettings();
    if (!camera_->ProbeFrame()) {
        ESP_LOGW(TAG, "Camera restart in place: started, but no frames (attempt %d)", attempt);
        return false;
    }
    camera_->EnableAutoWhiteBalance();   // the restart reloaded the sensor's defaults; see walle_camera.h
    ESP_LOGI(TAG, "Camera restart in place: frames OK (attempt %d)", attempt);
    return true;
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
    // First restart only the camera, in place: shut esp_video down, leave the sensor without its clock for a moment, bring it up again.
    // That is what the reboot below used to achieve (the sensor has been powered and settled for seconds by now, unlike at the very
    // first start), without the robot booting twice. A pointer to the camera (the photo tool keeps one) stays valid.
    for (int attempt = 1; attempt <= 2; ++attempt) {
        ESP_LOGW(TAG, "Camera boot probe: no frames (reset reason %d); restarting the camera in place (attempt %d)", (int)reason, attempt);
        if (RestartCameraInPlace(attempt)) {
            ESP_LOGI(TAG, "Camera boot probe: frames OK after restarting the camera in place - no reboot needed");
            return;
        }
    }
    if (!cold) {
        // Already a software/USB/watchdog restart: restarting again would not help and could loop.
        ESP_LOGE(TAG, "Camera boot probe: NO frames after a warm start (reset reason %d), also not after restarting the camera; "
                      "not rebooting - camera likely needs a hardware check", (int)reason);
        return;
    }
    ESP_LOGW(TAG, "Camera boot probe: still no frames after a cold power-up (reset reason %d); "
                  "rebooting once as the last resort", (int)reason);
    Application::GetInstance().Reboot();
}

void WallEBoard::OnServerConnectFailed() {
    constexpr int kPlainRetries = 2;
    constexpr int64_t kRetryDelayUs = 5LL * 1000 * 1000;
    if (!network_connected_ || napping_ || pending_nap_ || pending_deep_sleep_) {
        // no Wi-Fi at all (the station reconnects by itself), or on purpose going quiet: nothing to retry
        server_failures_ = 0;
        server_retry_at_us_ = 0;
        return;
    }
    ++server_failures_;
    if (server_failures_ <= kPlainRetries) {
        ESP_LOGW(TAG, "Server not reached (attempt %d); trying again in 5 s", server_failures_);
        server_retry_at_us_ = NowUs() + kRetryDelayUs;
    } else if (server_failures_ == kPlainRetries + 1) {
        // Wi-Fi says connected but nothing comes back: drop the link; the station joins again by itself
        // (WifiStation reconnects on STA_DISCONNECTED), and CheckServerRetry tries once more when it is back.
        ESP_LOGW(TAG, "Server still not reached after %d attempts; Wi-Fi looks half dead - joining it again",
                 server_failures_);
        server_retry_at_us_ = 0;
        wifi_rejoin_since_us_ = NowUs();
        esp_wifi_disconnect();
    } else {
        ESP_LOGW(TAG, "Server not reached, also after joining Wi-Fi again; waiting for the next wake word");
        server_failures_ = 0;
        server_retry_at_us_ = 0;
    }
}

void WallEBoard::CheckServerRetry(int64_t now) {
    constexpr int64_t kSettleUs = 3LL * 1000 * 1000;    // after the rejoin: let DHCP and ARP settle
    constexpr int64_t kRejoinMaxUs = 45LL * 1000 * 1000;
    if (wifi_rejoin_since_us_ != 0) {
        if (network_connected_ && WifiManager::GetInstance().IsConnected() && now - wifi_rejoin_since_us_ > kSettleUs) {
            ESP_LOGI(TAG, "Wi-Fi joined again; trying the server once more");
            wifi_rejoin_since_us_ = 0;
            server_retry_at_us_ = now + 2LL * 1000 * 1000;
        } else if (now - wifi_rejoin_since_us_ > kRejoinMaxUs) {
            ESP_LOGW(TAG, "Wi-Fi not back within 45 s; leaving it to the normal reconnect");
            wifi_rejoin_since_us_ = 0;
            server_failures_ = 0;
        }
    }
    if (server_retry_at_us_ == 0 || now < server_retry_at_us_) {
        return;
    }
    server_retry_at_us_ = 0;
    if (state_ != kDeviceStateIdle || napping_ || pending_nap_ || pending_deep_sleep_ || ringing_) {
        server_failures_ = 0;   // something else is going on (a new wake word, a nap): no retry over it
        return;
    }
    ESP_LOGI(TAG, "Trying the server again (attempt %d)", server_failures_ + 1);
    Application::GetInstance().ToggleChatState();   // idle -> connecting -> listening, like a button press
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
    CheckServerRetry(now);

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

    // QR pages: flip between the codes, and clear the screen after the total time.
    if (display_->IsQrShown() && qr_pages_.size() > 0) {
        const int64_t elapsed = now - qr_start_us_;
        if (elapsed >= kQrTotalUs) {
            display_->HideQr();
            qr_index_ = -1;
        } else {
            const int index = static_cast<int>((elapsed / kQrPageUs) % static_cast<int64_t>(qr_pages_.size()));
            if (index != qr_index_) {
                qr_index_ = index;
                RefreshQrUrls();
                display_->ShowQr(qr_pages_[index], index, static_cast<int>(qr_pages_.size()));
            }
        }
    }

    // Screensaver or QR pages requested by voice while talking: same wait, then end the conversation.
    if ((screensaver_requested_ || qr_requested_) && state_ == kDeviceStateListening && !pending_nap_ &&
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
