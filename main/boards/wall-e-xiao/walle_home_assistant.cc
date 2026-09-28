#include "walle_home_assistant.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "board.h"
#include "walle_settings.h"

#define TAG "WalleHA"

namespace {

constexpr size_t kMaxEntities = 60;
// Domains offered for voice control; others (climate, cover, lock, alarm_control_panel, ...)
// have modes and safety implications a generic turn_on/off/toggle would get wrong.
const char* kControllableDomains[] = {"light", "switch", "fan", "input_boolean", "script",
                                      "scene", "media_player", "automation"};

bool IsControllableDomain(const std::string& entity_id) {
    const auto dot = entity_id.find('.');
    if (dot == std::string::npos) {
        return false;
    }
    const std::string domain = entity_id.substr(0, dot);
    for (const char* candidate : kControllableDomains) {
        if (domain == candidate) {
            return true;
        }
    }
    return false;
}

std::string BaseUrl() {
    std::string url = WalleSettings::GetInstance().GetText("ha_url");
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

// GET or POST (with an optional JSON body) against a Home Assistant REST endpoint (path starts
// with "/api/"), with the long-lived access token as a bearer header.
std::expected<std::string, std::string> Request(const std::string& method, const std::string& path,
                                                 const std::string& json_body = "") {
    const std::string base = BaseUrl();
    const std::string token = WalleSettings::GetInstance().GetText("ha_token");
    if (base.empty() || token.empty()) {
        return std::unexpected("Home Assistant is not configured (set ha_url and ha_token on the "
                               "settings page).");
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return std::unexpected("no network");
    }
    auto http = network->CreateHttp(0);
    http->SetTimeout(6000);
    http->SetHeader("Authorization", "Bearer " + token);
    http->SetHeader("User-Agent", "wall-e-xiao");
    if (!json_body.empty()) {
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::string(json_body));
    }
    if (auto opened = http->Open(method, base + path); !opened) {
        return std::unexpected("could not reach Home Assistant at " + base);
    }
    auto status = http->GetStatusCode();
    if (status && (*status == 401 || *status == 403)) {
        http->Close();
        return std::unexpected("Home Assistant rejected the access token");
    }
    if (!status || *status < 200 || *status >= 300) {
        const std::string code = status ? std::to_string(*status) : std::string("unknown");
        http->Close();
        return std::unexpected("Home Assistant answered with an error (status " + code + ")");
    }
    std::string body = http->ReadAll();
    http->Close();
    return body;
}

}  // namespace

bool WalleHomeAssistant::IsConfigured() const {
    auto& settings = WalleSettings::GetInstance();
    return settings.GetBool("ha_enabled") && !settings.GetText("ha_url").empty() &&
           !settings.GetText("ha_token").empty();
}

std::expected<void, std::string> WalleHomeAssistant::Probe() {
    auto body = Request("GET", "/api/");
    if (!body) {
        return std::unexpected(body.error());
    }
    return {};
}

bool WalleHomeAssistant::IsReachable(int max_age_s) {
    if (!IsConfigured()) {
        std::lock_guard<std::mutex> lock(mutex_);
        have_probe_ = false;
        reachable_ = false;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (have_probe_ &&
            esp_timer_get_time() - probed_at_us_ < static_cast<int64_t>(max_age_s) * 1000000) {
            return reachable_;
        }
    }
    auto probed = Probe();
    if (!probed) {
        ESP_LOGW(TAG, "Probe failed: %s", probed.error().c_str());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    have_probe_ = true;
    reachable_ = static_cast<bool>(probed);
    probed_at_us_ = esp_timer_get_time();
    return reachable_;
}

void WalleHomeAssistant::Forget() {
    std::lock_guard<std::mutex> lock(mutex_);
    have_probe_ = false;
}

std::expected<std::vector<WalleHomeAssistant::Entity>, std::string>
WalleHomeAssistant::ListEntities() {
    auto body = Request("GET", "/api/states");
    if (!body) {
        return std::unexpected(body.error());
    }
    cJSON* root = cJSON_Parse(body->c_str());
    if (root == nullptr || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return std::unexpected("Home Assistant sent an unreadable answer");
    }
    std::vector<Entity> entities;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (entities.size() >= kMaxEntities) {
            break;
        }
        const cJSON* entity_id = cJSON_GetObjectItem(item, "entity_id");
        if (!cJSON_IsString(entity_id) || !IsControllableDomain(entity_id->valuestring)) {
            continue;
        }
        const cJSON* state = cJSON_GetObjectItem(item, "state");
        const cJSON* attributes = cJSON_GetObjectItem(item, "attributes");
        const cJSON* friendly_name =
            attributes ? cJSON_GetObjectItem(attributes, "friendly_name") : nullptr;
        Entity entity;
        entity.entity_id = entity_id->valuestring;
        entity.friendly_name = cJSON_IsString(friendly_name) ? friendly_name->valuestring
                                                             : entity.entity_id;
        entity.state = cJSON_IsString(state) ? state->valuestring : "unknown";
        entities.push_back(std::move(entity));
    }
    cJSON_Delete(root);
    return entities;
}

std::expected<std::string, std::string> WalleHomeAssistant::CallService(
    const std::string& service, const std::string& entity_id) {
    if (!IsControllableDomain(entity_id)) {
        return std::unexpected(entity_id + " is not a Jarvis-controllable entity; use "
                               "self.home_assistant.list_devices to see what's available.");
    }
    const std::string json_body = "{\"entity_id\":\"" + entity_id + "\"}";
    auto body = Request("POST", "/api/services/homeassistant/" + service, json_body);
    if (!body) {
        return std::unexpected(body.error());
    }
    return "Done.";
}

std::expected<std::string, std::string> WalleHomeAssistant::TurnOn(const std::string& entity_id) {
    return CallService("turn_on", entity_id);
}

std::expected<std::string, std::string> WalleHomeAssistant::TurnOff(const std::string& entity_id) {
    return CallService("turn_off", entity_id);
}

std::expected<std::string, std::string> WalleHomeAssistant::Toggle(const std::string& entity_id) {
    return CallService("toggle", entity_id);
}

std::expected<std::string, std::string> WalleHomeAssistant::GetState(
    const std::string& entity_id) {
    auto body = Request("GET", "/api/states/" + entity_id);
    if (!body) {
        return std::unexpected(body.error());
    }
    cJSON* root = cJSON_Parse(body->c_str());
    const cJSON* state = root ? cJSON_GetObjectItem(root, "state") : nullptr;
    if (!cJSON_IsString(state)) {
        if (root) cJSON_Delete(root);
        return std::unexpected("Home Assistant does not know an entity called " + entity_id);
    }
    const cJSON* attributes = cJSON_GetObjectItem(root, "attributes");
    const cJSON* friendly_name =
        attributes ? cJSON_GetObjectItem(attributes, "friendly_name") : nullptr;
    const std::string name = cJSON_IsString(friendly_name) ? friendly_name->valuestring : entity_id;
    std::string result = name + " is " + state->valuestring + ".";
    cJSON_Delete(root);
    return result;
}
