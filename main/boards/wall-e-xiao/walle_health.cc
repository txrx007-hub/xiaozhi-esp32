#include "walle_health.h"

#include <atomic>
#include <cmath>
#include <mutex>

#include <driver/temperature_sensor.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>

#define TAG "WalleHealth"

namespace walle_health {
namespace {

temperature_sensor_handle_t s_sensor = nullptr;
std::mutex s_sensor_mutex;
std::atomic<int> s_disconnects{0};
std::atomic<int> s_last_reason{0};

void OnWifiEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ++s_disconnects;
        if (data != nullptr) {
            s_last_reason = static_cast<wifi_event_sta_disconnected_t*>(data)->reason;
        }
    }
}

}  // namespace

void Init() {
    // -10..80 C is the sensor range with the smallest error on the ESP32-S3; the die normally sits between about 35 and 65 C.
    temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&config, &s_sensor);
    if (err == ESP_OK) {
        err = temperature_sensor_enable(s_sensor);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Temperature sensor not available: %s", esp_err_to_name(err));
        s_sensor = nullptr;
    }
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &OnWifiEvent, nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot count Wi-Fi disconnects: %s", esp_err_to_name(err));
    }
}

float ChipTempC() {
    std::lock_guard<std::mutex> lock(s_sensor_mutex);
    float celsius = NAN;
    if (s_sensor == nullptr || temperature_sensor_get_celsius(s_sensor, &celsius) != ESP_OK) {
        return NAN;
    }
    return celsius;
}

int WifiDisconnects() { return s_disconnects.load(); }

int LastDisconnectReason() { return s_last_reason.load(); }

}  // namespace walle_health
