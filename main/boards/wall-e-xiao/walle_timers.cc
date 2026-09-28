#include "walle_timers.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <esp_log.h>
#include <esp_timer.h>

#include "settings.h"

#define TAG "WalleTimers"

namespace {

constexpr const char* kNamespace = "walle_tmr";
constexpr int64_t kMissedGraceSeconds = 10 * 60;  // ring late timers up to 10 minutes late

int64_t NowMonoMs() { return esp_timer_get_time() / 1000; }

std::string CleanLabel(const std::string& text) {
    std::string label;
    for (char c : text) {
        if (c == '|' || static_cast<unsigned char>(c) < 0x20) {
            continue;
        }
        label += c;
    }
    while (!label.empty() && std::isspace(static_cast<unsigned char>(label.back()))) {
        label.pop_back();
    }
    size_t start = 0;
    while (start < label.size() && std::isspace(static_cast<unsigned char>(label[start]))) {
        ++start;
    }
    label = label.substr(start);
    if (label.size() > 24) {
        label.resize(24);
    }
    return label;
}

std::string FormatDuration(int64_t seconds) {
    if (seconds < 60) {
        return std::to_string(seconds) + " seconds";
    }
    const int64_t minutes = (seconds + 30) / 60;
    if (minutes < 60) {
        return std::to_string(minutes) + (minutes == 1 ? " minute" : " minutes");
    }
    const int64_t hours = minutes / 60;
    const int64_t rest = minutes % 60;
    std::string text = std::to_string(hours) + (hours == 1 ? " hour" : " hours");
    if (rest > 0) {
        text += " " + std::to_string(rest) + (rest == 1 ? " minute" : " minutes");
    }
    return text;
}

}  // namespace

bool WalleTimers::ClockSynced() {
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    return local.tm_year >= 2025 - 1900;
}

WalleTimers::WalleTimers() {
    Settings settings(kNamespace, false);
    const int count = settings.GetInt("count", 0);
    for (int i = 0; i < count && i < kMaxTimers; ++i) {
        const std::string entry = settings.GetString("t" + std::to_string(i), "");
        const size_t bar = entry.find('|');
        if (bar == std::string::npos) {
            continue;
        }
        Timer timer;
        timer.due_epoch = std::strtoll(entry.substr(0, bar).c_str(), nullptr, 10);
        timer.label = entry.substr(bar + 1);
        if (timer.due_epoch > 0) {
            timers_.push_back(timer);
        }
    }
    if (!timers_.empty()) {
        ESP_LOGI(TAG, "Restored %d timer(s)", static_cast<int>(timers_.size()));
    }
}

void WalleTimers::SaveLocked() const {
    Settings settings(kNamespace, true);
    int saved = 0;
    for (const auto& timer : timers_) {
        if (timer.due_epoch <= 0) {
            continue;  // unsynced countdowns live in RAM only
        }
        settings.SetString("t" + std::to_string(saved),
                           std::to_string(timer.due_epoch) + "|" + timer.label);
        ++saved;
    }
    settings.SetInt("count", saved);
}

std::string WalleTimers::UniqueLabelLocked(const std::string& wanted, const char* fallback) const {
    std::string base = CleanLabel(wanted);
    if (base.empty()) {
        base = fallback;
    }
    std::string label = base;
    for (int n = 2; n < 10; ++n) {
        bool taken = std::any_of(timers_.begin(), timers_.end(),
                                 [&](const Timer& t) { return t.label == label; });
        if (!taken) {
            break;
        }
        label = base + " " + std::to_string(n);
    }
    return label;
}

std::string WalleTimers::DescribeLocked(const Timer& timer) const {
    int64_t remaining;
    std::string at;
    if (timer.due_epoch > 0) {
        remaining = timer.due_epoch - static_cast<int64_t>(time(nullptr));
        time_t due = static_cast<time_t>(timer.due_epoch);
        struct tm local;
        localtime_r(&due, &local);
        char buf[16];
        strftime(buf, sizeof(buf), "%H:%M", &local);
        at = std::string(" (at ") + buf + ")";
    } else {
        remaining = (timer.due_mono_ms - NowMonoMs()) / 1000;
    }
    if (remaining < 0) {
        remaining = 0;
    }
    return timer.label + ": in " + FormatDuration(remaining) + at;
}

std::expected<std::string, std::string> WalleTimers::AddCountdown(int seconds,
                                                                  const std::string& label) {
    if (seconds < 5 || seconds > 24 * 3600) {
        return std::unexpected("A timer must be between 5 seconds and 24 hours.");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(timers_.size()) >= kMaxTimers) {
        return std::unexpected("I can keep at most 5 timers. Cancel one first.");
    }
    Timer timer;
    timer.label = UniqueLabelLocked(label, "timer");
    if (ClockSynced()) {
        timer.due_epoch = static_cast<int64_t>(time(nullptr)) + seconds;
    } else {
        timer.due_mono_ms = NowMonoMs() + static_cast<int64_t>(seconds) * 1000;
    }
    timers_.push_back(timer);
    SaveLocked();
    return "Timer set. " + DescribeLocked(timer) + ".";
}

std::expected<std::string, std::string> WalleTimers::AddAlarm(const std::string& hhmm,
                                                              const std::string& label) {
    int hours = -1;
    int minutes = -1;
    char extra = 0;
    if (sscanf(hhmm.c_str(), "%d:%d%c", &hours, &minutes, &extra) != 2 || hours < 0 ||
        hours > 23 || minutes < 0 || minutes > 59) {
        return std::unexpected("The alarm time must be HH:MM in 24 hour format, like 07:30.");
    }
    if (!ClockSynced()) {
        return std::unexpected("I don't know the time yet, so I can't set a clock alarm.");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(timers_.size()) >= kMaxTimers) {
        return std::unexpected("I can keep at most 5 timers. Cancel one first.");
    }
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    int64_t delta = static_cast<int64_t>(hours * 60 + minutes) * 60 -
                    (local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
    if (delta <= 0) {
        delta += 24 * 3600;  // next occurrence
    }
    Timer timer;
    timer.label = UniqueLabelLocked(label, "alarm");
    timer.due_epoch = static_cast<int64_t>(now) + delta;
    timers_.push_back(timer);
    SaveLocked();
    return "Alarm set. " + DescribeLocked(timer) + ".";
}

std::expected<std::string, std::string> WalleTimers::Cancel(const std::string& label) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (timers_.empty()) {
        return std::unexpected("There are no timers to cancel.");
    }
    const std::string wanted = CleanLabel(label);
    if (wanted.empty() || wanted == "all") {
        const int count = static_cast<int>(timers_.size());
        timers_.clear();
        SaveLocked();
        return "Cancelled " + std::to_string(count) + (count == 1 ? " timer." : " timers.");
    }
    auto it = std::find_if(timers_.begin(), timers_.end(),
                           [&](const Timer& t) { return t.label == wanted; });
    if (it == timers_.end()) {
        return std::unexpected("There is no timer called " + wanted + ". " + List());
    }
    timers_.erase(it);
    SaveLocked();
    return "Cancelled the " + wanted + " timer.";
}

std::string WalleTimers::List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (timers_.empty()) {
        return "No timers are running.";
    }
    std::string text = "Timers: ";
    for (size_t i = 0; i < timers_.size(); ++i) {
        if (i > 0) {
            text += "; ";
        }
        text += DescribeLocked(timers_[i]);
    }
    return text + ".";
}

int64_t WalleTimers::SecondsUntilNext() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t best = -1;
    const int64_t now = static_cast<int64_t>(time(nullptr));
    for (const auto& timer : timers_) {
        const int64_t remaining = timer.due_epoch > 0 ? timer.due_epoch - now
                                                      : (timer.due_mono_ms - NowMonoMs()) / 1000;
        if (best < 0 || remaining < best) {
            best = std::max<int64_t>(remaining, 0);
        }
    }
    return best;
}

void WalleTimers::Tick() {
    std::vector<std::string> fired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (timers_.empty()) {
            return;
        }
        const bool synced = ClockSynced();
        const int64_t now = static_cast<int64_t>(time(nullptr));
        const int64_t mono = NowMonoMs();
        bool changed = false;
        for (auto it = timers_.begin(); it != timers_.end();) {
            bool due = false;
            bool missed = false;
            if (it->due_epoch > 0) {
                if (synced && now >= it->due_epoch) {
                    due = true;
                    missed = now - it->due_epoch > kMissedGraceSeconds;
                }
            } else if (mono >= it->due_mono_ms) {
                due = true;
            }
            if (!due) {
                ++it;
                continue;
            }
            if (missed) {
                ESP_LOGW(TAG, "Timer %s was missed while the robot was off", it->label.c_str());
            } else {
                fired.push_back(it->label);
            }
            it = timers_.erase(it);
            changed = true;
        }
        if (changed) {
            SaveLocked();
        }
    }
    for (const auto& label : fired) {
        ESP_LOGI(TAG, "Timer %s is due", label.c_str());
        if (on_fire_) {
            on_fire_(label);
        }
    }
}
