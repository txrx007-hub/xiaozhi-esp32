#include "walle_weather.h"

#include <cctype>
#include <cmath>
#include <cstdio>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "walle_settings.h"

#define TAG "WalleWeather"

namespace {

std::string UrlEncode(const std::string& text) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

// WMO weather interpretation codes used by Open-Meteo
const char* Describe(int code) {
    switch (code) {
        case 0: return "clear sky";
        case 1: return "mainly clear";
        case 2: return "partly cloudy";
        case 3: return "overcast";
        case 45: case 48: return "fog";
        case 51: case 53: case 55: return "drizzle";
        case 56: case 57: return "freezing drizzle";
        case 61: return "light rain";
        case 63: return "rain";
        case 65: return "heavy rain";
        case 66: case 67: return "freezing rain";
        case 71: return "light snow";
        case 73: return "snow";
        case 75: return "heavy snow";
        case 77: return "snow grains";
        case 80: case 81: return "rain showers";
        case 82: return "heavy rain showers";
        case 85: case 86: return "snow showers";
        case 95: return "thunderstorm";
        case 96: case 99: return "thunderstorm with hail";
        default: return "unknown weather";
    }
}

std::expected<std::string, std::string> HttpGet(const std::string& url) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return std::unexpected("no network");
    }
    auto http = network->CreateHttp(0);
    http->SetTimeout(8000);
    http->SetHeader("User-Agent", "wall-e-xiao");
    if (auto opened = http->Open("GET", url); !opened) {
        return std::unexpected("could not reach the weather service");
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        http->Close();
        return std::unexpected("the weather service answered with an error");
    }
    std::string body = http->ReadAll();
    http->Close();
    return body;
}

double NumberOr(const cJSON* obj, const char* key, double fallback) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

double FirstOr(const cJSON* obj, const char* key, double fallback) {
    const cJSON* array = cJSON_GetObjectItem(obj, key);
    const cJSON* first = cJSON_IsArray(array) ? cJSON_GetArrayItem(array, 0) : nullptr;
    return cJSON_IsNumber(first) ? first->valuedouble : fallback;
}

}  // namespace

std::expected<void, std::string> WalleWeather::Geocode(const std::string& city) {
    auto body = HttpGet("https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&"
                        "format=json&name=" + UrlEncode(city));
    if (!body) {
        return std::unexpected(body.error());
    }
    cJSON* root = cJSON_Parse(body->c_str());
    if (root == nullptr) {
        return std::unexpected("the weather service sent an unreadable answer");
    }
    const cJSON* results = cJSON_GetObjectItem(root, "results");
    const cJSON* first = cJSON_IsArray(results) ? cJSON_GetArrayItem(results, 0) : nullptr;
    if (first == nullptr) {
        cJSON_Delete(root);
        return std::unexpected("I could not find a place called " + city);
    }
    const cJSON* name = cJSON_GetObjectItem(first, "name");
    const cJSON* country = cJSON_GetObjectItem(first, "country");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latitude_ = NumberOr(first, "latitude", 0);
        longitude_ = NumberOr(first, "longitude", 0);
        place_name_ = cJSON_IsString(name) ? name->valuestring : city;
        if (cJSON_IsString(country)) {
            place_name_ += std::string(", ") + country->valuestring;
        }
        geocoded_city_ = city;
    }
    cJSON_Delete(root);
    return {};
}

std::expected<void, std::string> WalleWeather::Fetch() {
    const std::string city = WalleSettings::GetInstance().GetText("weather_city");
    if (city.empty()) {
        return std::unexpected(
            "no weather city is set; ask me to set weather_city, for example to Amsterdam");
    }
    bool need_geocode;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        need_geocode = geocoded_city_ != city;
    }
    if (need_geocode) {
        if (auto geo = Geocode(city); !geo) {
            return geo;
        }
    }
    char url[320];
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snprintf(url, sizeof(url),
                 "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                 "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
                 "weather_code,wind_speed_10m&daily=temperature_2m_max,temperature_2m_min,"
                 "precipitation_probability_max&timezone=auto&forecast_days=1",
                 latitude_, longitude_);
    }
    auto body = HttpGet(url);
    if (!body) {
        return std::unexpected(body.error());
    }
    cJSON* root = cJSON_Parse(body->c_str());
    const cJSON* current = root != nullptr ? cJSON_GetObjectItem(root, "current") : nullptr;
    const cJSON* daily = root != nullptr ? cJSON_GetObjectItem(root, "daily") : nullptr;
    if (current == nullptr) {
        cJSON_Delete(root);
        return std::unexpected("the weather service sent an unreadable answer");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        temperature_ = NumberOr(current, "temperature_2m", 0);
        feels_like_ = NumberOr(current, "apparent_temperature", temperature_);
        humidity_ = static_cast<int>(NumberOr(current, "relative_humidity_2m", 0));
        wind_kmh_ = NumberOr(current, "wind_speed_10m", 0);
        weather_code_ = static_cast<int>(NumberOr(current, "weather_code", -1));
        today_min_ = daily ? FirstOr(daily, "temperature_2m_min", temperature_) : temperature_;
        today_max_ = daily ? FirstOr(daily, "temperature_2m_max", temperature_) : temperature_;
        rain_chance_ =
            daily ? static_cast<int>(FirstOr(daily, "precipitation_probability_max", -1)) : -1;
        have_reading_ = true;
        fetched_at_us_ = esp_timer_get_time();
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Weather updated for %s", city.c_str());
    return {};
}

std::expected<std::string, std::string> WalleWeather::Report(int max_age_s) {
    bool stale;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stale = !have_reading_ ||
                esp_timer_get_time() - fetched_at_us_ > static_cast<int64_t>(max_age_s) * 1000000;
    }
    if (stale) {
        if (auto fetched = Fetch(); !fetched) {
            return std::unexpected("Weather unavailable: " + fetched.error() + ".");
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    char text[320];
    snprintf(text, sizeof(text),
             "Weather in %s: %s, %.0f degrees Celsius (feels like %.0f), humidity %d%%, wind "
             "%.0f km/h. Today between %.0f and %.0f degrees",
             place_name_.c_str(), Describe(weather_code_), temperature_, feels_like_, humidity_,
             wind_kmh_, today_min_, today_max_);
    std::string report = text;
    if (rain_chance_ >= 0) {
        report += ", " + std::to_string(rain_chance_) + "% chance of rain";
    }
    return report + ".";
}

std::string WalleWeather::ClockLine() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!have_reading_ ||
        esp_timer_get_time() - fetched_at_us_ > static_cast<int64_t>(3 * 3600) * 1000000) {
        return "";
    }
    char line[48];
    snprintf(line, sizeof(line), "%.0f C  %s", temperature_, Describe(weather_code_));
    std::string text = line;
    if (!text.empty()) {
        // Capitalise the description ("12 C  Partly cloudy").
        size_t at = text.find("  ");
        if (at != std::string::npos && at + 2 < text.size()) {
            text[at + 2] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[at + 2])));
        }
    }
    return text;
}

void WalleWeather::RefreshInBackground(int max_age_s) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool fresh = have_reading_ && esp_timer_get_time() - fetched_at_us_ <=
                                                static_cast<int64_t>(max_age_s) * 1000000;
        if (fetching_ || fresh || WalleSettings::GetInstance().GetText("weather_city").empty()) {
            return;
        }
        fetching_ = true;
    }
    // Short-lived task with an internal-RAM stack (TLS needs ~8 KB), deleted when done.
    xTaskCreate(
        [](void* arg) {
            auto* self = static_cast<WalleWeather*>(arg);
            if (auto result = self->Fetch(); !result) {
                ESP_LOGW(TAG, "Weather refresh failed: %s", result.error().c_str());
            }
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                self->fetching_ = false;
            }
            vTaskDelete(nullptr);
        },
        "weather", 8192, this, 2, nullptr);
}

void WalleWeather::Forget() {
    std::lock_guard<std::mutex> lock(mutex_);
    geocoded_city_.clear();
    have_reading_ = false;
}
