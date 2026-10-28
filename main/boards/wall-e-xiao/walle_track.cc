#include "walle_track.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "board.h"
#include "motor_driver.h"
#include "system_info.h"
#include "wall_e_board.h"
#include "walle_camera.h"
#include "walle_display.h"
#include "walle_server.h"
#include "walle_settings.h"

#define TAG "WalleTrack"

namespace walle_track {

namespace {

// How far off-centre a face may be (as a part of half the picture width) before Jarvis turns, and where the medium and big corrections begin.
constexpr float kCentred = 0.12f;
constexpr float kMedium = 0.30f;
constexpr float kBig = 0.55f;
constexpr int kMaxRounds = 3;
constexpr int kSettleMs = 350;       // let the picture and the wheels calm down after a correction
constexpr float kOvershoot = 0.6f;   // a correction against the previous one is this much shorter (the previous one went too far)

std::mutex g_mutex;                  // one look-and-turn sequence at a time (one-shot or follow step)
std::atomic<bool> g_busy{false};
std::atomic<bool> g_oneshot{false};
TaskHandle_t g_task = nullptr;
int g_last_direction = 0;            // -1 = turned left, +1 = turned right, 0 = unknown (forgotten after a pause)
int64_t g_last_turn_us = 0;
int64_t g_next_talk_us = 0;
std::unique_ptr<Http> g_retired;     // the previous look's HTTP client, freed only when the next look is done (see Look())

int64_t NowUs() { return esp_timer_get_time(); }

float NumberOr(const cJSON* obj, const char* key, float fallback) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? static_cast<float>(item->valuedouble) : fallback;
}

// http://HOST:PORT/xiaozhi/ota/  ->  http://HOST:PORT/mcp/vision/face ("" on the xiaozhi.me cloud: it has no such call).
std::string FaceUrl() {
    if (!walle_server::IsCustom()) {
        return "";
    }
    const std::string ota = walle_server::OtaUrl();
    const size_t scheme = ota.find("://");
    const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    const size_t slash = ota.find('/', start);
    const std::string origin = slash == std::string::npos ? ota : ota.substr(0, slash);
    return origin + "/mcp/vision/face";
}

// A correction against the previous one (within 8 s) means that one went too far: take less this time.
int Damped(int ms, int direction) {
    const int64_t now = NowUs();
    const bool recent = g_last_turn_us != 0 && now - g_last_turn_us < 8LL * 1000 * 1000;
    if (recent && g_last_direction != 0 && direction != g_last_direction) {
        ms = std::max(80, static_cast<int>(ms * kOvershoot));
    }
    g_last_direction = direction;
    g_last_turn_us = now;
    return ms;
}

// Turns in place at face_power (100% by default) for ms. +1 = right, -1 = left. False if the move was stopped.
bool Pulse(int direction, int ms) {
    auto& board = WallEBoard::Get();
    if (WalleSettings::GetInstance().GetBool("face_invert")) {
        direction = -direction;
    }
    const int power = WalleSettings::GetInstance().GetInt("face_power") * direction;   // 100 unless the slider says otherwise; read at every turn, so it works live
    return board.motors().RunAndWait({{power, -power, ms}}, ms + 2000);
}

}  // namespace

bool Busy() { return g_busy.load(); }

std::expected<Face, std::string> Look() {
    auto& board = WallEBoard::Get();
    auto* camera = board.walle_camera();
    if (camera == nullptr || !camera->Detected()) {
        return std::unexpected("Jarvis has no camera.");
    }
    const std::string url = FaceUrl();
    if (url.empty()) {
        return std::unexpected("Face finding needs your own voice server.");
    }
    // fast capture: no preview is made for the screen at all, so nothing needs suppressing
    uint8_t* jpeg = nullptr;
    size_t jpeg_len = 0;
    if (!camera->CaptureJpeg(55, &jpeg, &jpeg_len, true)) {
        return std::unexpected(std::string("The camera did not take a picture: ") + camera->CaptureError());
    }
    std::string body(reinterpret_cast<const char*>(jpeg), jpeg_len);
    heap_caps_free(jpeg);

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return std::unexpected("No network.");
    }
    auto http = network->CreateHttp(0);
    // The HTTP client must outlive its network task: when the server closes the connection right after its answer, that task still calls back into
    // the client a moment later, and a client freed by then crashed the robot (use after free, watchdog reset, after 49 minutes of follow mode).
    // So each client is kept until the next look has used its own (g_mutex serialises the looks).
    struct Retire {
        std::unique_ptr<Http>& client;
        ~Retire() { g_retired = std::move(client); }
    } retire{http};
    http->SetTimeout(6000);
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("Content-Type", "image/jpeg");
    http->SetContent(std::move(body));
    if (auto opened = http->Open("POST", url); !opened) {
        return std::unexpected("Could not reach the voice server.");
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        const int code = status ? static_cast<int>(*status) : -1;
        http->Close();
        ESP_LOGW(TAG, "Face finding answered HTTP %d", code);
        return std::unexpected(code == 404 ? "The server cannot find faces yet (patch missing)."
                                           : "The server could not look for a face.");
    }
    const std::string reply = http->ReadAll();
    http->Close();

    cJSON* root = cJSON_Parse(reply.c_str());
    if (root == nullptr) {
        return std::unexpected("The server's answer was not readable.");
    }
    Face face;
    const cJSON* ok = cJSON_GetObjectItem(root, "success");
    if (!cJSON_IsTrue(ok)) {
        const cJSON* message = cJSON_GetObjectItem(root, "message");
        std::string why = cJSON_IsString(message) ? message->valuestring : "The server could not look for a face.";
        cJSON_Delete(root);
        return std::unexpected(why);
    }
    face.found = cJSON_IsTrue(cJSON_GetObjectItem(root, "found"));
    face.x = std::clamp(NumberOr(root, "x", 0), -1.0f, 1.0f);
    face.width = NumberOr(root, "w", 0);
    face.count = static_cast<int>(NumberOr(root, "n", face.found ? 1 : 0));
    cJSON_Delete(root);
    return face;
}

int TurnMs(float x) {
    const float a = std::fabs(x);
    if (a < kCentred) {
        return 0;
    }
    auto& settings = WalleSettings::GetInstance();
    if (a < kMedium) {
        return settings.GetInt("face_short_ms");
    }
    if (a < kBig) {
        return settings.GetInt("face_mid_ms");
    }
    return settings.GetInt("face_long_ms");
}

std::expected<std::string, std::string> FaceMe() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_busy = true;
    struct Done {
        ~Done() { g_busy = false; }
    } done;

    // (the caller wakes Jarvis from a nap first: that must happen on the main task)
    g_last_direction = 0;
    bool turned = false;
    for (int round = 0; round < kMaxRounds; ++round) {
        auto face = Look();
        if (!face) {
            return std::unexpected(face.error());
        }
        if (!face->found) {
            if (turned) {
                return std::string("I turned, but now I cannot see your face. Step in front of me.");
            }
            return std::string("I cannot see a face. Step in front of me, in the light, and ask again.");
        }
        const int ms = TurnMs(face->x);
        ESP_LOGI(TAG, "Face at %.2f (width %.2f, %d seen): %d ms", face->x, face->width, face->count, ms);
        if (ms == 0) {
            return std::string(turned ? "I am facing you now." : "I am already facing you.");
        }
        const int direction = face->x > 0 ? 1 : -1;
        if (!Pulse(direction, Damped(ms, direction))) {
            return std::unexpected("Stopped while turning.");
        }
        turned = true;
        vTaskDelay(pdMS_TO_TICKS(kSettleMs));
    }
    return std::string("I turned towards you.");
}

namespace {

// One look and at most one correction, while follow mode is on. Quiet: no result, no speech.
// Returns true when a face was seen.
bool FollowStep() {
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return true;
    }
    g_busy = true;
    struct Done {
        ~Done() { g_busy = false; }
    } done;

    auto face = Look();
    if (!face) {
        ESP_LOGW(TAG, "Follow: %s", face.error().c_str());
        return false;
    }
    if (!face->found) {
        return false;
    }
    const int ms = TurnMs(face->x);
    if (ms == 0) {
        return true;
    }
    const int direction = face->x > 0 ? 1 : -1;
    Pulse(direction, Damped(ms, direction));
    vTaskDelay(pdMS_TO_TICKS(kSettleMs));
    return true;
}

// Follow mode only moves while Jarvis is idle: not during a conversation (the wheels would be heard and the Wi-Fi is busy),
// not asleep, not during an alarm.
bool CanFollowNow() {
    auto& board = WallEBoard::Get();
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return false;
    }
    return !board.IsNapping() && !board.IsRinging() && board.NetworkConnected() && !board.motors().IsBusy();
}

void TrackTask(void*) {
    int misses = 0;
    while (true) {
        auto& settings = WalleSettings::GetInstance();
        const bool follow = settings.GetBool("follow_mode");
        // After a few looks without a face, look less often: nobody is there.
        const int wait_ms = follow ? settings.GetInt("follow_ms") * (misses >= 5 ? 4 : 1) : 1000;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
        if (g_oneshot.exchange(false)) {
            auto result = FaceMe();
            ESP_LOGI(TAG, "Face me: %s", result ? result->c_str() : result.error().c_str());
            continue;
        }
        if (!settings.GetBool("follow_mode") || !CanFollowNow()) {
            misses = 0;
            continue;
        }
        misses = FollowStep() ? 0 : misses + 1;
    }
}

// Short left-right shakes in place, at talk_power. Each step is long enough for the soft start to get the wheels moving.
std::vector<MotorDriver::Step> TalkSteps(const std::string& emotion, int power) {
    const int p = power;
    auto shake = [&](int ms) {
        return std::vector<MotorDriver::Step>{{p, -p, ms}, {-p, p, ms}};
    };
    if (emotion == "happy" || emotion == "laughing" || emotion == "funny" || emotion == "silly" ||
        emotion == "winking" || emotion == "delicious" || emotion == "cool" || emotion == "confident" ||
        emotion == "loving" || emotion == "kissy") {
        auto steps = shake(170);          // lively: two quick shakes
        const auto again = shake(170);
        steps.insert(steps.end(), again.begin(), again.end());
        return steps;
    }
    if (emotion == "sad" || emotion == "crying" || emotion == "sleepy" || emotion == "embarrassed") {
        return {{p, -p, 220}, {0, 0, 260}, {-p, p, 220}};   // slow and small
    }
    if (emotion == "thinking" || emotion == "confused") {
        return {{(esp_random() & 1) ? p : -p, (esp_random() & 1) ? -p : p, 200}, {0, 0, 350}};   // a glance to one side
    }
    return shake(190);                     // calm default: one shake
}

}  // namespace

void Start() {
    auto& settings = WalleSettings::GetInstance();
    if (settings.GetBool("follow_mode")) {
        settings.Set("follow_mode", "0");   // never driving around by itself after a restart
    }
    xTaskCreate(TrackTask, "track", 8192, nullptr, 3, &g_task);
}

void Wake() {
    if (g_task != nullptr) {
        xTaskNotifyGive(g_task);
    }
}

void FaceMeLater() {
    g_oneshot = true;
    Wake();
}

void TalkTick() {
    auto& board = WallEBoard::Get();
    auto& settings = WalleSettings::GetInstance();
    auto* display = board.walle_display();
    if (display == nullptr || !settings.GetBool("talk_motion") ||
        Application::GetInstance().GetDeviceState() != kDeviceStateSpeaking) {
        g_next_talk_us = 0;
        return;
    }
    const int64_t now = NowUs();
    if (g_next_talk_us == 0) {
        g_next_talk_us = now + 800 * 1000;   // not at the very first word
        return;
    }
    if (now < g_next_talk_us) {
        return;
    }
    g_next_talk_us = now + (2000 + static_cast<int64_t>(esp_random() % 2500)) * 1000;
    // Stay still while napping, at an alarm, during music or radio, in a photo or a follow look, and when anything else drives the wheels.
    if (board.IsNapping() || board.IsRinging() || display->IsBlank() || display->MusicPlaying() || Busy() ||
        board.motors().IsBusy()) {
        return;
    }
    board.motors().Run(TalkSteps(display->AutoEmotionCopy(), settings.GetInt("talk_power")), false);
}

}  // namespace walle_track
