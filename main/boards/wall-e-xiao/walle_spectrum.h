#pragma once

#include <cstdint>

// Shared between WalleAudioCodec (produces this on the audio task, from what's actually being
// sent to the speaker) and WalleDisplay (consumes it on the LVGL/main task, at its own cadence).
namespace walle_spectrum {

constexpr int kBands = 32;

// One analysis window's result: 0-255 per band, plus a slow-decaying peak-hold value per band.
// Deliberately tiny (64 bytes) so it can be copied under a lock at ~20-25 Hz for free, matching
// the struct Wall-E's build was sketched around.
constexpr int kWavePoints = 64;

struct Frame {
    uint8_t bands[kBands] = {};
    uint8_t peaks[kBands] = {};
    // Oscilloscope: the same window decimated to kWavePoints samples, auto-scaled to -127..127
    // (quiet stays flat: the scale never boosts below a fixed floor).
    int8_t wave[kWavePoints] = {};
};

// Selectable via self.display.set_visualizer / setting visualizer_mode; the index is the stored
// setting value, so only ever append. winamp/rainbow/scope/mouth draw in the strip under the eyes;
// radial/vu/orb replace the eyes with a square canvas while Jarvis speaks.
enum class Mode { kOff, kWinamp, kScope, kRadial, kVu, kMouth, kOrb, kRainbow };
constexpr const char* kModeNames[] = {"off", "winamp", "scope", "radial", "vu", "mouth", "orb", "rainbow"};
constexpr int kModeCount = sizeof(kModeNames) / sizeof(kModeNames[0]);

}  // namespace walle_spectrum
