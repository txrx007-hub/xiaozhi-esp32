#pragma once

#include <expected>
#include <string>

// Which voice server the robot talks to. The robot asks the "OTA URL" for the websocket address of
// its voice server, so changing that one saved URL switches the whole voice back end: the xiaozhi.me
// cloud (the built-in default) or an own server on the LAN (xiaozhi-esp32-server). The change takes
// effect after the next reboot. Shared by the USB console (!server) and the settings page.
namespace walle_server {

// The OTA URL in effect: the saved one, or the built-in default (cloud).
std::string OtaUrl();

// True when a URL is saved (own server); false = built-in default (cloud).
bool IsCustom();

// Address of the backend control panel (http://<server host>:8090/), taken from the saved server's host.
// Empty when the robot uses the cloud: then there is no panel of ours to point to.
std::string PanelUrl();

// Checks `value` the way Set() does but saves nothing. Returns the OTA URL that would be saved, or
// an empty string for "default" (cloud), or the reason it is refused. Used by `!server check`.
std::expected<std::string, std::string> Resolve(const std::string& value, bool from_web);

// Sets the server and returns a message for the user. `value` is "default" (back to the cloud), an
// IPv4 address or host name with optional :port (default port 8003), or - console only - a full
// http(s):// URL. With from_web = true only private LAN addresses are accepted: the page has no
// login and the robot streams its microphone to the server, so anyone on the Wi-Fi must not be
// able to point it at a host on the internet. The USB console (physical access) has no such limit.
std::expected<std::string, std::string> Set(const std::string& value, bool from_web);

}  // namespace walle_server
