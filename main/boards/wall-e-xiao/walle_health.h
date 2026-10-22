#pragma once

// WALL-E: a few health numbers for /api/status (the backend's robot-health card and the landing page's overview):
// the chip's own temperature sensor and how often the Wi-Fi connection dropped since boot.
namespace walle_health {

// Starts the temperature sensor and listens for Wi-Fi disconnects. Safe to call once at boot; a part that cannot start is
// simply reported as missing (NaN temperature) instead of failing the boot.
void Init();

// The chip's internal temperature in degrees Celsius (the die, not the room: it runs warmer than the air), or NaN when the sensor is not available.
float ChipTempC();

// Wi-Fi disconnects since boot, and the reason code of the last one (0 = none yet; see wifi_err_reason_t, e.g. 200 beacon timeout, 201 no AP found).
int WifiDisconnects();
int LastDisconnectReason();

}  // namespace walle_health
