// videodev2.h must come before esp_video.h (via walle_camera.h): see walle_camera.cc for why.
#include "linux/videodev2.h"

#include "walle_web.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "system_info.h"
#include "wall_e_board.h"
#include "walle_camera.h"
#include "walle_home_assistant.h"
#include "walle_settings.h"
#include "wifi_manager.h"

#define TAG "WalleWeb"

// Embedded by CMakeLists.txt (boards/<dir>/web/*.html), same EMBED_FILES mechanism as sounds.
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

namespace walle_web {

namespace {

httpd_handle_t s_server = nullptr;
std::string s_url;

const char* KindName(WalleSettings::Kind kind) {
    switch (kind) {
        case WalleSettings::Kind::kInt: return "int";
        case WalleSettings::Kind::kBool: return "bool";
        case WalleSettings::Kind::kHundredths: return "hundredths";
        case WalleSettings::Kind::kTimeOfDay: return "time";
        case WalleSettings::Kind::kLogLevel: return "enum";
        case WalleSettings::Kind::kVisualizerMode: return "enum";
        case WalleSettings::Kind::kText: return "text";
    }
    return "int";
}

// A UI step hint per key: small enough to reach every value, big enough to be usable on a
// touch slider. Purely cosmetic; the server still validates whatever value arrives.
int StepFor(const std::string& key, WalleSettings::Kind kind) {
    if (kind == WalleSettings::Kind::kHundredths) return 1;  // page sends fractions of 0.01
    if (key == "turn_ms_per_90" || key == "soft_start_ms") return 10;
    return 1;
}

cJSON* SettingToJson(const WalleSettings::Def& def) {
    auto& settings = WalleSettings::GetInstance();
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "key", def.key);
    cJSON_AddStringToObject(item, "kind", KindName(def.kind));
    cJSON_AddStringToObject(item, "help", def.help);

    switch (def.kind) {
        case WalleSettings::Kind::kInt:
            cJSON_AddNumberToObject(item, "min", def.min);
            cJSON_AddNumberToObject(item, "max", def.max);
            cJSON_AddNumberToObject(item, "step", StepFor(def.key, def.kind));
            cJSON_AddNumberToObject(item, "value", settings.GetInt(def.key));
            break;
        case WalleSettings::Kind::kHundredths:
            cJSON_AddNumberToObject(item, "min", def.min / 100.0);
            cJSON_AddNumberToObject(item, "max", def.max / 100.0);
            cJSON_AddNumberToObject(item, "step", 0.01);
            cJSON_AddNumberToObject(item, "value", settings.GetHundredths(def.key));
            break;
        case WalleSettings::Kind::kBool:
            cJSON_AddBoolToObject(item, "value", settings.GetBool(def.key));
            break;
        case WalleSettings::Kind::kTimeOfDay: {
            const int minutes = settings.GetInt(def.key);
            if (minutes < 0) {
                cJSON_AddNullToObject(item, "value");
            } else {
                char buf[16];  // minutes is stored as an int; oversize so -Wformat-truncation is happy
                snprintf(buf, sizeof(buf), "%02d:%02d", minutes / 60, minutes % 60);
                cJSON_AddStringToObject(item, "value", buf);
            }
            break;
        }
        case WalleSettings::Kind::kLogLevel: {
            cJSON_AddStringToObject(item, "value", settings.FormatCurrent(def).c_str());
            cJSON* options = cJSON_AddArrayToObject(item, "options");
            for (const char* level : {"error", "warn", "info", "debug"}) {
                cJSON_AddItemToArray(options, cJSON_CreateString(level));
            }
            break;
        }
        case WalleSettings::Kind::kVisualizerMode: {
            cJSON_AddStringToObject(item, "value", settings.FormatCurrent(def).c_str());
            cJSON* options = cJSON_AddArrayToObject(item, "options");
            for (const char* mode : {"off", "winamp"}) {
                cJSON_AddItemToArray(options, cJSON_CreateString(mode));
            }
            break;
        }
        case WalleSettings::Kind::kText:
            cJSON_AddNumberToObject(item, "max_length", def.max);
            // ha_token is a credential: report whether one is set, never the value itself, so it
            // never sits in plaintext in a browser response, devtools network tab or fetch cache.
            if (strcmp(def.key, "ha_token") == 0) {
                cJSON_AddBoolToObject(item, "is_set", !settings.GetText(def.key).empty());
                cJSON_AddStringToObject(item, "value", "");
            } else {
                cJSON_AddStringToObject(item, "value", settings.GetText(def.key).c_str());
            }
            break;
    }
    return item;
}

esp_err_t SendJson(httpd_req_t* req, cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    cJSON_free(text);
    cJSON_Delete(root);
    return err;
}

esp_err_t IndexHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html_start,
                           static_cast<ssize_t>(index_html_end - index_html_start));
}

esp_err_t GetSettingsHandler(httpd_req_t* req) {
    cJSON* array = cJSON_CreateArray();
    for (const auto& def : WalleSettings::GetInstance().Defs()) {
        cJSON_AddItemToArray(array, SettingToJson(def));
    }
    return SendJson(req, array);
}

esp_err_t GetStatusHandler(httpd_req_t* req) {
    auto& board = WallEBoard::Get();
    auto& wifi = WifiManager::GetInstance();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "board", SystemInfo::GetUserAgent().c_str());
    cJSON_AddStringToObject(root, "ssid", wifi.GetSsid().c_str());
    cJSON_AddStringToObject(root, "ip", wifi.GetIpAddress().c_str());
    cJSON_AddNumberToObject(root, "rssi", wifi.GetRssi());
    cJSON_AddNumberToObject(root, "state",
                            static_cast<int>(Application::GetInstance().GetDeviceState()));
    cJSON_AddBoolToObject(root, "napping", board.IsNapping());
    cJSON_AddBoolToObject(root, "ha_reachable", WalleHomeAssistant::GetInstance().IsReachable());
    return SendJson(req, root);
}

// WALL-E: temporary diagnostic for the purple-hue report. GET /debug/photo.jpg?format=yuyv|uyvy
// captures one frame and encodes THAT SAME frame under the requested pixel-format tag (default
// uyvy), so the two color-order hypotheses can be compared directly. Not a normal user feature;
// remove once the camera color pipeline is confirmed correct.
esp_err_t GetDebugPhotoHandler(httpd_req_t* req) {
    char query[32] = {};
    char format_param[8] = "uyvy";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "format", format_param, sizeof(format_param));
    }
    const bool want_yuyv = strcmp(format_param, "yuyv") == 0;
    const v4l2_pix_fmt_t as_format = want_yuyv ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_UYVY;

    auto* camera = WallEBoard::Get().walle_camera();
    if (camera == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no camera");
        return ESP_FAIL;
    }
    std::vector<uint8_t> jpeg = camera->CaptureJpegAs(as_format);
    if (jpeg.empty()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture or encode failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, reinterpret_cast<const char*>(jpeg.data()),
                           static_cast<ssize_t>(jpeg.size()));
}

// Reads the whole request body (settings payloads are a few dozen bytes; refuse anything odd).
bool ReadBody(httpd_req_t* req, std::string& out) {
    const size_t len = req->content_len;
    if (len == 0 || len > 512) {
        return false;
    }
    out.resize(len);
    size_t received = 0;
    while (received < len) {
        const int n = httpd_req_recv(req, out.data() + received, len - received);
        if (n <= 0) {
            return false;
        }
        received += n;
    }
    return true;
}

esp_err_t PostSettingsHandler(httpd_req_t* req) {
    std::string body;
    if (!ReadBody(req, body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request body");
        return ESP_FAIL;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    const cJSON* key = root ? cJSON_GetObjectItem(root, "key") : nullptr;
    const cJSON* value = root ? cJSON_GetObjectItem(root, "value") : nullptr;
    if (!cJSON_IsString(key) || !cJSON_IsString(value)) {
        if (root) cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected {\"key\":.., \"value\":..}");
        return ESP_FAIL;
    }

    // Apply on the main task, like every other settings write (voice, console).
    struct Pending {
        std::string key, value;
        std::expected<std::string, std::string> result{std::string()};
        bool done = false;
    };
    auto pending = std::make_shared<Pending>();
    pending->key = key->valuestring;
    pending->value = value->valuestring;
    cJSON_Delete(root);

    Application::GetInstance().Schedule([pending]() {
        pending->result = WalleSettings::GetInstance().Set(pending->key, pending->value);
        pending->done = true;
    });
    for (int waited = 0; !pending->done && waited < 2000; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    cJSON* out = cJSON_CreateObject();
    if (!pending->done) {
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddStringToObject(out, "message", "timed out applying the setting");
    } else if (pending->result) {
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "message", pending->result->c_str());
        cJSON_AddItemToArray(cJSON_AddArrayToObject(out, "settings"),
                             SettingToJson(*WalleSettings::GetInstance().Find(pending->key)));
    } else {
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddStringToObject(out, "message", pending->result.error().c_str());
    }
    return SendJson(req, out);
}

esp_err_t PostResetHandler(httpd_req_t* req) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    Application::GetInstance().Schedule([done]() {
        WalleSettings::GetInstance().ResetAll();
        *done = true;
    });
    for (int waited = 0; !done->load() && waited < 2000; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", done->load());
    cJSON_AddStringToObject(out, "message", done->load() ? "All settings reset to defaults."
                                                         : "Timed out resetting settings.");
    return SendJson(req, out);
}

}  // namespace

void Start() {
    if (s_server != nullptr) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 6;
    config.lru_purge_enable = true;
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start the LAN settings page");
        s_server = nullptr;
        return;
    }
    // Only the first 4 fields are set; later ones (e.g. is_websocket) exist only when
    // CONFIG_HTTPD_WS_SUPPORT is on and default to zero either way.
    static const httpd_uri_t kRoutes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = IndexHandler, .user_ctx = nullptr},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = GetSettingsHandler, .user_ctx = nullptr},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = PostSettingsHandler, .user_ctx = nullptr},
        {.uri = "/api/reset", .method = HTTP_POST, .handler = PostResetHandler, .user_ctx = nullptr},
        {.uri = "/api/status", .method = HTTP_GET, .handler = GetStatusHandler, .user_ctx = nullptr},
        {.uri = "/debug/photo.jpg", .method = HTTP_GET, .handler = GetDebugPhotoHandler, .user_ctx = nullptr},
    };
    for (const auto& route : kRoutes) {
        httpd_register_uri_handler(s_server, &route);
    }
    s_url = "http://" + WifiManager::GetInstance().GetIpAddress() + "/";
    ESP_LOGI(TAG, "LAN settings page: %s", s_url.c_str());
}

void Stop() {
    if (s_server != nullptr) {
        httpd_stop(s_server);
        s_server = nullptr;
    }
    s_url.clear();
}

const char* Url() { return s_url.c_str(); }

}  // namespace walle_web
