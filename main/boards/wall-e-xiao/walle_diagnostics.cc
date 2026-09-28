#include "walle_diagnostics.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <unistd.h>

#include "application.h"
#include "settings.h"
#include "wall_e_board.h"
#include "walle_settings.h"
#include "walle_sounds.h"
#include "wifi_manager.h"

#define TAG "WalleDiag"

namespace walle_diagnostics {

namespace {

std::string Fmt(const char* format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

const char* ResetReasonText(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power on";
        case ESP_RST_SW: return "software restart";
        case ESP_RST_PANIC: return "a crash";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT: return "a watchdog";
        case ESP_RST_DEEPSLEEP: return "waking from deep sleep";
        case ESP_RST_BROWNOUT: return "a brownout (the battery voltage dipped)";
        case ESP_RST_USB: return "USB reset";
        default: return "another cause";
    }
}

std::string ServerHost() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    size_t start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    size_t end = url.find_first_of(":/", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string CheckWifi() {
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsConnected()) {
        return "Wi-Fi is not connected.";
    }
    const int rssi = wifi.GetRssi();
    const char* quality = rssi > -60 ? "good" : (rssi > -70 ? "fair" : "weak");
    return Fmt("Wi-Fi is connected to %s with a %s signal of %d dBm.", wifi.GetSsid().c_str(),
               quality, rssi);
}

std::string CheckServer() {
    const std::string host = ServerHost();
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = nullptr;
    const int64_t start = esp_timer_get_time();
    if (getaddrinfo(host.c_str(), "443", &hints, &result) != 0 || result == nullptr) {
        return "I could not look up the server " + host + ".";
    }
    int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    struct timeval timeout = {3, 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    const bool ok = sock >= 0 && connect(sock, result->ai_addr, result->ai_addrlen) == 0;
    const int ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
    if (sock >= 0) {
        close(sock);
    }
    freeaddrinfo(result);
    if (!ok) {
        return "The server " + host + " did not answer.";
    }
    return Fmt("The server %s answered in %d milliseconds.", host.c_str(), ms);
}

std::string CheckMemory() {
    const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const int64_t up_s = esp_timer_get_time() / 1000000;
    return Fmt("Free memory: %u kilobytes internal (lowest %u), %.1f megabytes PSRAM. Up for "
               "%d hours %d minutes. Last start: %s.",
               static_cast<unsigned>(internal / 1024), static_cast<unsigned>(internal_min / 1024),
               psram / 1048576.0, static_cast<int>(up_s / 3600),
               static_cast<int>((up_s / 60) % 60), ResetReasonText(esp_reset_reason()));
}

std::string CheckCamera() {
    auto* camera = WallEBoard::Get().walle_camera();
    if (camera == nullptr) {
        return "There is no camera.";
    }
    if (!camera->Capture()) {
        return "The camera did not take a picture.";
    }
    return "The camera took a " + camera->LastFrameInfo() + " picture.";
}

std::string CheckMic() {
    auto* codec = WallEBoard::Get().walle_codec();
    codec->TakeLevel();
    vTaskDelay(pdMS_TO_TICKS(2000));
    auto level = codec->TakeLevel();
    if (level.samples == 0) {
        return "The microphone is not running right now.";
    }
    std::string text = Fmt("Microphone over 2 seconds: loudest %.0f dB, average %.0f dB, gain %d "
                           "dB",
                           level.peak_dbfs, level.rms_dbfs, codec->mic_gain_db());
    if (codec->mic_muted()) {
        text += ", muted";
    }
    if (level.clipped > 0) {
        text += Fmt(", %d clipped samples, so the gain may be too high", level.clipped);
    }
    return text + ".";
}

std::string CheckSpeaker() {
    auto* codec = WallEBoard::Get().walle_codec();
    Application::GetInstance().PlaySound(walle_sounds::Tone());
    return Fmt("Playing a test tone now at volume %d (cap %d).", codec->output_volume(),
               codec->volume_cap());
}

std::string CheckDisplay() {
    WallEBoard::Get().walle_display()->FlashColors();
    return "Flashing red, green, blue and white on the screen now.";
}

std::string CheckMotors() {
    // Pulse each motor forward and back at low power, after a pause so the warning is heard.
    static esp_timer_handle_t timer = nullptr;
    if (timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) {
            std::vector<MotorDriver::Step> steps = {
                {40, 0, 200, 0}, {0, 0, 150, 0}, {-40, 0, 200, 0}, {0, 0, 300, 0},
                {40, 0, 200, 1}, {0, 0, 150, 1}, {-40, 0, 200, 1},
            };
            WallEBoard::Get().motors().Run(steps, false);
        };
        args.name = "diag_motors";
        esp_timer_create(&args, &timer);
    }
    esp_timer_stop(timer);
    esp_timer_start_once(timer, 1500 * 1000);
    return "The motors will twitch now: each wheel forward and back for a fifth of a second.";
}

}  // namespace

const std::vector<std::string>& Checks() {
    static const std::vector<std::string> checks = {"wifi",    "server", "memory",
                                                    "camera",  "mic",    "speaker",
                                                    "display", "motors", "all"};
    return checks;
}

std::string Run(const std::string& check) {
    ESP_LOGI(TAG, "Running check: %s", check.c_str());
    if (check == "wifi") return CheckWifi();
    if (check == "server") return CheckServer();
    if (check == "memory") return CheckMemory();
    if (check == "camera") return CheckCamera();
    if (check == "mic") return CheckMic();
    if (check == "speaker") return CheckSpeaker();
    if (check == "display") return CheckDisplay();
    if (check == "motors") return CheckMotors();
    if (check == "all") {
        return CheckWifi() + " " + CheckServer() + " " + CheckMemory() + " " + CheckCamera() +
               " " + CheckMic() + " " + CheckSpeaker() + " " + CheckDisplay() + " " +
               CheckMotors();
    }
    return "Unknown check " + check + ". Checks: wifi, server, memory, camera, mic, speaker, "
           "display, motors, all.";
}

}  // namespace walle_diagnostics
