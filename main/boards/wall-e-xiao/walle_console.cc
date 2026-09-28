#include "walle_console.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "settings.h"
#include "ssid_manager.h"
#include "system_info.h"
#include "wall_e_board.h"
#include "walle_diagnostics.h"
#include "walle_settings.h"
#include "walle_sounds.h"
#include "walle_timers.h"
#include "walle_weather.h"
#include "walle_web.h"
#include "wifi_manager.h"

#define TAG "WalleConsole"

namespace walle_console {

namespace {

constexpr const char* kHelp =
    "Jarvis console\n"
    "  !status                 firmware, Wi-Fi, server, memory, camera, tuning\n"
    "  !server IP|URL|default  set the OTA/server URL (IP -> http://IP:8003/xiaozhi/ota/), reboots\n"
    "  !wifi SSID PASSWORD     add a saved Wi-Fi network   (!wifi list, !wifi clear)\n"
    "  !camera                 capture one camera frame\n"
    "  !mic status|gain N|mute|unmute|meter [s]   (gain in dB, 0-24)\n"
    "  !speaker                play a test tone   (!speaker vol N, !speaker status)\n"
    "  !stop                   stop motors, speech, listening and alarms\n"
    "  !motors                 short forward/back pulse on each motor (lift the wheels!)\n"
    "  !face EMOTION [ms]      show an eye expression\n"
    "  !settings               list   (!settings set KEY VALUE, !settings reset)\n"
    "  !standby | !nap         nap now (blank screen, wake word on)   !wake to end it\n"
    "  !sleep [MINUTES]        deep sleep until BOOT (or the timer)\n"
    "  !diag CHECK             wifi, server, memory, camera, mic, speaker, display, motors, all\n"
    "  !timers | !weather | !pattern [s] | !reboot | !help\n";

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

// Everything after the first n words, spaces preserved (Wi-Fi passwords, setting values).
std::string Rest(const std::string& line, int n) {
    size_t pos = 0;
    for (int word = 0; word < n; ++word) {
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
        while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
    }
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
    return line.substr(pos);
}

void Print(const std::string& text) {
    printf("%s\n", text.c_str());
    fflush(stdout);
}

std::string OtaUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    return url.empty() ? std::string(CONFIG_OTA_URL) + " (default)" : url;
}

void RebootSoon() {
    Print("Rebooting...");
    Application::GetInstance().Schedule([]() {
        vTaskDelay(pdMS_TO_TICKS(500));
        Application::GetInstance().Reboot();
    });
}

void MicMeter(int seconds) {
    static esp_timer_handle_t timer = nullptr;
    static int64_t until_us = 0;
    if (timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) {
            auto level = WallEBoard::Get().walle_codec()->TakeLevel();
            const int bars = level.rms_dbfs <= -70 ? 0 : static_cast<int>((level.rms_dbfs + 70) / 2.5f);
            char line[80];
            int n = snprintf(line, sizeof(line), "mic %5.0f dB  peak %5.0f  |", level.rms_dbfs,
                             level.peak_dbfs);
            for (int i = 0; i < 28 && n < 78; ++i) {
                line[n++] = i < bars ? '#' : '.';
            }
            line[n] = 0;
            printf("%s|%s\n", line, level.clipped ? " CLIP" : "");
            if (esp_timer_get_time() > until_us) {
                esp_timer_stop(timer);
                printf("meter done\n");
            }
        };
        args.name = "mic_meter";
        esp_timer_create(&args, &timer);
    }
    esp_timer_stop(timer);
    WallEBoard::Get().walle_codec()->TakeLevel();
    until_us = esp_timer_get_time() + static_cast<int64_t>(seconds) * 1000000;
    esp_timer_start_periodic(timer, 250 * 1000);
}

std::string StatusText() {
    auto& board = WallEBoard::Get();
    auto& app = Application::GetInstance();
    auto& wifi = WifiManager::GetInstance();
    auto& settings = WalleSettings::GetInstance();
    auto* codec = board.walle_codec();
    char buf[900];
    snprintf(buf, sizeof(buf),
             "firmware %s | board wall-e-xiao | state %d%s%s\n"
             "wifi: %s %s rssi %d ip %s\n"
             "settings page: %s\n"
             "server: %s\n"
             "memory: internal %u KB free (lowest %u KB), psram %u KB free\n"
             "camera: %s | motors: %s\n"
             "mic gain %d dB%s | wake threshold %s | volume %d (cap %d)\n"
             "idle clock after %d min | quiet hours %s-%s%s | timers: %s",
             SystemInfo::GetUserAgent().c_str(), static_cast<int>(app.GetDeviceState()),
             board.IsNapping() ? " (napping)" : "", board.IsRinging() ? " (alarm ringing)" : "",
             wifi.IsConnected() ? "connected to" : "not connected", wifi.GetSsid().c_str(),
             wifi.GetRssi(), wifi.GetIpAddress().c_str(),
             *walle_web::Url() ? walle_web::Url() : "not running", OtaUrl().c_str(),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             board.walle_camera() ? board.walle_camera()->LastFrameInfo().c_str() : "none",
             board.motors().Status().c_str(), codec->mic_gain_db(),
             codec->mic_muted() ? " (muted)" : "",
             settings.FormatCurrent(*settings.Find("wake_threshold")).c_str(),
             codec->output_volume(), codec->volume_cap(), settings.GetInt("idle_clock_min"),
             settings.FormatCurrent(*settings.Find("quiet_start")).c_str(),
             settings.FormatCurrent(*settings.Find("quiet_end")).c_str(),
             board.QuietHoursActive() ? " (active)" : "", WalleTimers::GetInstance().List().c_str());
    return buf;
}

void Execute(const std::string& line) {
    auto args = Split(line);
    if (args.empty()) {
        return;
    }
    auto& board = WallEBoard::Get();
    auto& app = Application::GetInstance();
    auto& settings = WalleSettings::GetInstance();
    const std::string& cmd = args[0];
    auto arg = [&](size_t i) { return i < args.size() ? args[i] : std::string(); };

    if (cmd == "!help") {
        Print(kHelp);
    } else if (cmd == "!status") {
        Print(StatusText());
    } else if (cmd == "!server") {
        if (args.size() < 2) {
            Print("server: " + OtaUrl());
            return;
        }
        Settings wifi_settings("wifi", true);
        if (arg(1) == "default") {
            wifi_settings.EraseKey("ota_url");
        } else {
            std::string url = arg(1);
            if (url.find("://") == std::string::npos) {
                url = "http://" + url + ":8003/xiaozhi/ota/";
            }
            wifi_settings.SetString("ota_url", url);
        }
        Print("server set to " + OtaUrl());
        RebootSoon();
    } else if (cmd == "!wifi") {
        auto& ssids = SsidManager::GetInstance();
        if (arg(1) == "list") {
            const auto& list = ssids.GetSsidList();
            if (list.empty()) {
                Print("no saved Wi-Fi networks");
            }
            for (size_t i = 0; i < list.size(); ++i) {
                Print(std::to_string(i) + ": " + list[i].ssid);
            }
        } else if (arg(1) == "clear") {
            ssids.Clear();
            Print("saved Wi-Fi networks cleared");
        } else if (args.size() >= 2) {
            ssids.AddSsid(arg(1), Rest(line, 2));
            Print("saved " + arg(1) + "; use !reboot to connect with it");
        } else {
            Print("usage: !wifi SSID PASSWORD | !wifi list | !wifi clear");
        }
    } else if (cmd == "!camera") {
        Print(walle_diagnostics::Run("camera"));
    } else if (cmd == "!mic") {
        auto* codec = board.walle_codec();
        if (arg(1) == "gain" && args.size() >= 3) {
            auto result = settings.Set("mic_gain_db", arg(2));
            Print(result ? *result : result.error());
        } else if (arg(1) == "mute") {
            codec->SetMicMuted(true);
            Print("microphone muted");
        } else if (arg(1) == "unmute") {
            codec->SetMicMuted(false);
            Print("microphone unmuted");
        } else if (arg(1) == "meter") {
            int seconds = args.size() >= 3 ? std::atoi(arg(2).c_str()) : 10;
            MicMeter(seconds < 1 ? 1 : (seconds > 120 ? 120 : seconds));
        } else {
            Print("mic gain " + std::to_string(codec->mic_gain_db()) + " dB" +
                  (codec->mic_muted() ? ", muted" : ", on") +
                  ". Try !mic meter while talking.");
        }
    } else if (cmd == "!speaker") {
        auto* codec = board.walle_codec();
        if (arg(1) == "vol" && args.size() >= 3) {
            codec->SetOutputVolume(std::atoi(arg(2).c_str()));
            Print("volume " + std::to_string(codec->output_volume()) + " (requested " +
                  std::to_string(codec->requested_volume()) + ", cap " +
                  std::to_string(codec->volume_cap()) + ")");
        } else if (arg(1) == "status") {
            Print("volume " + std::to_string(codec->output_volume()) + ", cap " +
                  std::to_string(codec->volume_cap()) + ", output " +
                  (codec->output_enabled() ? "on" : "off"));
        } else {
            app.PlaySound(walle_sounds::Tone());
            Print("playing test tone");
        }
    } else if (cmd == "!stop") {
        board.motors().Stop();
        board.StopRinging();
        auto state = app.GetDeviceState();
        if (state == kDeviceStateSpeaking) {
            app.GetAudioService().ResetDecoder();
            app.AbortSpeaking(kAbortReasonNone);
        } else if (state == kDeviceStateListening) {
            app.StopListening();
        }
        Print("stopped");
    } else if (cmd == "!motors") {
        Print(walle_diagnostics::Run("motors"));
    } else if (cmd == "!face") {
        int ms = args.size() >= 3 ? std::atoi(arg(2).c_str()) : 3000;
        board.walle_display()->HoldEmotion(arg(1).empty() ? "neutral" : arg(1), ms);
        Print("face " + (arg(1).empty() ? std::string("neutral") : arg(1)));
    } else if (cmd == "!settings") {
        if (arg(1) == "set" && args.size() >= 4) {
            auto result = settings.Set(arg(2), Rest(line, 3));
            Print(result ? *result : result.error());
        } else if (arg(1) == "reset") {
            settings.ResetAll();
            Print("all settings reset to defaults");
        } else {
            Print(settings.ListText());
        }
    } else if (cmd == "!standby" || cmd == "!nap") {
        board.RequestNap("console");
        Print("napping (wake word and Wi-Fi stay on); BOOT, the wake word or !wake ends it");
    } else if (cmd == "!wake") {
        board.ExitNap();
        Print("awake");
    } else if (cmd == "!sleep") {
        auto result = board.RequestDeepSleep(args.size() >= 2 ? std::atoi(arg(1).c_str()) : 0, "");
        Print(result ? *result : result.error());
        if (result) {
            board.EnterDeepSleepNow();
        }
    } else if (cmd == "!diag") {
        Print(walle_diagnostics::Run(arg(1).empty() ? "all" : arg(1)));
    } else if (cmd == "!timers") {
        Print(WalleTimers::GetInstance().List());
    } else if (cmd == "!weather") {
        auto report = WalleWeather::GetInstance().Report();
        Print(report ? *report : report.error());
    } else if (cmd == "!pattern") {
        int seconds = args.size() >= 2 ? std::atoi(arg(1).c_str()) : 15;
        board.walle_display()->ShowTestPattern((seconds < 1 ? 15 : seconds) * 1000);
        Print("test pattern: RED must be top-left, GREEN top-right, BLUE bottom-left, "
              "WHITE bottom-right, '^ UP ^' at the top, a thin white frame on all 4 edges");
    } else if (cmd == "!reboot") {
        RebootSoon();
    } else {
        Print("unknown command " + cmd + "; try !help");
    }
}

void ReaderTask(void*) {
    std::string line;
    char buf[64];
    while (true) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        for (int i = 0; i < n; ++i) {
            const char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (!line.empty() && line[0] == '!') {
                    std::string command = line;
                    Application::GetInstance().Schedule([command]() { Execute(command); });
                }
                line.clear();
            } else if (line.size() < 200) {
                line += c;
            }
        }
    }
}

}  // namespace

void Start() {
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {};
        config.rx_buffer_size = 256;
        config.tx_buffer_size = 1024;
        if (usb_serial_jtag_driver_install(&config) != ESP_OK) {
            ESP_LOGE(TAG, "USB serial driver install failed; console disabled");
            return;
        }
    }
    usb_serial_jtag_vfs_use_driver();  // logs and printf share the driver with the reader
    xTaskCreate(ReaderTask, "console", 3072, nullptr, 2, nullptr);
    ESP_LOGI(TAG, "USB console ready; type !help");
}

}  // namespace walle_console
