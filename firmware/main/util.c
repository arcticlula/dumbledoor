#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "util.h"

#define CLOCK_SET_AFTER 1700000000   // Nov 2023: anything earlier means NTP hasn't answered yet

static char s_id[10];

const char *util_device_id(void)
{
    if (!s_id[0]) {
        uint8_t mac[6] = { 0 };
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_id, sizeof(s_id), "dd-%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
    return s_id;
}

uint32_t util_uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

int64_t util_wall_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < CLOCK_SET_AFTER) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void restart_cb(void *arg)
{
    esp_restart();
}

void util_restart_soon(uint32_t ms)
{
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = { .callback = restart_cb, .name = "restart" };
    if (timer || esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_stop(timer);
        esp_timer_start_once(timer, (uint64_t)ms * 1000);
    }
}

void util_hex(const uint8_t *in, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0F];
    }
    out[2 * len] = '\0';
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool util_unhex(const char *in, uint8_t *out, size_t len)
{
    if (strnlen(in, 2 * len + 1) != 2 * len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        int hi = nibble(in[2 * i]);
        int lo = nibble(in[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}
