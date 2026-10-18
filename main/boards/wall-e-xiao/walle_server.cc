#include "walle_server.h"

#include <cctype>
#include <cstdlib>
#include <utility>

#include <sdkconfig.h>

#include "settings.h"

namespace walle_server {

namespace {

const char* const kKey = "ota_url";
constexpr int kDefaultPort = 8003;
constexpr int kPanelPort = 8090;  // the backend control panel (panel.py) of the own server
constexpr size_t kMaxUrlLength = 200;

std::string Trim(const std::string& s) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool EndsWith(const std::string& s, const char* suffix) {
    const std::string suf(suffix);
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool AllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

// Splits "host" or "host:port"; checks the characters and the port range.
std::expected<std::pair<std::string, int>, std::string> ParseHostPort(const std::string& text) {
    std::string host = text;
    int port = kDefaultPort;
    const size_t colon = text.rfind(':');
    if (colon != std::string::npos) {
        const std::string port_text = text.substr(colon + 1);
        if (!AllDigits(port_text) || port_text.size() > 5) {
            return std::unexpected("the port after ':' must be a number");
        }
        port = atoi(port_text.c_str());
        if (port < 1 || port > 65535) {
            return std::unexpected("the port must be 1 to 65535");
        }
        host = text.substr(0, colon);
    }
    if (host.empty() || host.size() > 100 || host.front() == '.' || host.back() == '.' ||
        host.find("..") != std::string::npos) {
        return std::unexpected("not a valid IP address or host name");
    }
    for (char c : host) {
        if (!isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') {
            return std::unexpected("only letters, digits, '.', '-' and '_' are allowed in the host");
        }
    }
    return std::make_pair(host, port);
}

// Dotted-quad check; on success fills a and b (the first two octets). Leading zeros ("010") are
// refused because some network stacks read them as octal.
bool ParseIpv4(const std::string& host, int& a, int& b) {
    int octets[4] = {};
    int count = 0;
    size_t start = 0;
    while (true) {
        const size_t dot = host.find('.', start);
        const std::string part = host.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (count == 4 || !AllDigits(part) || part.size() > 3 || (part.size() > 1 && part[0] == '0') ||
            atoi(part.c_str()) > 255) {
            return false;
        }
        octets[count++] = atoi(part.c_str());
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    if (count != 4) return false;
    a = octets[0];
    b = octets[1];
    return true;
}

bool LooksLikeIp(const std::string& host) {
    for (char c : host) {
        if (!isdigit(static_cast<unsigned char>(c)) && c != '.') return false;
    }
    return true;
}

// 10/8, 172.16/12, 192.168/16, or a LAN name (no dots, .local, .lan).
bool IsPrivateHost(const std::string& host) {
    if (LooksLikeIp(host)) {
        int a = 0, b = 0;
        if (!ParseIpv4(host, a, b)) return false;
        return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
    }
    return host.find('.') == std::string::npos || EndsWith(host, ".local") || EndsWith(host, ".lan");
}

}  // namespace

std::string OtaUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString(kKey);
    return url.empty() ? std::string(CONFIG_OTA_URL) : url;
}

bool IsCustom() {
    Settings settings("wifi", false);
    return !settings.GetString(kKey).empty();
}

std::string PanelUrl() {
    if (!IsCustom()) {
        return "";
    }
    // http://HOST[:PORT]/xiaozhi/ota/  ->  HOST
    const std::string url = OtaUrl();
    const size_t scheme = url.find("://");
    const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    const size_t slash = url.find('/', start);
    std::string host = url.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos) {
        host.resize(colon);
    }
    if (host.empty()) {
        return "";
    }
    return "http://" + host + ":" + std::to_string(kPanelPort) + "/";
}

std::expected<std::string, std::string> Resolve(const std::string& value, bool from_web) {
    const std::string v = Trim(value);
    if (v.empty() || v == "default") {
        return std::string();
    }
    if (v.size() > kMaxUrlLength) {
        return std::unexpected("too long");
    }

    if (v.find("://") != std::string::npos) {
        if (from_web) {
            return std::unexpected(
                "Enter an IP address or LAN name, not a full URL. A full URL can only be set from the USB console.");
        }
        if (v.rfind("http://", 0) != 0 && v.rfind("https://", 0) != 0) {
            return std::unexpected("the URL must start with http:// or https://");
        }
        for (char c : v) {
            if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f || c == '"' || c == '\\') {
                return std::unexpected("the URL contains a character that is not allowed");
            }
        }
        return v;
    }

    auto parsed = ParseHostPort(v);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const auto& [host, port] = *parsed;
    if (LooksLikeIp(host)) {
        int a = 0, b = 0;
        if (!ParseIpv4(host, a, b)) {
            return std::unexpected("not a valid IPv4 address");
        }
    }
    if (from_web && !IsPrivateHost(host)) {
        return std::unexpected(
            "From the web page only a LAN address is accepted (192.168.x.x, 10.x.x.x, 172.16-31.x.x or a LAN name). "
            "Use the USB console for anything else.");
    }
    return "http://" + host + ":" + std::to_string(port) + "/xiaozhi/ota/";
}

std::expected<std::string, std::string> Set(const std::string& value, bool from_web) {
    auto url = Resolve(value, from_web);
    if (!url) {
        return std::unexpected(url.error());
    }
    Settings settings("wifi", true);
    if (url->empty()) {
        settings.EraseKey(kKey);
        return "Server set to the built-in default (xiaozhi.me cloud).";
    }
    settings.SetString(kKey, *url);
    return "Server set to " + *url;
}

}  // namespace walle_server
