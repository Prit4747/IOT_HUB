/*
 * ppp_manager.c -- wait for IP_EVENT_PPP_GOT_IP, track link state, and a
 * plain TCP reachability self-test.
 *
 * Merged from the two originals (USB Modem's and Quectel_EC200U's
 * ppp_manager.c, which had only small differences) into one generic
 * version, with one deliberate simplification:
 *
 * DROPPED: the USB project's IOT_ETH_EVENT_* handler (IOT_ETH_EVENT_START/
 * STOP/CONNECTED/DISCONNECTED). Espressif's iot_eth is a USB-backend-only
 * concept (iot_usbh_modem's glue layer) -- including iot_eth.h here would
 * mean this "generic" file is no longer actually transport-agnostic,
 * breaking the same abstraction boundary modem_hal.h exists to enforce.
 * Those events were informational logging only, never load-bearing: the
 * real GOT_IP detection below is unaffected by their absence.
 *
 * KEPT (from the Quectel_EC200U version): apply_got_ip() unconditionally
 * overrides DNS to a known-good public resolver (8.8.8.8 + 8.8.4.4 backup)
 * rather than only falling back to it when the carrier gave none. Some
 * carrier-provided DNS resolves common sites fine but is flaky for others
 * (e.g. a release CDN an OTA URL points at) -- and this decision is
 * transport-agnostic, so it now applies uniformly to every backend, not
 * just the one project that originally had it.
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_netif_net_stack.h"
#include "lwip/netif.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/ip4_addr.h"
#include "lwip/dns.h"

#include "ppp_manager.h"
#include "modem_hal.h"
#include "config.h"

static const char *TAG = "PPP";

#define PPP_GOT_IP_BIT   BIT0
#define PPP_LOST_IP_BIT  BIT1

static EventGroupHandle_t s_ppp_events = NULL;
static volatile bool s_connected = false;

static bool netif_has_v4(esp_netif_t *nif)
{
    esp_netif_ip_info_t info;
    if (nif == NULL || esp_netif_get_ip_info(nif, &info) != ESP_OK) {
        return false;
    }
    return info.ip.addr != 0;
}

static void apply_got_ip(esp_netif_t *nif, const esp_netif_ip_info_t *ip_info)
{
    ESP_LOGI(TAG, "PPP GOT_IP  " IPSTR, IP2STR(&ip_info->ip));

    /* Lower the netif MTU below lwIP's 1500-byte default. Observed in
     * practice (2026-09-01, HTTPS OTA against GitHub over this exact PPP
     * link): plain HTTP TCP connects succeeded fine, but every HTTPS
     * connect attempt failed at the TLS handshake stage with
     * "delayed connect error: Software caused connection abort" --
     * before any application data was exchanged. TLS handshakes push
     * noticeably larger packets than a small HTTP GET (ClientHello with
     * SNI/cert-bundle extensions, a multi-KB ServerHello+certificate
     * chain), and cellular PPP paths often silently drop/abort packets
     * above some carrier-specific size well under the standard 1500 --
     * this is a well-known PPPoS-over-cellular gotcha, not specific to
     * one modem or backend, hence fixed here rather than per-backend.
     * 1400 is a conservative, commonly-used value that leaves headroom
     * under most carriers' actual path MTU. If OTA connects are still
     * flaky after this, try lowering further (e.g. 1350/1300) before
     * suspecting anything else.
     *
     * No public esp_netif_set_mtu() exists in this IDF version (checked
     * esp_netif.h directly -- not there), so this goes through
     * esp_netif_get_netif_impl(), which for the lwIP port returns the
     * underlying lwIP `struct netif *` -- its `mtu` field is directly
     * settable and takes effect immediately for future sends. This works
     * identically regardless of which backend created the netif (esp_modem
     * for UART, iot_usbh_modem internally for USB), since it operates on
     * the netif handle itself, not on how it was constructed. */
    struct netif *lwip_nif = (struct netif *)esp_netif_get_netif_impl(nif);
    if (lwip_nif) {
        lwip_nif->mtu = PPP_MTU;
    } else {
        ESP_LOGW(TAG, "esp_netif_get_netif_impl returned NULL -- MTU not changed");
    }

    /* Always force a known-good public resolver -- see file-header note.
     * Set both esp_netif DNS *and* lwIP dns_setserver(): with SoftAP + PPP
     * up together, getaddrinfo can miss netif-only DNS and fail even though
     * PPP has a valid CGNAT address (seen as "internet self-test FAILED"
     * immediately after GOT_IP). */
    esp_netif_dns_info_t dns_main = {0};
    dns_main.ip.type = ESP_IPADDR_TYPE_V4;
    IP4_ADDR(&dns_main.ip.u_addr.ip4, 8, 8, 8, 8);
    esp_netif_set_dns_info(nif, ESP_NETIF_DNS_MAIN, &dns_main);

    esp_netif_dns_info_t dns_backup = {0};
    dns_backup.ip.type = ESP_IPADDR_TYPE_V4;
    IP4_ADDR(&dns_backup.ip.u_addr.ip4, 8, 8, 4, 4);
    esp_netif_set_dns_info(nif, ESP_NETIF_DNS_BACKUP, &dns_backup);

    ip_addr_t dns0, dns1;
    IP_ADDR4(&dns0, 8, 8, 8, 8);
    IP_ADDR4(&dns1, 8, 8, 4, 4);
    dns_setserver(0, &dns0);
    dns_setserver(1, &dns1);

    esp_netif_set_default_netif(nif);
    s_connected = true;
    if (s_ppp_events) {
        xEventGroupClearBits(s_ppp_events, PPP_LOST_IP_BIT);
        xEventGroupSetBits(s_ppp_events, PPP_GOT_IP_BIT);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        apply_got_ip(ev->esp_netif, &ev->ip_info);
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP LOST_IP");
        esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap) {
            esp_netif_set_default_netif(ap);
        }
        s_connected = false;
        if (s_ppp_events) {
            xEventGroupClearBits(s_ppp_events, PPP_GOT_IP_BIT);
            xEventGroupSetBits(s_ppp_events, PPP_LOST_IP_BIT);
        }
    }
}

static void on_ppp_changed(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    ESP_LOGI(TAG, "PPP status event id=%" PRId32, id);
    if (id == NETIF_PPP_ERRORUSER) {
        ESP_LOGW(TAG, "PPP user interrupt (netif torn down)");
    } else if (id == NETIF_PPP_ERRORPEERDEAD) {
        /* LCP echo keepalives (CONFIG_LWIP_ENABLE_LCP_ECHO) decided the
         * peer stopped answering -- usually carrier/modem dropped PPP. */
        ESP_LOGW(TAG, "PPP peer dead (LCP echo timeout) -- link will be recovered");
    }
}

esp_err_t ppp_manager_init_events(void)
{
    if (s_ppp_events) {
        return ESP_OK;
    }
    s_ppp_events = xEventGroupCreate();
    if (!s_ppp_events) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                                               on_ip_event, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP,
                                     on_ip_event, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                     on_ppp_changed, NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "PPP IP + NETIF_PPP_STATUS handlers registered");
    return ESP_OK;
}

esp_err_t ppp_manager_connect(void)
{
    if (s_ppp_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if ((xEventGroupGetBits(s_ppp_events) & PPP_GOT_IP_BIT) || netif_has_v4(modem_get_netif())) {
        s_connected = true;
        return ESP_OK;
    }

    s_connected = false;
    xEventGroupClearBits(s_ppp_events, PPP_GOT_IP_BIT | PPP_LOST_IP_BIT);

    ESP_LOGI(TAG, "Waiting up to %d ms for IP_EVENT_PPP_GOT_IP", PPP_CONNECT_TIMEOUT_MS);

    EventBits_t bits = xEventGroupWaitBits(
        s_ppp_events,
        PPP_GOT_IP_BIT | PPP_LOST_IP_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(PPP_CONNECT_TIMEOUT_MS));

    if (bits & PPP_GOT_IP_BIT) {
        return ESP_OK;
    }
    if (netif_has_v4(modem_get_netif())) {
        s_connected = true;
        xEventGroupSetBits(s_ppp_events, PPP_GOT_IP_BIT);
        ESP_LOGW(TAG, "GOT_IP event missed -- netif already has an address");
        return ESP_OK;
    }
    if (bits & PPP_LOST_IP_BIT) {
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t ppp_manager_disconnect(void)
{
    s_connected = false;
    return modem_ppp_stop();
}

bool ppp_manager_is_connected(void)
{
    return s_connected;
}

static esp_err_t tcp_connect_addr(const struct sockaddr *addr, socklen_t addrlen,
                                  const char *label)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return ESP_FAIL;
    }

    struct timeval tv = {0};
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    esp_err_t ret = ESP_OK;
    if (connect(sock, addr, addrlen) != 0) {
        ESP_LOGE(TAG, "TCP connect %s failed errno=%d", label, errno);
        ret = ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "TCP connect %s ok", label);
    }
    close(sock);
    return ret;
}

esp_err_t ppp_manager_verify_connectivity(const char *host, int port)
{
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    /* SoftAP is also up -- re-assert PPP as default + lwIP DNS every time
     * before testing, in case anything flipped the default route back. */
    esp_netif_t *nif = modem_get_netif();
    if (nif) {
        esp_netif_set_default_netif(nif);
    }
    ip_addr_t dns0, dns1;
    IP_ADDR4(&dns0, 8, 8, 8, 8);
    IP_ADDR4(&dns1, 8, 8, 4, 4);
    dns_setserver(0, &dns0);
    dns_setserver(1, &dns1);

    /* Brief settle: IPCP just finished; immediate DNS sometimes fails. */
    vTaskDelay(pdMS_TO_TICKS(500));

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    for (int attempt = 1; attempt <= 3; attempt++) {
        struct addrinfo hints = {0};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = NULL;

        ESP_LOGI(TAG, "DNS lookup: %s (try %d/3)", host, attempt);
        int gai = getaddrinfo(host, port_str, &hints, &res);
        if (gai == 0 && res != NULL) {
            char label[64];
            snprintf(label, sizeof(label), "%s:%d", host, port);
            esp_err_t ret = tcp_connect_addr(res->ai_addr, res->ai_addrlen, label);
            freeaddrinfo(res);
            if (ret == ESP_OK) {
                return ESP_OK;
            }
        } else {
            ESP_LOGW(TAG, "DNS failed (try %d/3, gai=%d)", attempt, gai);
            if (res) {
                freeaddrinfo(res);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* DNS may be filtered by the carrier while raw IP still works -- prove
     * the data path with a numeric connect so OTA isn't blocked on DNS
     * alone (OTA URLs still need DNS; this separates the failures). */
    ESP_LOGW(TAG, "DNS path failed -- trying numeric fallback 1.1.1.1:80");
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    sa.sin_addr.s_addr = ipaddr_addr("1.1.1.1");
    if (tcp_connect_addr((struct sockaddr *)&sa, sizeof(sa), "1.1.1.1:80") == ESP_OK) {
        ESP_LOGW(TAG, "IP data path OK but DNS broken -- treat as FAIL for OTA");
    }
    return ESP_FAIL;
}
