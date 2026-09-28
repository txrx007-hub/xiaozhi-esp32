#pragma once

// LAN settings page: a small HTTP server (no login, plain HTTP — LAN only) serving one page
// at http://<robot-ip>/ that reads and writes the same settings as voice and the USB console.
// Starts once Wi-Fi connects; stops on disconnect (restarted automatically on reconnect).
namespace walle_web {

void Start();
void Stop();
const char* Url();  // "" if the server is not running

}  // namespace walle_web
