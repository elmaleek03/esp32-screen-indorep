// wifi_profiles.h — AP credentials for the three boot-menu choices.
//
// WARNET and CAFE are open (no password). IDRP is the debug/provisioning AP.
#pragma once

#define WIFI_SSID_WARNET   "INDOREP WARNET"
#define WIFI_PASS_WARNET   ""                 // open network

// Confirmed on site: the cafe AP is spelled with a HYPHEN, not a space.
#define WIFI_SSID_CAFE     "INDOREP-CAFE"
#define WIFI_PASS_CAFE     ""                 // open network

#define WIFI_SSID_DEBUG    "IDRP"
#define WIFI_PASS_DEBUG    "topupgamemurah"   // WPA2

// How long to wait for association + IP before giving up (ms)
#define WIFI_CONNECT_TIMEOUT_MS   20000
