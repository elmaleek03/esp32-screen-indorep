// app_state.h — shared runtime types for the display firmware.
#pragma once

#include <stdint.h>

// Which AP the operator picked on the boot menu.
enum class NetChoice : uint8_t {
    NONE = 0,
    WARNET,
    CAFE,
    DEBUG,
};

// Top-level UI state machine.
//
// The flow is one-way after SUMMARY: once the operator presses Continue, the
// device is committed to that network for the life of the boot and WIFI_SELECT
// becomes unreachable. See ui::show() for the latch that enforces it.
enum class Screen : uint8_t {
    BOOT_SCREEN = 0,  // cosmetic terminal log, auto-advances
    WIFI_SELECT,      // choose WARNET / CAFE (small Debug bottom-right)
    CONNECTING,       // associating + DHCP + starting web server
    SUMMARY,          // big green check + wifi info + Continue (commits the route)
    HOME,             // standby: analog clock + branch name
    QRIS,             // QR + cart summary + total
};

// Result of a web-server start attempt, shown on the CONFIRM screen.
enum class ServerState : uint8_t {
    STOPPED = 0,
    RUNNING,
    FAILED,
};

// Maximum cart lines the QRIS page will render. Sized to fit the 480x272 panel
// at 20 px line height with the QR occupying the right half.
#define MAX_DISPLAY_ITEMS 8

struct PaymentItem {
    char item[64];
    int  qty;
};

// Payload pushed by POST /update. Bounded so it can be held in a static buffer
// and copied under a short critical section (see web_server.cpp).
#define PAYLOAD_QR_MAX   1024
#define PAYLOAD_TOTAL_MAX 48

struct PaymentPayload {
    char        qr[PAYLOAD_QR_MAX];
    char        total[PAYLOAD_TOTAL_MAX];
    PaymentItem items[MAX_DISPLAY_ITEMS];
    int         item_count;
    bool        has_qr;
};
