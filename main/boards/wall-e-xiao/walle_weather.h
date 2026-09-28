#pragma once

#include <cstdint>
#include <expected>
#include <mutex>
#include <string>

// Current weather from Open-Meteo (free, no API key) for the city in setting weather_city.
// Sends only the city name (for geocoding) and its coordinates to open-meteo.com.
class WalleWeather {
public:
    static WalleWeather& GetInstance() {
        static WalleWeather instance;
        return instance;
    }

    // Plain sentence for the AI. Refreshes if the cached reading is older than max_age_s.
    std::expected<std::string, std::string> Report(int max_age_s = 30 * 60);
    // Short line for the idle clock, e.g. "12 C  Cloudy". Empty if unknown.
    std::string ClockLine();
    // Refresh in a short-lived background task if the cache is stale.
    void RefreshInBackground(int max_age_s = 30 * 60);
    void Forget();  // city changed

private:
    WalleWeather() = default;
    std::expected<void, std::string> Fetch();  // blocking HTTPS
    std::expected<void, std::string> Geocode(const std::string& city);

    std::mutex mutex_;
    std::string geocoded_city_;
    std::string place_name_;
    double latitude_ = 0;
    double longitude_ = 0;
    bool have_reading_ = false;
    int64_t fetched_at_us_ = 0;
    float temperature_ = 0;
    float feels_like_ = 0;
    int humidity_ = 0;
    float wind_kmh_ = 0;
    int weather_code_ = -1;
    float today_min_ = 0;
    float today_max_ = 0;
    int rain_chance_ = -1;
    bool fetching_ = false;
};
