#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Local countdown timers and clock alarms, set by voice. When the clock is synced they are
// stored as absolute times in NVS, so they survive reboots and deep sleep.
class WalleTimers {
public:
    static constexpr int kMaxTimers = 5;

    static WalleTimers& GetInstance() {
        static WalleTimers instance;
        return instance;
    }

    std::expected<std::string, std::string> AddCountdown(int seconds, const std::string& label);
    std::expected<std::string, std::string> AddAlarm(const std::string& hhmm,
                                                     const std::string& label);
    std::expected<std::string, std::string> Cancel(const std::string& label);  // "all" = every one
    std::string List() const;

    // Seconds until the next timer is due, -1 if none (deep sleep wakes up in time for it).
    int64_t SecondsUntilNext() const;

    // Called about once per second. Fires due timers through the callback.
    void Tick();
    void OnFire(std::function<void(const std::string& label)> callback) {
        on_fire_ = std::move(callback);
    }

    static bool ClockSynced();

private:
    struct Timer {
        std::string label;
        int64_t due_epoch = 0;  // local-time epoch seconds when the clock is synced
        int64_t due_mono_ms = 0;  // fallback when the clock is not synced (not saved)
    };

    WalleTimers();
    void SaveLocked() const;
    std::string DescribeLocked(const Timer& timer) const;
    std::string UniqueLabelLocked(const std::string& wanted, const char* fallback) const;

    mutable std::mutex mutex_;
    std::vector<Timer> timers_;
    std::function<void(const std::string&)> on_fire_;
};
