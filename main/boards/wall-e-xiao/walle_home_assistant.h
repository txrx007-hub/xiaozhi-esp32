#pragma once

#include <cstdint>
#include <expected>
#include <mutex>
#include <string>
#include <vector>

// Minimal Home Assistant REST client: reachability probe, entity listing (so voice commands can
// look an entity up by name) and turn_on/turn_off/toggle/state via HA's generic "homeassistant"
// domain services, which proxy to the right real service for most controllable entity types
// (lights, switches, fans, scripts, input_booleans, media players; covers/locks have their own
// open/close or lock/unlock semantics and are read-only here via GetState).
// Needs ha_enabled=1 plus ha_url and a long-lived ha_token (WalleSettings, set from the LAN
// settings page - see walle_web.cc, which never echoes ha_token back once it is set).
class WalleHomeAssistant {
public:
    struct Entity {
        std::string entity_id;
        std::string friendly_name;
        std::string state;
    };

    static WalleHomeAssistant& GetInstance() {
        static WalleHomeAssistant instance;
        return instance;
    }

    bool IsConfigured() const;
    // Cached reachability, refreshed at most once per max_age_s so the settings page and the
    // MCP tools' own checks agree on whether Home Assistant commands should be offered.
    bool IsReachable(int max_age_s = 60);
    void Forget();  // ha_url / ha_token / ha_enabled changed: drop the cached probe

    // Lights, switches, fans and similar controllable entities, newest states from HA. Capped
    // to keep the answer short enough to read out and cheap to send to the AI backend.
    std::expected<std::vector<Entity>, std::string> ListEntities();
    std::expected<std::string, std::string> TurnOn(const std::string& entity_id);
    std::expected<std::string, std::string> TurnOff(const std::string& entity_id);
    std::expected<std::string, std::string> Toggle(const std::string& entity_id);
    std::expected<std::string, std::string> GetState(const std::string& entity_id);

private:
    WalleHomeAssistant() = default;
    std::expected<std::string, std::string> CallService(const std::string& service,
                                                         const std::string& entity_id);
    std::expected<void, std::string> Probe();

    std::mutex mutex_;
    bool have_probe_ = false;
    bool reachable_ = false;
    int64_t probed_at_us_ = 0;
};
