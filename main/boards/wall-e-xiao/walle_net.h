#pragma once

#include <cstdint>

// Counts what the robot sends and receives over Wi-Fi: packets and bytes, measured where lwIP hands frames to the Wi-Fi driver and takes them from it
// (every IP packet of this robot: the voice link, the settings page, DNS, ACKs ...). Starts at boot, so the numbers reset with every restart.
// The counters are 32 bit and wrap: a reader works with the difference between two readings.
namespace walle_net {

struct Counters {
    uint32_t tx_pkts;
    uint32_t rx_pkts;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t t_ms;  // milliseconds since boot when the counters were read
};

void Start();  // idempotent; waits for the Wi-Fi station interface to exist and then hooks into it
Counters Read();

}  // namespace walle_net
