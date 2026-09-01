#pragma once

/**
 * @file web_server.h
 * @brief Wi-Fi softAP + HTTP server: the UI and REST/SSE surface that
 *        finally gives ota_manager_start() a caller.
 *
 * Generic like ppp_manager/ota_manager -- talks to modem_hal.h only for
 * the one cosmetic thing that's genuinely worth surfacing to the UI
 * (modem_module_name(), for the status display), never to a backend
 * directly.
 */

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the Wi-Fi softAP and HTTP server.
 *
 * Opens AP with SSID "IOT-HUB-AP" on 192.168.4.1.
 * Serves the web UI and REST endpoints (/status, /events, /ota/start, /modem/retry).
 */
esp_err_t web_server_start(void);

/**
 * @brief Push a JSON event string to all connected SSE clients.
 *
 * Safe to call from any task. Matches ota_manager.h's ota_event_cb_t
 * signature exactly, so it can be passed straight to
 * ota_manager_set_event_cb() with no adapter.
 * @param json_event  Null-terminated JSON string, e.g. {"type":"log","msg":"hello"}
 */
void web_server_push_event(const char *json_event);

/**
 * @brief Returns true (once) if the web UI requested a modem retry.
 */
bool web_server_retry_requested(void);

/**
 * @brief Update the status fields shown on the web UI.
 */
void web_server_set_status(const char *fw_version,
                           const char *ssid,
                           int  modem_state,   /* 0=offline,1=connecting,2=online */
                           const char *ip4,
                           const char *operator_name,
                           int  rssi);

#ifdef __cplusplus
}
#endif
