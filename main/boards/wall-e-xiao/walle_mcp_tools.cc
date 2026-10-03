#include "walle_mcp_tools.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "config.h"
#include "mcp_server.h"
#include "wall_e_board.h"
#include "walle_diagnostics.h"
#include "walle_settings.h"
#include "walle_timers.h"
#include "walle_weather.h"
#include "walle_web.h"

#define TAG "WalleTools"

namespace {

const std::vector<std::string> kEmotions = {
    "neutral", "happy",     "laughing", "funny",  "sad",      "angry",    "crying",
    "loving",  "embarrassed", "surprised", "shocked", "thinking", "winking", "cool",
    "relaxed", "delicious", "kissy",    "confident", "sleepy", "silly",   "confused"};

std::string EmotionList() {
    std::string text;
    for (size_t i = 0; i < kEmotions.size(); ++i) {
        text += (i ? ", " : "") + kEmotions[i];
    }
    return text;
}

int RandomIn(int low, int high) { return low + static_cast<int>(esp_random() % (high - low + 1)); }

std::vector<MotorDriver::Step> DanceSteps(const std::string& style, int turn_ms) {
    std::vector<MotorDriver::Step> steps;
    auto wiggle = [&](int times) {
        for (int i = 0; i < times; ++i) {
            steps.push_back({55, -55, 200});
            steps.push_back({-55, 55, 200});
        }
    };
    if (style == "wiggle") {
        wiggle(5);
    } else if (style == "spin") {
        steps.push_back({70, -70, std::min(turn_ms * 4 * 60 / 70, 5000)});
        wiggle(2);
    } else if (style == "happy") {
        for (int i = 0; i < 2; ++i) {
            steps.push_back({55, 55, 300});
            steps.push_back({-55, -55, 300});
        }
        wiggle(2);
        steps.push_back({65, -65, std::min(turn_ms * 2 * 60 / 65, 3000)});
    } else {  // random: a new routine every time, 3 to 6 seconds
        int total = 0;
        const int target = RandomIn(3000, 6000);
        while (total < target) {
            MotorDriver::Step step{0, 0, 0};
            const int speed = RandomIn(45, 90);
            switch (RandomIn(0, 5)) {
                case 0: step = {speed, speed, RandomIn(150, 400)}; break;    // forward
                case 1: step = {-speed, -speed, RandomIn(150, 400)}; break;  // back
                case 2: step = {speed, -speed, RandomIn(150, 450)}; break;   // spin right
                case 3: step = {-speed, speed, RandomIn(150, 450)}; break;   // spin left
                case 4: step = {speed, 0, RandomIn(150, 350)}; break;        // pivot
                default: step = {0, 0, RandomIn(100, 200)}; break;           // pause
            }
            steps.push_back(step);
            total += step.duration_ms;
        }
    }
    return steps;
}

int TotalMs(const std::vector<MotorDriver::Step>& steps) {
    int total = 0;
    for (const auto& step : steps) {
        total += step.duration_ms;
    }
    return total;
}

}  // namespace

void RegisterWalleTools(WallEBoard& board) {
    (void)board;  // tools look the board up themselves (they outlive this call)
    auto& mcp = McpServer::GetInstance();

    // ---------------------------------------------------------------- robot
    mcp.AddTool("self.robot.move",
                "Drive Jarvis. direction: forward, backward, left (spin left in place) or right "
                "(spin right in place). speed: 0-100 percent (capped by setting motor_max_speed). "
                "duration_ms: 100-5000. Moves are queued, so several calls run one after another, "
                "and Jarvis stops by itself after each move. Use self.robot.stop to stop at once.",
                PropertyList({Property("direction", kPropertyTypeString),
                              Property("speed", kPropertyTypeInteger, 60, 0, 100),
                              Property("duration_ms", kPropertyTypeInteger, 1000, 100, 5000)}),
                [](const PropertyList& p) -> ToolResult {
                    auto& board = WallEBoard::Get();
                    const auto direction = p["direction"].value<std::string>();
                    const int speed = p["speed"].value<int>();
                    const int ms = p["duration_ms"].value<int>();
                    int left;
                    int right;
                    if (direction == "forward") {
                        left = speed, right = speed;
                    } else if (direction == "backward") {
                        left = -speed, right = -speed;
                    } else if (direction == "left") {
                        left = -speed, right = speed;
                    } else if (direction == "right") {
                        left = speed, right = -speed;
                    } else {
                        return std::unexpected("direction must be forward, backward, left or right");
                    }
                    board.ExitNap();
                    if (!board.motors().Run({{left, right, ms}}, true)) {
                        return std::unexpected("Too many moves are queued; wait or stop first.");
                    }
                    return "Moving " + direction + " at " + std::to_string(speed) + "% for " +
                           std::to_string(ms) + " ms.";
                });

    mcp.AddTool("self.robot.turn",
                "Turn Jarvis in place by an angle. degrees: -360 to 360; positive turns right "
                "(clockwise), negative turns left. 'Turn around' is 180. speed: 20-100. Turning is "
                "timed (calibrated by setting turn_ms_per_90), so the angle is approximate.",
                PropertyList({Property("degrees", kPropertyTypeInteger, -360, 360),
                              Property("speed", kPropertyTypeInteger, 60, 20, 100)}),
                [](const PropertyList& p) -> ToolResult {
                    auto& board = WallEBoard::Get();
                    auto& settings = WalleSettings::GetInstance();
                    const int degrees = p["degrees"].value<int>();
                    const int speed = p["speed"].value<int>();
                    if (degrees == 0) {
                        return "No turn needed.";
                    }
                    int ms = std::abs(degrees) * settings.GetInt("turn_ms_per_90") / 90 * 60 / speed;
                    ms = std::clamp(ms, 100, 10000);
                    const int s = degrees > 0 ? speed : -speed;
                    board.ExitNap();
                    if (!board.motors().Run({{s, -s, ms}}, true)) {
                        return std::unexpected("Too many moves are queued; wait or stop first.");
                    }
                    return "Turning " + std::string(degrees > 0 ? "right " : "left ") +
                           std::to_string(std::abs(degrees)) + " degrees.";
                });

    mcp.AddTool("self.robot.stop", "Stop Jarvis's wheels immediately and cancel queued moves.",
                PropertyList(), [](const PropertyList&) -> ToolResult {
                    auto& board = WallEBoard::Get();
                    board.motors().Stop();
                    return "Stopped.";
                });

    mcp.AddTool("self.robot.dance",
                "Do a short dance (3-6 seconds) with happy eyes. style: random (default, a new "
                "routine every time), wiggle, spin or happy.",
                PropertyList({Property("style", kPropertyTypeString, std::string("random"))}),
                [](const PropertyList& p) -> ToolResult {
                    auto& board = WallEBoard::Get();
                    auto& settings = WalleSettings::GetInstance();
                    auto style = p["style"].value<std::string>();
                    if (style != "wiggle" && style != "spin" && style != "happy") {
                        style = "random";
                    }
                    auto steps = DanceSteps(style, settings.GetInt("turn_ms_per_90"));
                    board.ExitNap();
                    board.motors().Run(steps, false);
                    board.walle_display()->HoldEmotion(style == "random" ? "laughing" : "happy",
                                                       TotalMs(steps) + 500);
                    return "Dancing (" + style + ").";
                });

    mcp.AddTool("self.robot.look_around",
                "Turn around in four 90-degree steps, take a photo at each, and describe the "
                "surroundings in one answer (takes about 10 seconds). question: what to look for.",
                PropertyList({Property("question", kPropertyTypeString,
                                       std::string("Describe what you see in each direction."))}),
                [](const PropertyList& p) -> ToolResult {
                    auto& board = WallEBoard::Get();
                    auto& settings = WalleSettings::GetInstance();
                    auto* camera = board.walle_camera();
                    if (camera == nullptr) {
                        return std::unexpected("Jarvis has no camera.");
                    }
                    board.ExitNap();
                    TaskPriorityReset priority_reset(1);
                    const int turn_ms = settings.GetInt("turn_ms_per_90");
                    board.walle_display()->HoldEmotion("thinking", 4 * (turn_ms + 900) + 4000);
                    camera->CollageBegin();
                    for (int slot = 0; slot < 4; ++slot) {
                        if (!camera->CollageAdd(slot)) {
                            camera->CollageEnd();
                            return std::unexpected("The camera did not take a picture.");
                        }
                        if (!board.motors().RunAndWait({{60, -60, turn_ms}}, turn_ms + 2000)) {
                            camera->CollageEnd();
                            return std::unexpected("Stopped while turning.");
                        }
                        vTaskDelay(pdMS_TO_TICKS(250));  // let the view settle
                    }
                    auto result = camera->CollageExplain(
                        "This image is a 2x2 collage of four photos Jarvis took while turning "
                        "right in 90 degree steps: top-left is straight ahead, top-right is to "
                        "the right, bottom-left is behind, bottom-right is to the left. " +
                        p["question"].value<std::string>());
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                    return *result;
                });

    // ---------------------------------------------------------------- screen
    mcp.AddTool("self.screen.set_emotion",
                "Show an eye expression on Jarvis's screen for hold_ms, then go back to "
                "automatic expressions. emotion: one of " + EmotionList() + ".",
                PropertyList({Property("emotion", kPropertyTypeString),
                              Property("hold_ms", kPropertyTypeInteger, 3000, 500, 30000)}),
                [](const PropertyList& p) -> ToolResult {
                    auto& board = WallEBoard::Get();
                    const auto emotion = p["emotion"].value<std::string>();
                    if (std::find(kEmotions.begin(), kEmotions.end(), emotion) == kEmotions.end()) {
                        return std::unexpected("Unknown emotion. Use one of: " + EmotionList());
                    }
                    board.ExitNap();
                    board.walle_display()->HoldEmotion(emotion, p["hold_ms"].value<int>());
                    return "Showing " + emotion + ".";
                });

    mcp.AddTool("self.display.set_visualizer",
                "Set what Jarvis's screen shows while it speaks. mode: off (default, plain level "
                "bars), winamp (32 green/yellow/red spectrum bars with peak-hold dots), scope "
                "(oscilloscope waveform), mouth (a robot mouth under the eyes that moves with the "
                "voice), or - replacing the eyes while speaking - radial (spectrum bars around a "
                "circle), vu (analog VU meter needle) or orb (pulsing rings). Saved.",
                PropertyList({Property("mode", kPropertyTypeString)}),
                [](const PropertyList& p) -> ToolResult {
                    auto& settings = WalleSettings::GetInstance();
                    auto result =
                        settings.Set("visualizer_mode", p["mode"].value<std::string>());
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                    return *result;
                });

    mcp.AddTool("self.display.show_screensaver",
                "Show the idle screen right away (big clock, date and weather) instead of waiting "
                "for the usual idle timeout - like a screensaver. It goes away by itself the "
                "instant you say \"" WAKE_WORD_NAME "\" again; there is no separate command to "
                "turn it off.",
                PropertyList(), [](const PropertyList&) -> ToolResult {
                    WallEBoard::Get().RequestScreensaver();
                    return "Screensaver will show once this reply finishes.";
                });

    // ---------------------------------------------------------------- audio
    mcp.AddTool("self.audio.set_mic_gain",
                "Set the microphone gain in dB (0-24, default 18). Higher hears quieter speech but "
                "also more noise. 'More sensitive' means 3 dB above the current value (see "
                "self.audio.get_tuning). Saved. Tell the user the new value.",
                PropertyList({Property("db", kPropertyTypeInteger, 0, 24)}),
                [](const PropertyList& p) -> ToolResult {
                    auto& settings = WalleSettings::GetInstance();
                    auto result = settings.Set("mic_gain_db", std::to_string(p["db"].value<int>()));
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                    return *result;
                });

    mcp.AddTool("self.audio.set_wake_threshold",
                "Set how easily the wake word \"" WAKE_WORD_NAME "\" triggers: value from 0.50 to "
                "0.95 (default 0.52). Lower wakes more easily but may trigger by mistake. 'Wake up "
                "more easily' means 0.02 lower than now (see self.audio.get_tuning). Saved.",
                PropertyList({Property("value", kPropertyTypeString)}),
                [](const PropertyList& p) -> ToolResult {
                    auto& settings = WalleSettings::GetInstance();
                    auto result = settings.Set("wake_threshold", p["value"].value<std::string>());
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                    return *result;
                });

    mcp.AddTool("self.audio.get_tuning",
                "Report the microphone gain, wake word threshold and speaker volume limits.",
                PropertyList(), [](const PropertyList&) -> ToolResult {
                    auto& board = WallEBoard::Get();
                    auto& settings = WalleSettings::GetInstance();
                    auto* codec = board.walle_codec();
                    return "Mic gain " + std::to_string(codec->mic_gain_db()) +
                           " dB; wake threshold " +
                           settings.FormatCurrent(*settings.Find("wake_threshold")) +
                           " (while Jarvis talks it uses the model default); volume " +
                           std::to_string(codec->output_volume()) + ", max_volume " +
                           std::to_string(settings.GetInt("max_volume")) +
                           (board.QuietHoursActive() ? " (night mode cap 40 is active)." : ".");
                });

    // ---------------------------------------------------------------- settings
    mcp.AddTool("self.settings.list",
                "List all Jarvis settings with their value, range and meaning.", PropertyList(),
                [](const PropertyList&) -> ToolResult {
                    auto& settings = WalleSettings::GetInstance(); return settings.ListText(); });

    mcp.AddTool("self.settings.get", "Read one Jarvis setting. key: see self.settings.list.",
                PropertyList({Property("key", kPropertyTypeString)}),
                [](const PropertyList& p) -> ToolResult {
                    auto& settings = WalleSettings::GetInstance();
                    return settings.Describe(p["key"].value<std::string>());
                });

    mcp.AddTool("self.settings.set",
                "Change a Jarvis setting and save it; it applies immediately. key and value: see "
                "self.settings.list (numbers, 0/1, HH:MM or off for quiet hours, a city name for "
                "weather_city). Then tell the user what changed.",
                PropertyList({Property("key", kPropertyTypeString),
                              Property("value", kPropertyTypeString)}),
                [](const PropertyList& p) -> ToolResult {
                    auto& settings = WalleSettings::GetInstance();
                    auto result = settings.Set(p["key"].value<std::string>(),
                                               p["value"].value<std::string>());
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                    return *result;
                });

    mcp.AddTool("self.settings.reset", "Reset all Jarvis settings to their defaults.",
                PropertyList(), [](const PropertyList&) -> ToolResult {
                    auto& settings = WalleSettings::GetInstance();
                    settings.ResetAll();
                    return "All settings are back to their defaults.";
                });

    // ---------------------------------------------------------------- system
    mcp.AddTool("self.system.enter_standby",
                "Take a nap: Jarvis's screen goes dark and it rests, but it stays on Wi-Fi and "
                "still hears \"" WAKE_WORD_NAME "\". The nap starts when you finish replying. "
                "Saying the wake word or pressing the BOOT button wakes it.",
                PropertyList(), [](const PropertyList&) -> ToolResult {
                    auto& board = WallEBoard::Get();
                    board.RequestNap("voice");
                    return "Nap requested; it starts after this reply. Tell the user to say \"" WAKE_WORD_NAME
                           "\" to wake Jarvis.";
                });

    mcp.AddTool("self.system.get_standby", "Tell whether Jarvis is napping.", PropertyList(),
                [](const PropertyList&) -> ToolResult {
                    auto& board = WallEBoard::Get(); return board.IsNapping(); });

    // Upstream's tool (22 boards), plus stopping our LAN settings page first: it also listens on
    // port 80 (and httpd's default control port), where the WiFi setup page must start.
    mcp.AddTool("self.system.reconfigure_wifi",
                "End this conversation and enter WiFi configuration mode (Jarvis opens its own "
                "hotspot to pick a new network).\n**CAUTION** You must ask the user to confirm "
                "this action.",
                PropertyList(), [](const PropertyList&) -> ToolResult {
                    Application::GetInstance().Schedule([]() {
                        auto& board = WallEBoard::Get();
                        board.ExitNap();
                        walle_web::Stop();
                        board.EnterWifiConfigMode();
                    });
                    return true;
                });

    mcp.AddTool("self.system.deep_sleep",
                "Deep sleep to save battery: everything turns off, including Wi-Fi and the wake "
                "word. Only the BOOT button wakes Jarvis, or the timer if one is given. "
                "duration_minutes: 0-1440 (for example 2 hours = 120), or wake_at: 'HH:MM' in "
                "24-hour local time. Before it sleeps, tell the user how Jarvis can be woken.",
                PropertyList({Property("duration_minutes", kPropertyTypeInteger, 0, 0, 1440),
                              Property("wake_at", kPropertyTypeString, std::string(""))}),
                [](const PropertyList& p) -> ToolResult {
                    auto& board = WallEBoard::Get();
                    auto result = board.RequestDeepSleep(p["duration_minutes"].value<int>(),
                                                         p["wake_at"].value<std::string>());
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                    return *result;
                });

    // ---------------------------------------------------------------- timers
    mcp.AddTool("self.timer.set",
                "Set a local timer or alarm that rings on Jarvis (even while napping). For a "
                "countdown give duration_seconds (5-86400, for example 10 minutes = 600); for a "
                "clock alarm give at as 'HH:MM' (24-hour local time). label: a short name like "
                "'tea'. Up to 5 timers.",
                PropertyList({Property("duration_seconds", kPropertyTypeInteger, 0, 0, 86400),
                              Property("at", kPropertyTypeString, std::string("")),
                              Property("label", kPropertyTypeString, std::string(""))}),
                [](const PropertyList& p) -> ToolResult {
                    auto& timers = WalleTimers::GetInstance();
                    const auto at = p["at"].value<std::string>();
                    const auto label = p["label"].value<std::string>();
                    auto result = at.empty()
                                      ? timers.AddCountdown(p["duration_seconds"].value<int>(), label)
                                      : timers.AddAlarm(at, label);
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                    return *result;
                });

    mcp.AddTool("self.timer.list", "List Jarvis's running timers and alarms.", PropertyList(),
                [](const PropertyList&) -> ToolResult { return WalleTimers::GetInstance().List(); });

    mcp.AddTool("self.timer.cancel",
                "Cancel a timer or alarm by its label, or 'all'. Also silences a ringing alarm.",
                PropertyList({Property("label", kPropertyTypeString, std::string("all"))}),
                [](const PropertyList& p) -> ToolResult {
                    auto& board = WallEBoard::Get();
                    const bool was_ringing = board.IsRinging();
                    board.StopRinging();
                    auto result = WalleTimers::GetInstance().Cancel(p["label"].value<std::string>());
                    if (!result) {
                        return was_ringing ? ToolResult(std::string("Alarm silenced."))
                                           : ToolResult(std::unexpected(result.error()));
                    }
                    return *result;
                });

    // ---------------------------------------------------------------- weather
    mcp.AddTool("self.weather.get",
                "Current weather and today's temperature range for Jarvis's city (setting "
                "weather_city). If no city is set, ask the user for their city and save it with "
                "self.settings.set key=weather_city.",
                PropertyList(), [](const PropertyList&) -> ToolResult {
                    TaskPriorityReset priority_reset(1);
                    auto report = WalleWeather::GetInstance().Report();
                    if (!report) {
                        return std::unexpected(report.error());
                    }
                    return *report;
                });

    // ---------------------------------------------------------------- diagnostics
    mcp.AddTool("self.diagnostics.run_check",
                "Run one self-check and report the result in plain words. check: wifi, server, "
                "memory, camera, mic (listens for 2 seconds), speaker (plays a tone), display "
                "(flashes colors), motors (the wheels twitch briefly), or all.",
                PropertyList({Property("check", kPropertyTypeString)}),
                [](const PropertyList& p) -> ToolResult {
                    return walle_diagnostics::Run(p["check"].value<std::string>());
                });

    ESP_LOGI(TAG, "Jarvis tools registered");
}
