#include "walle_settings.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_log.h>

#include "settings.h"
#include "walle_spectrum.h"

#define TAG "WalleSettings"

namespace {

constexpr const char* kNamespace = "walle";
constexpr const char* kLogLevels[] = {"error", "warn", "info", "debug"};
// self.display.set_visualizer; the names live in walle_spectrum.h next to the Mode enum.
constexpr const char* const* kVisualizerModes = walle_spectrum::kModeNames;
constexpr int kVisualizerModeCount = walle_spectrum::kModeCount;
// The picture that replaces the eyes while music plays.
constexpr const char* kMusicIcons[] = {"music icon 1", "music icon 2", "music icon 3", "music icon 4"};
constexpr int kMusicIconCount = 4;

std::string Trim(const std::string& text) {
    size_t start = 0;
    size_t end = text.size();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(start, end - start);
}

std::string Lower(std::string text) {
    for (auto& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return Trim(text);
}

bool ParseInt(const std::string& text, int& out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

// The name a setting is stored under in NVS. NVS keys are at most 15 characters (saving a longer one aborts with
// ESP_ERR_NVS_KEY_TOO_LONG), so a longer setting name gets a fixed short one here; the name everyone sees stays the same.
std::string StoreKey(const char* key) {
    static constexpr struct {
        const char* name;
        const char* stored;
    } kShort[] = {
        {"camera_brightness", "cam_brightness"},
        {"camera_saturation", "cam_saturation"},
    };
    for (const auto& s : kShort) {
        if (std::strcmp(key, s.name) == 0) {
            return s.stored;
        }
    }
    const std::string name(key);
    if (name.size() > 15) {   // a new long name without an entry above: still never crash, just store it shortened
        ESP_LOGE(TAG, "Setting name %s is too long for NVS; stored as %s (add it to StoreKey)", key, name.substr(0, 15).c_str());
        return name.substr(0, 15);
    }
    return name;
}

}  // namespace

WalleSettings::WalleSettings() {
    defs_ = {
        {"motor_max_speed", Kind::kInt, 20, 100, 100, "caps all movement, in percent"},
        {"motor_trim", Kind::kInt, -20, 20, 0,
         "wheel balance: raise it if Jarvis drifts left, lower it if it drifts right"},
        {"turn_ms_per_90", Kind::kInt, 100, 3000, 600, "milliseconds for a 90 degree turn at speed 60"},
        {"motor_a_invert", Kind::kBool, 0, 1, 0, "reverses motor A (right wheel)"},
        {"motor_b_invert", Kind::kBool, 0, 1, 0, "reverses motor B (left wheel)"},
        {"motor_swap", Kind::kBool, 0, 1, 0, "swaps the left and right motors"},
        {"soft_start_ms", Kind::kInt, 0, 1000, 200, "ramp-up time that limits current spikes"},
        {"talk_motion", Kind::kBool, 0, 1, 1,
         "small left and right body movements while Jarvis is speaking (never during music, a nap or a photo)"},
        {"talk_power", Kind::kInt, 20, 100, 80, "power of those talking movements, in percent (20 is a small twitch, 100 a big shake)"},
        {"follow_mode", Kind::kBool, 0, 1, 0,
         "keep turning towards your face (needs the own server; always off after a restart)"},
        {"face_power", Kind::kInt, 30, 100, 100,
         "power of the big turns towards your face and of follow mode, in percent (100 = full power); small corrections use 80% of it (never lower than 80); the length of every turn is worked out by Jarvis itself"},
        {"face_deadzone", Kind::kInt, 5, 40, 18,
         "how far off-centre your face may be, in percent of half the picture, before Jarvis turns at all (higher = calmer, lower = more exact)"},
        {"follow_ms", Kind::kInt, 100, 5000, 1000, "follow mode: pause between two looks, in milliseconds"},
        {"face_invert", Kind::kBool, 0, 1, 1, "swap left and right if Jarvis turns away from your face"},
        {"mic_gain_db", Kind::kInt, 0, 24, 18, "software microphone gain in dB"},
        {"wake_threshold", Kind::kHundredths, 50, 95, 52, "wake word threshold, lower wakes more easily"},
        {"shutter_sound", Kind::kBool, 0, 1, 1, "click sound when taking a photo"},
        {"camera_brightness", Kind::kInt, -5, 5, 2,
         "camera exposure target: raise it if photos come out dark, lower it if bright spots wash out (0 = sensor default)"},
        {"camera_saturation", Kind::kInt, -4, 4, 0,
         "camera colour saturation: higher for stronger colours, lower for paler ones (0 = sensor default)"},
        {"wake_chirp", Kind::kBool, 0, 1, 1, "short chirp right after the wake word"},
        {"quiet_start", Kind::kTimeOfDay, -1, 1439, -1, "night mode start, HH:MM or off"},
        {"quiet_end", Kind::kTimeOfDay, -1, 1439, -1, "night mode end, HH:MM or off"},
        {"idle_clock_min", Kind::kInt, 0, 120, 5, "show the big clock after this many idle minutes (0 = off)"},
        {"weather_city", Kind::kText, 0, 40, 0, "city for the weather, for example Amsterdam, or off"},
        {"visualizer_mode", Kind::kVisualizerMode, 0, kVisualizerModeCount - 1, 0,
         "what the screen shows while Jarvis speaks: off, winamp, rainbow, scope, radial, vu, mouth or orb"},
        {"music_icon", Kind::kMusicIcon, 0, kMusicIconCount - 1, 0,
         "the picture that replaces the eyes while music plays: music icon 1, 2, 3 or 4"},
        {"log_level", Kind::kLogLevel, 0, 3, 1, "serial log detail: error, warn, info or debug"},
    };

    // One-time migration: top speed and the shutter sound were removed from the portal and now
    // default to 100% / on. Drop values saved by earlier builds so the new defaults apply.
    {
        Settings migrate(kNamespace, true);
        if (migrate.GetInt("cfg_ver", 0) < 2) {
            migrate.EraseKey("motor_max_speed");
            migrate.EraseKey("shutter_sound");
            migrate.SetInt("cfg_ver", 2);
        }
        // v3: full power and full volume by default. The speaker volume cap (max_volume) is gone, and a motor cap saved
        // by an earlier build is dropped so the 100% default applies again.
        if (migrate.GetInt("cfg_ver", 0) < 3) {
            migrate.EraseKey("motor_max_speed");
            migrate.EraseKey("max_volume");
            migrate.SetInt("cfg_ver", 3);
        }
    }

    Settings settings(kNamespace, false);
    values_.reserve(defs_.size());
    for (const auto& def : defs_) {
        if (def.kind == Kind::kText) {
            texts_[def.key] = settings.GetString(StoreKey(def.key), "");
            values_.push_back(0);
            continue;
        }
        int value = settings.GetInt(StoreKey(def.key), def.def);
        if (value < def.min || value > def.max) {
            value = def.def;
        }
        values_.push_back(value);
    }
}

const WalleSettings::Def* WalleSettings::Find(const std::string& key) const {
    for (const auto& def : defs_) {
        if (key == def.key) {
            return &def;
        }
    }
    return nullptr;
}

int WalleSettings::GetInt(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < defs_.size(); ++i) {
        if (key == defs_[i].key) {
            return values_[i];
        }
    }
    ESP_LOGE(TAG, "Unknown setting %s", key.c_str());
    return 0;
}

std::string WalleSettings::GetText(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = texts_.find(key);
    return it == texts_.end() ? std::string() : it->second;
}

std::string WalleSettings::Format(const Def& def, int value) const {
    char buf[16];
    switch (def.kind) {
        case Kind::kHundredths:
            snprintf(buf, sizeof(buf), "%d.%02d", value / 100, value % 100);
            return buf;
        case Kind::kTimeOfDay:
            if (value < 0) {
                return "off";
            }
            snprintf(buf, sizeof(buf), "%02d:%02d", value / 60, value % 60);
            return buf;
        case Kind::kLogLevel:
            return kLogLevels[value < 0 ? 0 : (value > 3 ? 3 : value)];
        case Kind::kVisualizerMode:
            return kVisualizerModes[value < 0 ? 0
                                              : (value >= kVisualizerModeCount
                                                     ? kVisualizerModeCount - 1
                                                     : value)];
        case Kind::kMusicIcon:
            return kMusicIcons[value < 0 ? 0 : (value >= kMusicIconCount ? kMusicIconCount - 1 : value)];
        case Kind::kBool:
            return value ? "1" : "0";
        default:
            return std::to_string(value);
    }
}

std::string WalleSettings::FormatCurrent(const Def& def) const {
    if (def.kind == Kind::kText) {
        auto text = GetText(def.key);
        return text.empty() ? "off" : text;
    }
    return Format(def, GetInt(def.key));
}

std::string WalleSettings::Range(const Def& def) const {
    switch (def.kind) {
        case Kind::kHundredths:
            return Format(def, def.min) + "-" + Format(def, def.max);
        case Kind::kTimeOfDay:
            return "00:00-23:59 or off";
        case Kind::kLogLevel:
            return "error, warn, info, debug";
        case Kind::kVisualizerMode:
            return "off, winamp, scope, radial, vu, mouth, orb, rainbow";
        case Kind::kMusicIcon:
            return "music icon 1, music icon 2, music icon 3, music icon 4";
        case Kind::kBool:
            return "0 or 1";
        case Kind::kText:
            return "text, up to " + std::to_string(def.max) + " characters, or off";
        default:
            return std::to_string(def.min) + " to " + std::to_string(def.max);
    }
}

std::string WalleSettings::Describe(const std::string& key) const {
    const Def* def = Find(key);
    if (def == nullptr) {
        return "There is no setting called " + key + ".";
    }
    std::string text = std::string(def->key) + " = " + FormatCurrent(*def) + " (range " +
                       Range(*def);
    if (def->kind != Kind::kText) {
        text += ", default " + Format(*def, def->def);
    }
    return text + "): " + def->help;
}

std::string WalleSettings::ListText() const {
    std::string text;
    for (const auto& def : defs_) {
        text += Describe(def.key);
        text += "\n";
    }
    return text;
}

std::expected<int, std::string> WalleSettings::Parse(const Def& def,
                                                     const std::string& raw) const {
    const std::string text = Lower(raw);
    int value = 0;
    switch (def.kind) {
        case Kind::kBool:
            if (text == "1" || text == "on" || text == "true" || text == "yes") {
                return 1;
            }
            if (text == "0" || text == "off" || text == "false" || text == "no") {
                return 0;
            }
            return std::unexpected(std::string(def.key) + " must be 0 or 1.");

        case Kind::kHundredths: {
            char* end = nullptr;
            double number = std::strtod(text.c_str(), &end);
            if (text.empty() || end == text.c_str() || *end != '\0') {
                return std::unexpected(std::string(def.key) + " must be a number like " +
                                       Format(def, def.def) + ".");
            }
            // Accept both 0.52 and 52.
            value = number > 1.0 ? static_cast<int>(std::lround(number))
                                 : static_cast<int>(std::lround(number * 100.0));
            break;
        }

        case Kind::kTimeOfDay: {
            if (text == "off" || text == "none" || text == "-1") {
                return -1;
            }
            int hours = 0;
            int minutes = 0;
            char extra = 0;
            int fields = sscanf(text.c_str(), "%d:%d%c", &hours, &minutes, &extra);
            if (fields != 2) {
                if (!ParseInt(text, hours)) {
                    return std::unexpected(std::string(def.key) +
                                           " must be HH:MM (24 hour) or off.");
                }
                minutes = 0;
            }
            if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) {
                return std::unexpected(std::string(def.key) + " must be between 00:00 and 23:59.");
            }
            return hours * 60 + minutes;
        }

        case Kind::kLogLevel:
            for (int i = 0; i < 4; ++i) {
                if (text == kLogLevels[i] || text == std::to_string(i)) {
                    return i;
                }
            }
            if (text == "warning") {
                return 1;
            }
            return std::unexpected("log_level must be error, warn, info or debug.");

        case Kind::kVisualizerMode:
            for (int i = 0; i < kVisualizerModeCount; ++i) {
                if (text == kVisualizerModes[i] || text == std::to_string(i)) {
                    return i;
                }
            }
            return std::unexpected("visualizer_mode must be off, winamp, scope, radial, vu, mouth, orb or rainbow.");

        case Kind::kMusicIcon:
            for (int i = 0; i < kMusicIconCount; ++i) {
                if (text == kMusicIcons[i] || text == std::to_string(i + 1) || text == "icon " + std::to_string(i + 1)) {
                    return i;
                }
            }
            return std::unexpected("music_icon must be music icon 1, 2, 3 or 4.");

        default:
            if (!ParseInt(text, value)) {
                return std::unexpected(std::string(def.key) + " must be a whole number.");
            }
            break;
    }

    if (value < def.min || value > def.max) {
        return std::unexpected(std::string(def.key) + " must be between " + Format(def, def.min) +
                               " and " + Format(def, def.max) + "; " + Format(def, value) +
                               " is out of range.");
    }
    return value;
}

std::expected<std::string, std::string> WalleSettings::Set(const std::string& key,
                                                           const std::string& text) {
    const Def* def = Find(key);
    if (def == nullptr) {
        return std::unexpected("There is no setting called " + key +
                               ". Use self.settings.list to see them.");
    }

    if (def->kind == Kind::kText) {
        std::string value = Trim(text);
        const std::string lower = Lower(value);
        if (lower == "off" || lower == "none") {
            value.clear();
        }
        if (value.size() > static_cast<size_t>(def->max)) {
            return std::unexpected(std::string(def->key) + " can be at most " +
                                   std::to_string(def->max) + " characters.");
        }
        for (char c : value) {
            if (static_cast<unsigned char>(c) < 0x20 || c == '"' || c == '\\') {
                return std::unexpected(std::string(def->key) + " contains an invalid character.");
            }
        }
        std::string old_value;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            old_value = texts_[def->key];
            texts_[def->key] = value;
        }
        {
            Settings settings(kNamespace, true);
            settings.SetString(StoreKey(def->key), value);
        }
        if (on_changed_) {
            on_changed_(def->key);
        }
        auto show = [](const std::string& v) { return v.empty() ? std::string("off") : v; };
        return std::string(def->key) + " changed from " + show(old_value) + " to " +
               show(value) + ".";
    }

    auto parsed = Parse(*def, text);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }

    int old_value;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t index = def - defs_.data();
        old_value = values_[index];
        values_[index] = *parsed;
    }
    {
        Settings settings(kNamespace, true);
        settings.SetInt(StoreKey(def->key), *parsed);
    }
    ESP_LOGI(TAG, "%s: %s -> %s", def->key, Format(*def, old_value).c_str(),
             Format(*def, *parsed).c_str());
    if (on_changed_) {
        on_changed_(def->key);
    }
    if (old_value == *parsed) {
        return std::string(def->key) + " is already " + Format(*def, *parsed) + ".";
    }
    return std::string(def->key) + " changed from " + Format(*def, old_value) + " to " +
           Format(*def, *parsed) + ".";
}

void WalleSettings::ResetAll() {
    {
        Settings settings(kNamespace, true);
        settings.EraseAll();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < defs_.size(); ++i) {
            values_[i] = defs_[i].def;
        }
        for (auto& [key, value] : texts_) {
            value.clear();
        }
    }
    ESP_LOGI(TAG, "All settings reset to defaults");
    if (on_changed_) {
        for (const auto& def : defs_) {
            on_changed_(def.key);
        }
    }
}
