#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

typedef struct {
    bool setup_active;     // the setup hotspot is open
    bool has_credentials;  // a home network is saved
    bool connected;        // joined the home network and got an address
    char ssid[33];         // home network name
    char ip[16];
    int last_error;        // last disconnect reason (wifi_err_reason_t), 0 if none
} net_status_t;

// Starts Wi-Fi: joins the saved network, or opens the setup hotspot if there is none.
// Also starts mDNS (<hostname>.local) and, once connected, NTP.
void net_start(void);

// Opens the setup hotspot (captive portal) for CONFIG_DUMBLEDOOR_SETUP_TIMEOUT_MIN.
void net_start_setup(void);

bool net_setup_active(void);
bool net_connected(void);
void net_get_status(net_status_t *out);

// Signal strength of the home network in dBm, 0 when not connected.
int net_rssi(void);

// Whether `ip` (network byte order) is the setup hotspot's own address.
bool net_is_setup_ip(uint32_t ip);

// Saves a home network and connects to it.
esp_err_t net_join(const char *ssid, const char *password);

// Blocking scan. Returns the number of records written.
int net_scan(wifi_ap_record_t *records, int max);
