/*
 * Tiny DNS server for the setup hotspot. Based on the ESP-IDF captive_portal
 * example, but it closes its socket when stopped (the example leaks it, so a
 * second start can't bind port 53) and answers non-A queries with no records.
 */
#include <errno.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "captive_dns.h"

#define DNS_PORT 53
#define TYPE_A 1
#define CLASS_IN 1
#define ANSWER_LEN 16

static const char *TAG = "dns";

static volatile bool s_running;
static TaskHandle_t s_task;

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

// Turns the query in buf into a reply in place. Returns the reply length, or 0 to ignore it.
static int build_reply(uint8_t *buf, int len, uint32_t ip)
{
    dns_header_t *h = (dns_header_t *)buf;
    uint16_t flags = ntohs(h->flags);
    if ((flags & 0x8000) || (flags & 0x7800) || ntohs(h->qdcount) != 1) {
        return 0;   // a response, not a standard query, or several questions
    }

    int pos = sizeof(dns_header_t);
    while (pos < len && buf[pos] != 0) {
        if (buf[pos] & 0xC0) {
            return 0;   // compressed names don't appear in questions
        }
        pos += buf[pos] + 1;
    }
    pos += 1 + 4;   // the name's terminating zero, then type and class
    if (pos > len) {
        return 0;
    }
    uint16_t qtype = (buf[pos - 4] << 8) | buf[pos - 3];
    uint16_t qclass = (buf[pos - 2] << 8) | buf[pos - 1];

    h->flags = htons(0x8080 | (flags & 0x0100));   // response + recursion available, copy RD
    h->nscount = 0;
    h->arcount = 0;   // the reply ends after the question, dropping any EDNS record
    if (qtype != TYPE_A || qclass != CLASS_IN) {
        h->ancount = 0;   // e.g. AAAA: no records, so the phone falls back to IPv4
        return pos;
    }

    static const uint8_t answer[] = {
        0xC0, 0x0C,               // name: pointer to the question
        0x00, TYPE_A, 0x00, CLASS_IN,
        0x00, 0x00, 0x00, 0x3C,   // TTL 60 s
        0x00, 0x04,               // address length
    };
    h->ancount = htons(1);
    memcpy(buf + pos, answer, sizeof(answer));
    memcpy(buf + pos + sizeof(answer), &ip, 4);
    return pos + ANSWER_LEN;
}

static void dns_task(void *arg)
{
    uint32_t ip = (uint32_t)(uintptr_t)arg;   // network byte order
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "can't open port 53 (errno %d)", errno);
        s_running = false;
    } else {
        // Wake up every second to notice captive_dns_stop().
        struct timeval timeout = { .tv_sec = 1 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ESP_LOGI(TAG, "answering all queries with " IPSTR, IP2STR((esp_ip4_addr_t *)&ip));
    }

    uint8_t buf[512];
    while (s_running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf) - ANSWER_LEN, 0, (struct sockaddr *)&from, &from_len);
        if (len < (int)sizeof(dns_header_t)) {
            continue;   // timeout or junk
        }
        int reply_len = build_reply(buf, len, ip);
        if (reply_len > 0) {
            sendto(sock, buf, reply_len, 0, (struct sockaddr *)&from, from_len);
        }
    }

    if (sock >= 0) {
        close(sock);
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

void captive_dns_start(esp_netif_t *netif)
{
    esp_netif_ip_info_t info;
    esp_netif_get_ip_info(netif, &info);
    s_running = true;
    if (!s_task) {
        xTaskCreate(dns_task, "captive_dns", 3072, (void *)(uintptr_t)info.ip.addr, 4, &s_task);
    }
}

void captive_dns_stop(void)
{
    s_running = false;   // the task notices within a second and closes its socket
}
