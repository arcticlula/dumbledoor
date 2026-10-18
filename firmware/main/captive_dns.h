#pragma once

#include "esp_netif.h"

// Answers every DNS A query with `netif`'s address (the setup hotspot), which
// makes phones open the setup page.
void captive_dns_start(esp_netif_t *netif);
void captive_dns_stop(void);
