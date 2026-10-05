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
enum class Screen : uint8_t {
    BOOT_MENU = 0,   // pick WARNET / CAFE / debug
    CONNECTING,      // associating + DHCP + starting web server
    CONFIRM,         // show SSID / IP / web-server status + Finish + Back
    HOME,            // standby homescreen (static "ready" text)
    QRIS,            // QR + cart summary + total
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
