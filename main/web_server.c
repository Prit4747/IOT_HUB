/*
 * web_server.c -- Wi-Fi softAP + HTTP server for IoT Hub OTA.
 *
 * Endpoints:
 *   GET  /           -> web_ui.html (embedded)
 *   GET  /status     -> JSON status snapshot
 *   GET  /events     -> SSE stream (log, progress, reboot events)
 *   POST /ota/start  -> {"url":"..."} -- triggers OTA
 *   POST /modem/retry-> requests modem reconnect
 *
 * SSE note: the /events handler returns immediately after recording the
 * client socket. Events are pushed later from other tasks with
 * httpd_socket_send(), which is the only send API safe to call outside a
 * request handler. Blocking inside the handler would stall the single
 * httpd worker and freeze every other endpoint.
 *
 * Carried over from the original projects essentially unchanged -- this
 * file never talks to the modem or a backend directly, only to
 * ota_manager/ppp_manager's public state and (for exactly one field,
 * "module") modem_hal.h's compile-time modem_module_name(). That one
 * addition is new versus the originals: since this project can target
 * three different modules from the same codebase, the status display
 * should say which one is actually running, rather than a hardcoded
 * per-project title.
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"

#include "web_server.h"
#include "ota_manager.h"
#include "modem_hal.h"

static const char *TAG = "WEB";

/* ------------------------------------------------------------------ */
/*  Embedded HTML                                                       */
/* ------------------------------------------------------------------ */

extern const uint8_t web_ui_html_start[] asm("_binary_web_ui_html_start");
extern const uint8_t web_ui_html_end[]   asm("_binary_web_ui_html_end");

/* ------------------------------------------------------------------ */
/*  Configuration                                                       */
/* ------------------------------------------------------------------ */

#define AP_SSID          "IOT-HUB-AP"
#define AP_PASS          ""
#define AP_CHAN          6
#define MAX_SSE_CLIENTS  2

/* ------------------------------------------------------------------ */
/*  State                                                               */
/* ------------------------------------------------------------------ */

static httpd_handle_t    s_server = NULL;

static SemaphoreHandle_t s_status_mux;
static char s_fw[32]   = "-";
static char s_ssid[32] = AP_SSID;
static char s_ip[20]   = "-";
static char s_op[32]   = "-";
static int  s_modem    = 0;
static int  s_rssi     = 0;

static SemaphoreHandle_t s_sse_mux;
static int  s_sse_fd[MAX_SSE_CLIENTS];   /* -1 = free slot */

static volatile bool s_retry_requested = false;

/* ------------------------------------------------------------------ */
/*  SSE: push one event to every connected client                       */
/* ------------------------------------------------------------------ */

void web_server_push_event(const char *json_event)
{
    if (!json_event || !s_server || !s_sse_mux) return;

    char body[512];
    int  blen = snprintf(body, sizeof(body), "data: %s\n\n", json_event);
    if (blen <= 0 || blen >= (int)sizeof(body)) return;

    /* The /events response uses chunked transfer encoding, so raw socket
     * writes must carry their own chunk header and trailer. */
    char frame[600];
    int  flen = snprintf(frame, sizeof(frame), "%x\r\n%s\r\n", blen, body);
    if (flen <= 0 || flen >= (int)sizeof(frame)) return;

    xSemaphoreTake(s_sse_mux, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        int fd = s_sse_fd[i];
        if (fd < 0) continue;
        if (httpd_socket_send(s_server, fd, frame, (size_t)flen, 0) < 0) {
            s_sse_fd[i] = -1;
            httpd_sess_trigger_close(s_server, fd);
        }
    }
    xSemaphoreGive(s_sse_mux);
}

/* ------------------------------------------------------------------ */
/*  Status snapshot                                                     */
/* ------------------------------------------------------------------ */

void web_server_set_status(const char *fw, const char *ssid, int modem_state,
                           const char *ip4, const char *op, int rssi)
{
    char ev[384];

    xSemaphoreTake(s_status_mux, portMAX_DELAY);
    if (fw)   { strncpy(s_fw,   fw,   sizeof(s_fw)   - 1); s_fw[sizeof(s_fw)     - 1] = '\0'; }
    if (ssid) { strncpy(s_ssid, ssid, sizeof(s_ssid) - 1); s_ssid[sizeof(s_ssid) - 1] = '\0'; }
    if (ip4)  { strncpy(s_ip,   ip4,  sizeof(s_ip)   - 1); s_ip[sizeof(s_ip)     - 1] = '\0'; }
    if (op)   { strncpy(s_op,   op,   sizeof(s_op)   - 1); s_op[sizeof(s_op)     - 1] = '\0'; }
    s_modem = modem_state;
    s_rssi  = rssi;

    snprintf(ev, sizeof(ev),
             "{\"type\":\"status\",\"fw\":\"%s\",\"ssid\":\"%s\",\"state\":%d,"
             "\"ip\":\"%s\",\"op\":\"%s\",\"rssi\":%d,\"module\":\"%s\"}",
             s_fw, s_ssid, s_modem, s_ip, s_op, s_rssi, modem_module_name());
    xSemaphoreGive(s_status_mux);

    web_server_push_event(ev);
}

bool web_server_retry_requested(void)
{
    bool v = s_retry_requested;
    s_retry_requested = false;
    return v;
}

/* ------------------------------------------------------------------ */
/*  HTTP handlers                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t h_root(httpd_req_t *req)
{
    size_t len = web_ui_html_end - web_ui_html_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)web_ui_html_start, (ssize_t)len);
}

static esp_err_t h_status(httpd_req_t *req)
{
    char buf[384];

    xSemaphoreTake(s_status_mux, portMAX_DELAY);
    snprintf(buf, sizeof(buf),
             "{\"fw\":\"%s\",\"ssid\":\"%s\",\"state\":%d,"
             "\"ip\":\"%s\",\"op\":\"%s\",\"rssi\":%d,\"module\":\"%s\"}",
             s_fw, s_ssid, s_modem, s_ip, s_op, s_rssi, modem_module_name());
    xSemaphoreGive(s_status_mux);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t h_events(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return ESP_FAIL;

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");

    /* Opens the chunked response; later events are framed by hand. */
    esp_err_t err = httpd_resp_send_chunk(req, ": ping\n\n", 8);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_sse_mux, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (s_sse_fd[i] == fd) { slot = i; break; }   /* reconnect on same fd */
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
            if (s_sse_fd[i] < 0) { slot = i; break; }
        }
    }
    if (slot < 0) {
        /* All slots busy -- evict the oldest so a fresh browser tab can attach. */
        slot = 0;
        httpd_sess_trigger_close(s_server, s_sse_fd[0]);
    }
    s_sse_fd[slot] = fd;
    xSemaphoreGive(s_sse_mux);

    ESP_LOGI(TAG, "SSE client attached on fd=%d (slot %d)", fd, slot);

    /* Return without the terminating chunk so the stream stays open. */
    return ESP_OK;
}

static esp_err_t h_ota_start(httpd_req_t *req)
{
    char body[600] = {0};
    int  cap = (int)sizeof(body) - 1;
    int  len = (int)req->content_len < cap ? (int)req->content_len : cap;

    if (len <= 0 || httpd_req_recv(req, body, len) <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    char url[512] = {0};
    const char *p = strstr(body, "\"url\"");
    if (p) {
        p = strchr(p + 5, '"');
        if (p) {
            p++;
            const char *e = strchr(p, '"');
            if (e) {
                size_t n = (size_t)(e - p);
                if (n >= sizeof(url)) n = sizeof(url) - 1;
                memcpy(url, p, n);
            }
        }
    }

    httpd_resp_set_type(req, "application/json");

    if (url[0] == '\0') {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"missing url\"}");
    }

    esp_err_t err = ota_manager_start(url);
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"OTA already running\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":false,\"err\":\"start failed\"}");
}

static esp_err_t h_modem_retry(httpd_req_t *req)
{
    s_retry_requested = true;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* Drop the SSE slot when the browser closes the tab. */
static void on_sess_close(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    if (!s_sse_mux) return;
    xSemaphoreTake(s_sse_mux, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (s_sse_fd[i] == sockfd) {
            s_sse_fd[i] = -1;
            ESP_LOGI(TAG, "SSE client detached on fd=%d", sockfd);
        }
    }
    xSemaphoreGive(s_sse_mux);
    close(sockfd);
}

/* ------------------------------------------------------------------ */
/*  Wi-Fi AP init                                                       */
/* ------------------------------------------------------------------ */

static void wifi_ap_init(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = AP_SSID,
            .ssid_len       = strlen(AP_SSID),
            .channel        = AP_CHAN,
            .password       = AP_PASS,
            .max_connection = 4,
            .authmode       = (strlen(AP_PASS) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* PPP must stay the default route once it is up; the AP is reachable
     * through its own directly-connected subnet either way. */
    ESP_LOGI(TAG, "Wi-Fi AP started: SSID=\"%s\" IP=192.168.4.1", AP_SSID);
}

/* ------------------------------------------------------------------ */
/*  Start                                                               */
/* ------------------------------------------------------------------ */

esp_err_t web_server_start(void)
{
    s_status_mux = xSemaphoreCreateMutex();
    s_sse_mux    = xSemaphoreCreateMutex();
    if (!s_status_mux || !s_sse_mux) return ESP_ERR_NO_MEM;

    for (int i = 0; i < MAX_SSE_CLIENTS; i++) s_sse_fd[i] = -1;

    const esp_app_desc_t *app = esp_app_get_description();
    strncpy(s_fw, app->version, sizeof(s_fw) - 1);
    s_fw[sizeof(s_fw) - 1] = '\0';

    wifi_ap_init();

    httpd_config_t srv_cfg   = HTTPD_DEFAULT_CONFIG();
    srv_cfg.lru_purge_enable = true;
    srv_cfg.max_open_sockets = 5;
    srv_cfg.stack_size       = 8192;
    srv_cfg.close_fn         = on_sess_close;

    esp_err_t err = httpd_start(&s_server, &srv_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = h_root        },
        { .uri = "/status",      .method = HTTP_GET,  .handler = h_status      },
        { .uri = "/events",      .method = HTTP_GET,  .handler = h_events      },
        { .uri = "/ota/start",   .method = HTTP_POST, .handler = h_ota_start   },
        { .uri = "/modem/retry", .method = HTTP_POST, .handler = h_modem_retry },
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}
