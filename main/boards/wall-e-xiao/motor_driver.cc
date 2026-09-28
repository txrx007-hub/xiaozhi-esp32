#include "motor_driver.h"

#include <algorithm>
#include <cstdlib>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_private/esp_gpio_reserve.h>
#include <esp_timer.h>
#include <soc/soc_caps.h>

#include "config.h"
#include "walle_settings.h"

#define TAG "MotorDriver"

namespace {

constexpr gpio_num_t kPins[4] = {MOTOR_IN1_PIN, MOTOR_IN2_PIN, MOTOR_IN3_PIN, MOTOR_IN4_PIN};
constexpr ledc_channel_t kChannels[4] = {LEDC_CHANNEL_2, LEDC_CHANNEL_3, LEDC_CHANNEL_4,
                                         LEDC_CHANNEL_5};
constexpr ledc_timer_t kTimer = LEDC_TIMER_1;
constexpr ledc_mode_t kMode = LEDC_LOW_SPEED_MODE;
constexpr uint32_t kPwmHz = 20000;  // above hearing, so the microphone does not pick up whine
constexpr ledc_timer_bit_t kResolution = LEDC_TIMER_10_BIT;
constexpr uint32_t kMaxDuty = (1u << 10) - 1;
// N20 gear motors do not turn below roughly a third of full duty, so commands start there.
constexpr int kMinDutyPercent = 35;
constexpr int kTickMs = 10;

int64_t NowMs() { return esp_timer_get_time() / 1000; }

int Sign(int v) { return (v > 0) - (v < 0); }

// Runs before app_main so the wheels do not twitch while the rest of the firmware starts.
struct EarlyMotorsOff {
    EarlyMotorsOff() { MotorDriver::EarlyInit(); }
} s_early_motors_off;

}  // namespace

void MotorDriver::EarlyInit() {
    gpio_config_t io = {};
    for (auto pin : kPins) {
        io.pin_bit_mask |= 1ULL << pin;
    }
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
    for (auto pin : kPins) {
        gpio_set_level(pin, 0);
        gpio_hold_dis(pin);  // released only after the pin is already driven low
    }
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_dis();
#endif
    // gpio_config() above marks these pins as exclusively claimed by the plain-GPIO driver.
    // MotorDriver's constructor reclaims the same 4 pins for LEDC moments later; without this
    // revoke, ESP-IDF's GPIO-conflict tracker still shows them as taken and LEDC logs a
    // harmless but confusing "GPIO N is not usable, maybe conflict with others" warning. This
    // only clears that bookkeeping; the pins stay physically driven low the whole time.
    uint64_t mask = 0;
    for (auto pin : kPins) {
        mask |= 1ULL << pin;
    }
    esp_gpio_revoke(mask);
}

void MotorDriver::HoldLowForDeepSleep() {
    EarlyInit();  // routes the pins back to plain GPIO, driven low
    for (auto pin : kPins) {
        gpio_hold_en(pin);
    }
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_en();
#endif
}

MotorDriver::MotorDriver() {
    ledc_timer_config_t timer = {};
    timer.speed_mode = kMode;
    timer.duty_resolution = kResolution;
    timer.timer_num = kTimer;
    timer.freq_hz = kPwmHz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    for (int i = 0; i < 4; ++i) {
        ledc_channel_config_t channel = {};
        channel.gpio_num = kPins[i];
        channel.speed_mode = kMode;
        channel.channel = kChannels[i];
        channel.timer_sel = kTimer;
        channel.duty = 0;
        channel.hpoint = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&channel));
    }

    wake_ = xSemaphoreCreateBinary();
    xTaskCreate(
        [](void* arg) {
            static_cast<MotorDriver*>(arg)->Task();
            vTaskDelete(nullptr);
        },
        "motors", 3072, this, 4, &task_);
    ESP_LOGI(TAG, "Motor PWM ready: %lu Hz on GPIO%d/%d/%d/%d", (unsigned long)kPwmHz, kPins[0],
             kPins[1], kPins[2], kPins[3]);
}

void MotorDriver::SetMotorRaw(int motor, int duty_percent) {
    duty_percent = std::clamp(duty_percent, -100, 100);
    const uint32_t duty = kMaxDuty * std::abs(duty_percent) / 100;
    const int fwd = motor * 2;
    const int rev = fwd + 1;
    ledc_set_duty(kMode, kChannels[fwd], duty_percent > 0 ? duty : 0);
    ledc_update_duty(kMode, kChannels[fwd]);
    ledc_set_duty(kMode, kChannels[rev], duty_percent < 0 ? duty : 0);
    ledc_update_duty(kMode, kChannels[rev]);
}

void MotorDriver::Brake() {
    // IN1=IN2=high shorts each motor for a moment so it stops at once, then coast.
    for (auto channel : kChannels) {
        ledc_set_duty(kMode, channel, kMaxDuty);
        ledc_update_duty(kMode, channel);
    }
    vTaskDelay(pdMS_TO_TICKS(40));
    for (auto channel : kChannels) {
        ledc_set_duty(kMode, channel, 0);
        ledc_update_duty(kMode, channel);
    }
}

void MotorDriver::SetWheels(int left, int right) {
    auto& settings = WalleSettings::GetInstance();
    const int max_speed = settings.GetInt("motor_max_speed");
    const int trim = settings.GetInt("motor_trim");

    int l = std::clamp(left, -100, 100) * max_speed / 100;
    int r = std::clamp(right, -100, 100) * max_speed / 100;
    // Positive trim slows the right wheel (fixes a drift to the left), negative the left.
    if (trim > 0) {
        r = r * (100 - trim) / 100;
    } else if (trim < 0) {
        l = l * (100 + trim) / 100;
    }

    auto to_duty = [](int v) {
        if (v == 0) {
            return 0;
        }
        const int magnitude = kMinDutyPercent + (100 - kMinDutyPercent) * std::abs(v) / 100;
        return v > 0 ? magnitude : -magnitude;
    };

    // Default (motor_swap = 0), as wired in the diagram: A = right wheel, B = left wheel.
    const bool swap = settings.GetBool("motor_swap");
    int a = to_duty(swap ? l : r);
    int b = to_duty(swap ? r : l);
    if (settings.GetBool("motor_a_invert")) {
        a = -a;
    }
    if (settings.GetBool("motor_b_invert")) {
        b = -b;
    }
    SetMotorRaw(0, a);
    SetMotorRaw(1, b);
}

int MotorDriver::QueuedMsLocked() const {
    int total = 0;
    for (const auto& step : queue_) {
        total += step.duration_ms;
    }
    return total;
}

bool MotorDriver::Run(const std::vector<Step>& steps, bool append) {
    int requested = 0;
    for (const auto& step : steps) {
        requested += step.duration_ms;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!append) {
            queue_.clear();
            generation_.fetch_add(1);  // interrupt the running step
        }
        if (QueuedMsLocked() + requested > kMaxQueuedMs) {
            return false;
        }
        for (const auto& step : steps) {
            queue_.push_back(step);
        }
    }
    xSemaphoreGive(wake_);
    return true;
}

bool MotorDriver::RunAndWait(const std::vector<Step>& steps, int timeout_ms) {
    const uint32_t start_generation = generation_.load() + 1;  // Run() bumps it once
    if (!Run(steps, false)) {
        return false;
    }
    const int64_t deadline = NowMs() + timeout_ms;
    vTaskDelay(pdMS_TO_TICKS(30));
    while (NowMs() < deadline) {
        bool empty;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            empty = queue_.empty();
        }
        if (generation_.load() != start_generation) {
            return false;  // stopped or replaced
        }
        if (empty && !busy_.load()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    Stop();
    return false;
}

void MotorDriver::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        generation_.fetch_add(1);
    }
    Brake();
    current_left_ = 0;
    current_right_ = 0;
    xSemaphoreGive(wake_);
}

std::string MotorDriver::Status() const {
    int queued_ms;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queued_ms = QueuedMsLocked();
    }
    if (!busy_.load() && queued_ms == 0) {
        return "stopped";
    }
    return "moving (left " + std::to_string(current_left_) + "%, right " +
           std::to_string(current_right_) + "%, " + std::to_string(queued_ms) +
           " ms more queued)";
}

void MotorDriver::Task() {
    while (true) {
        xSemaphoreTake(wake_, portMAX_DELAY);
        while (true) {
            Step step;
            uint32_t generation;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.empty()) {
                    break;
                }
                step = queue_.front();
                queue_.pop_front();
                generation = generation_.load();
            }
            busy_ = true;

            const int64_t start = NowMs();
            const int64_t end = start + step.duration_ms;

            if (step.raw_motor >= 0) {
                SetMotorRaw(step.raw_motor, step.left);
                SetMotorRaw(1 - step.raw_motor, 0);
                while (NowMs() < end && generation_.load() == generation) {
                    vTaskDelay(pdMS_TO_TICKS(kTickMs));
                }
                SetMotorRaw(step.raw_motor, 0);
                continue;
            }

            // Soft start: ramp up when a wheel speeds up or reverses; slowing down is instant.
            const int soft_ms = WalleSettings::GetInstance().GetInt("soft_start_ms");
            auto ramp_origin = [](int from, int to) {
                if (Sign(from) == Sign(to) && std::abs(to) <= std::abs(from)) {
                    return to;
                }
                return Sign(from) == Sign(to) ? from : 0;
            };
            const int from_left = ramp_origin(current_left_, step.left);
            const int from_right = ramp_origin(current_right_, step.right);

            while (generation_.load() == generation) {
                const int64_t now = NowMs();
                if (now >= end) {
                    break;
                }
                const int elapsed = static_cast<int>(now - start);
                const int left = (soft_ms > 0 && elapsed < soft_ms)
                                     ? from_left + (step.left - from_left) * elapsed / soft_ms
                                     : step.left;
                const int right = (soft_ms > 0 && elapsed < soft_ms)
                                      ? from_right + (step.right - from_right) * elapsed / soft_ms
                                      : step.right;
                if (left != current_left_ || right != current_right_) {
                    SetWheels(left, right);
                    current_left_ = left;
                    current_right_ = right;
                }
                vTaskDelay(pdMS_TO_TICKS(kTickMs));
            }
        }
        // Queue empty (or stopped): coast.
        SetWheels(0, 0);
        current_left_ = 0;
        current_right_ = 0;
        busy_ = false;
    }
}
