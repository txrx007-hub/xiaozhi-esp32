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

// The turn is worked out from the face's offset x (-1..+1 across the picture) and the robot's own turning speed, which it learns while it works
// (g_gain: how far the picture shifts per millisecond of turning at full power). The margin before Jarvis turns at all is the setting face_deadzone,
// the power of the big turns is face_power; corrections of less than kMedium use 80% of it (never lower).
constexpr float kMedium = 0.30f;
constexpr int kSmallPower = 80;
constexpr int kMaxLooks = 4;           // "look at me": at most four pictures; each is inspected first, then the robot decides where (and how far) to turn
constexpr int64_t kFreshUs = 1500 * 1000;   // a camera queue untouched for longer than this holds old frames: the next look takes all three
constexpr float kSearchShift = 1.0f;   // each search step turns the picture by about this much (half the field of view)
constexpr float kSeedGain = 0.0007f;   // measured on this robot at 100%: about 0.34 of the picture in 500 ms
constexpr float kMinGain = 0.0003f;
constexpr float kMaxGain = 0.0020f;
constexpr float kUndershoot = 0.85f;   // aim a little short: the next look corrects the rest, and nothing overshoots
constexpr int kMinPulseMs = 120;
constexpr int kMaxPulseMs = 1500;
constexpr int kSettleMs = 350;       // let the picture and the wheels calm down after a correction
constexpr float kOvershoot = 0.6f;   // a correction against the previous one is this much shorter (the previous one went too far)

std::mutex g_mutex;                  // one look-and-turn sequence at a time (one-shot or follow step)
std::atomic<bool> g_busy{false};
std::atomic<bool> g_oneshot{false};
std::atomic<bool> g_faced{false};    // the last FaceMe ended facing a face
std::atomic<uint32_t> g_looks{0};        // looks answered by the server since boot (live stats window)
std::atomic<uint32_t> g_looks_found{0};  // ... and those with a face in them
std::mutex g_call_mutex;
std::string g_call_text;             // FaceMeThenCall: what to say once facing the face (empty: nothing)
TaskHandle_t g_task = nullptr;
int g_last_direction = 0;            // -1 = turned left, +1 = turned right, 0 = unknown (forgotten after a pause)
int64_t g_last_turn_us = 0;
int64_t g_next_talk_us = 0;
bool g_audio_flowing = false;        // sound is coming out of the speaker (see TalkTick)
int g_talk_side = -1;                // which wheel led the last talking movement: -1 left, +1 right
esp_timer_handle_t g_talk_timer = nullptr;
constexpr int kAudioFlowMs = 250;    // sound counts as coming out while its last chunk went to the speaker at most this long ago
constexpr int kFirstMoveMs = 150;    // the first movement of a reply starts this long after its first sound
std::unique_ptr<Http> g_retired;     // the previous look's HTTP client, freed only when the next look is done (see Look())
float g_gain = kSeedGain;            // learned: picture widths (x units) per millisecond of turning at 100% power
int64_t g_last_look_us = 0;          // when the camera was last asked for a look
bool g_stale_view = false;           // the wheels turned since the last look: the camera queue still holds frames from before the turn
int g_seen_side = 0;                 // which side the face was last seen on: -1 left, +1 right (where a search starts)

// The turn that was just made, kept until the next look shows what it did.
struct Pending {
    bool valid = false;
    float x_before = 0;
    float width = 0;
    int ms = 0;
    int power = 100;
    int64_t end_us = 0;
} g_pending;

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

// Turns in place at the given power (percent) for ms. +1 = towards what is on the right of the picture, -1 = left. False if the move was stopped.
bool Pulse(int direction, int ms, int percent) {
    auto& board = WallEBoard::Get();
    if (WalleSettings::GetInstance().GetBool("face_invert")) {
        direction = -direction;
    }
    const int power = percent * direction;
    const bool done = board.motors().RunAndWait({{power, -power, ms}}, ms + 2000);
    g_stale_view = true;
    return done;
}

struct Plan {
    int ms = 0;      // 0 = the face is within the margin: no turn
    int power = 0;
};

Plan PlanTurn(float x) {
    auto& settings = WalleSettings::GetInstance();
    const float a = std::fabs(x);
    if (a < settings.GetInt("face_deadzone") / 100.0f) {
        return {};
    }
    const int cap = settings.GetInt("face_power");
    const int power = a < kMedium ? std::min(cap, kSmallPower) : cap;
    const float per_ms = g_gain * power / 100.0f;
    const int ms = std::clamp(static_cast<int>(a / per_ms * kUndershoot), kMinPulseMs, kMaxPulseMs);
    return {ms, power};
}

// A look after a turn shows how far that turn really moved the picture: that corrects the robot's idea of its own turning speed.
void Learn(const Face& now) {
    if (!g_pending.valid) {
        return;
    }
    const Pending p = g_pending;
    g_pending.valid = false;
    if (!now.found || NowUs() - p.end_us > 2500LL * 1000 || p.ms < 150 || std::fabs(p.x_before) < 0.2f) {
        return;   // no face, too long after the turn (the person may have moved), or too small to measure
    }
    if (p.width > 0 && now.width > 0) {
        const float ratio = now.width / p.width;
        if (ratio < 0.85f || ratio > 1.18f) {
            return;   // the person walked towards or away from the robot
        }
    }
    const float progress = (p.x_before - now.x) * (p.x_before > 0 ? 1.0f : -1.0f);   // how far the face came towards the middle
    const float observed = progress / (p.ms * p.power / 100.0f);
    if (observed < 0.5f * g_gain || observed > 1.6f * g_gain) {
        return;   // far from what the robot knows: the person moved, or the picture was blurred
    }
    const float before = g_gain;
    g_gain = std::clamp(0.8f * g_gain + 0.2f * observed, kMinGain, kMaxGain);
    ESP_LOGI(TAG, "Learned: the picture moved %.2f in %d ms at %d%%: turning speed %.5f -> %.5f per ms", progress, p.ms, p.power, before, g_gain);
}

// Makes the turn for a face at x. Returns false if the move was stopped.
bool Turn(const Face& face, const Plan& plan) {
    const int direction = face.x > 0 ? 1 : -1;
    const int ms = Damped(plan.ms, direction);
    if (!Pulse(direction, ms, plan.power)) {
        return false;
    }
    g_pending = {true, face.x, face.width, ms, plan.power, NowUs()};
    g_seen_side = direction;
    return true;
}

std::atomic<bool> g_gazing{false};   // the eyes are looking away from straight ahead
int g_gaze_misses = 0;               // looks in a row without a face (the eyes go back to the middle after two)

// The eyes look at a face (nullptr or not found: straight ahead). The settings eyes_range (sideways) and eyes_range_y (up and down) are how far they slide, in
// pixels: the eye shapes are about 200 px wide on the 240 px screen, so sideways they have little room before their outer edge leaves the screen, up and down much more
// (measured over all faces: 13 px left and right, 32 px up and 52 px down at worst, 72 / 81 for the neutral face). With the defaults (40 / 80) a face at the side of the
// picture sends the eyes to the edge of the screen and a face in a corner of the picture into the corner, partly cut off by the screen edge. The camera picture is not
// mirrored (CAMERA_HMIRROR is off): a face on the right of the picture stands on the robot's right, which is the LEFT of the screen for someone looking at it, so
// the eyes slide left (eyes_invert swaps that). A face high in the picture is looked up at.
void GazeAt(const Face* face) {
    auto* display = WallEBoard::Get().walle_display();
    if (display == nullptr) {
        return;
    }
    if (face == nullptr || !face->found) {
        if (g_gazing.exchange(false)) {
            display->SetGaze(0, 0);
        }
        return;
    }
    auto& settings = WalleSettings::GetInstance();
    const float range_x = static_cast<float>(settings.GetInt("eyes_range"));
    const float range_y = static_cast<float>(settings.GetInt("eyes_range_y"));
    const float sign = settings.GetBool("eyes_invert") ? 1.0f : -1.0f;   // screen x grows to the right of someone looking at the robot
    // A face seldom reaches the very edge of the picture: the eyes reach their full travel when it is 60% of the way out. Nearly straight ahead: straight ahead.
    auto lead = [](float v) { return std::fabs(v) < 0.06f ? 0.0f : std::clamp(v * 1.6f, -1.0f, 1.0f); };
    const int dx = static_cast<int>(std::lround(lead(face->x) * sign * range_x));
    const int dy = static_cast<int>(std::lround(lead(face->y) * range_y));
    g_gazing = dx != 0 || dy != 0;
    display->SetGaze(dx, dy);
}

}  // namespace

bool Busy() { return g_busy.load(); }
uint32_t Looks() { return g_looks.load(); }
uint32_t LooksFound() { return g_looks_found.load(); }

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
    // No preview for the screen. Right after a turn all three frames are taken: the first ones in the queue are from before or during the turn.
    uint8_t* jpeg = nullptr;
    size_t jpeg_len = 0;
    // The camera keeps a queue of frames that were taken while nobody asked: after a turn, or after any pause, the first ones are old (an earlier view,
    // blur, nobody in front of it). Then all three frames are taken so that the last one is fresh; in a quick series of looks one is enough.
    const int64_t now = NowUs();
    const bool old_queue = g_stale_view || g_last_look_us == 0 || now - g_last_look_us > kFreshUs;
    const int frames = old_queue ? 3 : 1;
    g_stale_view = false;
    if (!camera->CaptureJpeg(55, &jpeg, &jpeg_len, frames, false)) {
        return std::unexpected(std::string("The camera did not take a picture: ") + camera->CaptureError());
    }
    g_last_look_us = NowUs();
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
    face.y = std::clamp(NumberOr(root, "y", 0), -1.0f, 1.0f);
    face.width = NumberOr(root, "w", 0);
    face.count = static_cast<int>(NumberOr(root, "n", face.found ? 1 : 0));
    cJSON_Delete(root);
    ++g_looks;
    if (face.found) {
        ++g_looks_found;
    }
    return face;
}

std::expected<std::string, std::string> FaceMe() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_busy = true;
    struct Done {
        ~Done() { g_busy = false; }
    } done;

    // (the caller wakes Jarvis from a nap first: that must happen on the main task)
    g_last_direction = 0;
    g_faced = false;
    g_pending.valid = false;
    g_stale_view = true;   // the first picture must be a fresh one, not the last frame from whenever the camera was last used
    // The eyes look at the face while Jarvis turns to it, and are straight ahead again at the end (unless eyes_follow keeps them on the face).
    struct EyesBack {
        ~EyesBack() {
            if (!WalleSettings::GetInstance().GetBool("eyes_follow")) {
                GazeAt(nullptr);
            }
        }
    } eyes_back;
    bool turned = false;
    bool seen = false;
    for (int look = 1; look <= kMaxLooks; ++look) {
        auto face = Look();
        if (!face) {
            return std::unexpected(face.error());
        }
        Learn(*face);
        GazeAt(&*face);
        if (!face->found) {
            if (seen) {
                return std::string("I turned, but now I cannot see your face. Step in front of me.");
            }
            if (look == kMaxLooks) {
                break;   // the last picture: nothing left to look at after another turn
            }
            // Nobody in the picture: turn on the spot (towards the side where a face was last seen, else to the right) and look again.
            const int ms = std::clamp(static_cast<int>(kSearchShift / g_gain), 500, 1800);
            ESP_LOGI(TAG, "Picture %d of %d: no face in view: searching (%d ms)", look, kMaxLooks, ms);
            if (!Pulse(g_seen_side != 0 ? g_seen_side : 1, ms, WalleSettings::GetInstance().GetInt("face_power"))) {
                return std::unexpected("Stopped while turning.");
            }
            turned = true;
            vTaskDelay(pdMS_TO_TICKS(kSettleMs));
            continue;
        }
        seen = true;
        g_seen_side = face->x > 0 ? 1 : -1;
        const Plan plan = PlanTurn(face->x);
        ESP_LOGI(TAG, "Picture %d of %d: face at %.2f (width %.2f, %d seen): %d ms at %d%%", look, kMaxLooks, face->x, face->width, face->count, plan.ms, plan.power);
        if (plan.ms == 0) {
            g_faced = true;
            return std::string(turned ? "I am facing you now." : "I am already facing you.");
        }
        if (!Turn(*face, plan)) {
            return std::unexpected("Stopped while turning.");
        }
        GazeAt(nullptr);   // turned: the face is now (nearly) straight ahead
        turned = true;
        vTaskDelay(pdMS_TO_TICKS(kSettleMs));
    }
    if (!seen) {
        return std::string("I looked around but I cannot see a face. Step in front of me, in the light, and ask again.");
    }
    g_faced = true;   // the last turn was towards the face (four pictures used up)
    return std::string("I turned towards you.");
}

namespace {

// One look while follow mode or eyes_follow is on. The eyes look at the face; with move (follow mode) there is also at most one correction of the wheels.
// Quiet: no result, no speech. Returns true when a face was seen.
bool FollowStep(bool move) {
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
    if (move) {
        Learn(*face);
    }
    if (!face->found) {
        if (++g_gaze_misses >= 2) {
            GazeAt(nullptr);   // nobody there any more: the eyes look straight ahead again
        }
        return false;
    }
    g_gaze_misses = 0;
    GazeAt(&*face);
    if (!move) {
        return true;
    }
    const Plan plan = PlanTurn(face->x);
    if (plan.ms == 0) {
        return true;
    }
    Turn(*face, plan);
    GazeAt(nullptr);   // turned: the face is now (nearly) straight ahead
    vTaskDelay(pdMS_TO_TICKS(kSettleMs));
    return true;
}

// Tracking only happens while Jarvis is idle: not during a conversation (the wheels would be heard and the Wi-Fi is busy), not asleep, not during an alarm.
// With move (follow mode) the wheels must also be free. Eyes only (move false): the eyes must be what the screen shows, otherwise there is nothing to move.
bool CanFollowNow(bool move) {
    auto& board = WallEBoard::Get();
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return false;
    }
    if (board.IsNapping() || board.IsRinging() || !board.NetworkConnected()) {
        return false;
    }
    if (move) {
        return !board.motors().IsBusy();
    }
    auto* display = board.walle_display();
    return display != nullptr && display->EyesVisible();
}

void TrackTask(void*) {
    int misses = 0;
    while (true) {
        auto& settings = WalleSettings::GetInstance();
        const bool follow = settings.GetBool("follow_mode");
        const bool eyes = settings.GetBool("eyes_follow") && walle_server::IsCustom();   // finding the face needs the own server
        // After a few looks without a face, look less often: nobody is there.
        const int wait_ms = (follow || eyes) ? settings.GetInt("follow_ms") * (misses >= 5 ? 4 : 1) : 1000;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
        if (g_oneshot.exchange(false)) {
            auto result = FaceMe();
            ESP_LOGI(TAG, "Face me: %s", result ? result->c_str() : result.error().c_str());
            std::string text;
            {
                std::lock_guard<std::mutex> lock(g_call_mutex);
                text.swap(g_call_text);
            }
            if (!text.empty()) {   // the greeter: speak only when it ended facing you
                const bool said = result && g_faced && Application::GetInstance().DeviceCall(text);
                ESP_LOGI(TAG, "Greeting after Face me: %s", said ? "spoken" : "not spoken (no face at the end, or not idle)");
            }
            continue;
        }
        const bool move = settings.GetBool("follow_mode");
        const bool look = move || (settings.GetBool("eyes_follow") && walle_server::IsCustom());
        if (!look || !CanFollowNow(move)) {
            misses = 0;
            g_gaze_misses = 0;
            if (!g_busy) {   // (a Face me from the voice tools is under way: it looks after the eyes itself)
                GazeAt(nullptr);   // not tracking (switched off, or a conversation, a nap, the clock): the eyes look straight ahead
            }
            continue;
        }
        misses = FollowStep(move) ? 0 : misses + 1;
    }
}

// Short left-right sways, at talk_power. Each step is long enough for the soft start to get the wheels moving. side 0: both wheels turn the body on
// the spot; side -1 / +1: only the left / right wheel moves and the body swings about the other one. A sway goes out and comes back, so the heading ends the same.
std::vector<MotorDriver::Step> TalkSteps(const std::string& emotion, int power, int side) {
    const int l = side == 0 ? power : (side < 0 ? power : 0);
    const int r = side == 0 ? -power : (side < 0 ? 0 : power);
    auto swing = [&](int ms, int dir) { return MotorDriver::Step{dir * l, dir * r, ms}; };
    auto shake = [&](int ms) {
        return std::vector<MotorDriver::Step>{swing(ms, 1), swing(ms, -1)};
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
        return {swing(220, 1), {0, 0, 260}, swing(220, -1)};   // slow and small
    }
    if (emotion == "thinking" || emotion == "confused") {
        return {swing(200, (esp_random() & 1) ? 1 : -1), {0, 0, 350}};   // a glance to one side
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
    esp_timer_create_args_t talk = {};
    talk.callback = [](void*) { TalkTick(); };
    talk.name = "talk_motion";
    talk.skip_unhandled_events = true;
    if (esp_timer_create(&talk, &g_talk_timer) == ESP_OK) {
        esp_timer_start_periodic(g_talk_timer, 100 * 1000);
    }
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

void FaceMeThenCall(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(g_call_mutex);
        g_call_text = text;
    }
    FaceMeLater();
}

// Every 100 ms (a timer of its own). The "Speaking" state starts well before the first word comes out of the speaker (the server's first sentence
// still has to be spoken, sent and decoded), so the movements follow the sound itself: they begin a moment after the first sound reaches the speaker
// and stand still again in the gaps between sentences.
void TalkTick() {
    auto& board = WallEBoard::Get();
    auto& settings = WalleSettings::GetInstance();
    auto* display = board.walle_display();
    auto& app = Application::GetInstance();
    if (display == nullptr || !settings.GetBool("talk_motion") || app.GetDeviceState() != kDeviceStateSpeaking) {
        g_next_talk_us = 0;
        g_audio_flowing = false;
        return;
    }
    const bool flowing = app.GetAudioService().MsSinceLastOutput() <= kAudioFlowMs;
    if (!flowing) {
        g_audio_flowing = false;   // no words coming out yet, or a pause between two sentences: stand still
        return;
    }
    const int64_t now = NowUs();
    if (!g_audio_flowing) {
        g_audio_flowing = true;
        if (g_next_talk_us == 0) {
            g_next_talk_us = now + kFirstMoveMs * 1000;   // the first sound of this reply
        }
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
    int side = 0;
    if (settings.GetBool("talk_alternate")) {
        g_talk_side = -g_talk_side;   // left, right, left, ...
        side = g_talk_side;
    }
    board.motors().Run(TalkSteps(display->AutoEmotionCopy(), settings.GetInt("talk_power"), side), false);
}

}  // namespace walle_track
