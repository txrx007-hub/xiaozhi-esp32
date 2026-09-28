#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// Two N20 gear motors on a Mini L298N / MX1508 (IN1-IN4, no enable pins), driven by LEDC PWM.
// Commands are per wheel, -100..100 percent of motor_max_speed (+ is forward). Every step
// has a duration and the driver coasts when the queue runs out; Stop() always wins.
class MotorDriver {
public:
    struct Step {
        int left;             // wheel command, or the raw duty when raw_motor >= 0
        int right;
        int duration_ms;
        int raw_motor = -1;   // diagnostics: drive only motor 0 (A) or 1 (B), no mapping
    };

    // Drives IN1-IN4 low. Runs from a static initializer, before the board exists, and
    // releases the deep-sleep hold after a wake-up.
    static void EarlyInit();
    // Parks IN1-IN4 low and holds them through deep sleep.
    static void HoldLowForDeepSleep();

    MotorDriver();

    // Queues steps. Replaces queued steps unless append is true. Returns false if the
    // total queued time would exceed kMaxQueuedMs.
    bool Run(const std::vector<Step>& steps, bool append);
    // Runs steps and waits for them (used by look_around). False if stopped or timed out.
    bool RunAndWait(const std::vector<Step>& steps, int timeout_ms);
    void Stop();
    bool IsBusy() const { return busy_.load(); }
    std::string Status() const;

    static constexpr int kMaxQueuedMs = 15000;

private:
    void Task();
    void SetWheels(int left, int right);   // applies speed cap, trim, swap and invert
    void SetMotorRaw(int motor, int duty_percent);
    void Brake();
    int QueuedMsLocked() const;

    mutable std::mutex mutex_;
    std::deque<Step> queue_;
    SemaphoreHandle_t wake_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> busy_{false};
    std::atomic<bool> abort_{false};
    std::atomic<uint32_t> generation_{0};
    int current_left_ = 0;
    int current_right_ = 0;
};
