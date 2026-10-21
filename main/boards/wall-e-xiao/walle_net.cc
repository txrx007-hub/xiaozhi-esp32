#include "walle_net.h"

#include <atomic>

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_timer.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/tcpip.h>

#define TAG "WalleNet"

namespace walle_net {
namespace {

std::atomic<uint32_t> g_tx_pkts{0};
std::atomic<uint32_t> g_rx_pkts{0};
std::atomic<uint32_t> g_tx_bytes{0};
std::atomic<uint32_t> g_rx_bytes{0};

netif_linkoutput_fn g_orig_out = nullptr;
netif_input_fn g_orig_in = nullptr;
esp_timer_handle_t g_timer = nullptr;

err_t CountedOut(struct netif* n, struct pbuf* p) {
    g_tx_pkts.fetch_add(1, std::memory_order_relaxed);
    g_tx_bytes.fetch_add(p->tot_len, std::memory_order_relaxed);
    return g_orig_out(n, p);
}

err_t CountedIn(struct pbuf* p, struct netif* n) {
    g_rx_pkts.fetch_add(1, std::memory_order_relaxed);
    g_rx_bytes.fetch_add(p->tot_len, std::memory_order_relaxed);  // read before the stack takes the buffer over
    return g_orig_in(p, n);
}

// Hooks the station interface once it exists (it is created when Wi-Fi starts); tried again every 2 s in case Wi-Fi is torn down and set up again.
void TryHook(void*) {
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == nullptr) {
        return;
    }
    auto* n = static_cast<struct netif*>(esp_netif_get_netif_impl(sta));
    if (n == nullptr || n->linkoutput == nullptr || n->input == nullptr || n->linkoutput == CountedOut) {
        return;
    }
    LOCK_TCPIP_CORE();
    if (g_orig_out == nullptr) {  // the first interface: remember the real functions
        g_orig_out = n->linkoutput;
        g_orig_in = n->input;
    }
    n->linkoutput = CountedOut;
    n->input = CountedIn;
    UNLOCK_TCPIP_CORE();
    ESP_LOGI(TAG, "counting Wi-Fi packets");
}

}  // namespace

void Start() {
    if (g_timer != nullptr) {
        return;
    }
    esp_timer_create_args_t args = {};
    args.callback = TryHook;
    args.name = "walle_net";
    if (esp_timer_create(&args, &g_timer) == ESP_OK) {
        esp_timer_start_periodic(g_timer, 2 * 1000 * 1000);
        TryHook(nullptr);
    }
}

Counters Read() {
    Counters c;
    c.tx_pkts = g_tx_pkts.load(std::memory_order_relaxed);
    c.rx_pkts = g_rx_pkts.load(std::memory_order_relaxed);
    c.tx_bytes = g_tx_bytes.load(std::memory_order_relaxed);
    c.rx_bytes = g_rx_bytes.load(std::memory_order_relaxed);
    c.t_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    return c;
}

}  // namespace walle_net
