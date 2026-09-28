#pragma once

#include "application.h"
#include "led/led.h"

// Upstream calls Led::OnStateChanged() on every device state change (and on VAD changes
// while listening). Wall-E has no status LED, so this "LED" forwards state transitions to
// the board: listening face, level bars, wake threshold while speaking, delayed nap/sleep.
template <typename BoardT>
class WalleStateHook : public Led {
public:
    explicit WalleStateHook(BoardT* board) : board_(board) {}

    void OnStateChanged() override {
        const DeviceState now = Application::GetInstance().GetDeviceState();
        if (now == last_) {
            return;  // VAD change only
        }
        const DeviceState previous = last_;
        last_ = now;
        board_->OnDeviceStateChanged(previous, now);
    }

private:
    BoardT* board_;
    DeviceState last_ = kDeviceStateUnknown;
};
