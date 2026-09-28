#pragma once

#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Wall-E settings, stored in NVS namespace "walle" and adjustable by voice or the USB console.
// Numbers are ints internally: hundredths for wake_threshold, minutes of the day (-1 = off)
// for the quiet hours, 0-3 for log_level. Text settings (weather_city) are strings.
class WalleSettings {
public:
    enum class Kind { kInt, kBool, kHundredths, kTimeOfDay, kLogLevel, kText, kVisualizerMode };

    struct Def {
        const char* key;
        Kind kind;
        int min;  // for kText: unused
        int max;  // for kText: maximum length
        int def;
        const char* help;
    };

    static WalleSettings& GetInstance() {
        static WalleSettings instance;
        return instance;
    }

    int GetInt(const std::string& key) const;
    bool GetBool(const std::string& key) const { return GetInt(key) != 0; }
    float GetHundredths(const std::string& key) const { return GetInt(key) / 100.0f; }
    std::string GetText(const std::string& key) const;

    // Parses, validates and stores a value, then notifies the listener.
    // Returns one plain sentence about the change, or why it was rejected.
    std::expected<std::string, std::string> Set(const std::string& key, const std::string& value);
    void ResetAll();

    const std::vector<Def>& Defs() const { return defs_; }
    const Def* Find(const std::string& key) const;
    std::string FormatCurrent(const Def& def) const;
    std::string Range(const Def& def) const;
    std::string Describe(const std::string& key) const;
    std::string ListText() const;

    void OnChanged(std::function<void(const std::string& key)> callback) {
        on_changed_ = std::move(callback);
    }

private:
    WalleSettings();
    std::string Format(const Def& def, int value) const;
    std::expected<int, std::string> Parse(const Def& def, const std::string& text) const;

    std::vector<Def> defs_;
    std::vector<int> values_;
    std::map<std::string, std::string> texts_;
    mutable std::mutex mutex_;
    std::function<void(const std::string&)> on_changed_;
};
