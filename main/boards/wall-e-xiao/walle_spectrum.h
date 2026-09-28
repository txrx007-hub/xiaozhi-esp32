#pragma once

#include <cstdint>

// Shared between WalleAudioCodec (produces this on the audio task, from what's actually being
// sent to the speaker) and WalleDisplay (consumes it on the LVGL/main task, at its own cadence).
namespace walle_spectrum {

constexpr int kBands = 32;

// One analysis window's result: 0-255 per band, plus a slow-decaying peak-hold value per band.
// Deliberately tiny (64 bytes) so it can be copied under a lock at ~20-25 Hz for free, matching
// the struct Wall-E's build was sketched around.
struct Frame {
    uint8_t bands[kBands] = {};
    uint8_t peaks[kBands] = {};
};

// Selectable via self.display.set_visualizer / setting visualizer_mode. More modes (mirror_bars,
// circular, ...) can be added later without changing Frame or the FFT/analysis side at all.
enum class Mode { kOff, kWinamp };

}  // namespace walle_spectrum
