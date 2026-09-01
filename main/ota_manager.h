#pragma once

/**
 * @file ota_manager.h
 * @brief Full .bin OTA (esp_https_ota) and delta .patch OTA (esp_delta_ota),
 *        picked by URL extension. Generic like ppp_manager.h -- only talks
 *        to esp_http_client/esp_https_ota over whatever the current default
 *        route is, never to the modem or a specific backend.
 *
 * One deliberate change from the three original projects: they called
 * web_server_push_event() directly from ota_manager.c, a hard compile-time
 * dependency on the web server existing. Since web_server hasn't been
 * built yet at this point in the project (a later step), and to keep
 * ota_manager genuinely standalone/reusable, events are instead delivered
 * through an optional callback set via ota_manager_set_event_cb(). When
 * web_server.c lands, main.c wires it up with one line
 * (ota_manager_set_event_cb(web_server_push_event)) instead of ota_manager
 * needing to know web_server exists at all. Until then, ESP_LOGI alone
 * carries every event.
 */

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_RUNNING,
    OTA_STATE_DONE,
    OTA_STATE_FAILED,
} ota_state_t;

/** Same wire format as the original projects' SSE events, e.g.
 *  {"type":"log","msg":"..."} / {"type":"progress","pct":N,"label":"..."} /
 *  {"type":"reboot","secs":N} / {"type":"ota_failed"} -- so an eventual
 *  web_server can pass this straight through unmodified. */
typedef void (*ota_event_cb_t)(const char *json_event);

/** Called right before esp_restart() on a successful OTA, before the final
 *  reboot delay. Used by a modem backend that needs to leave the link in a
 *  clean state first (e.g. hang up PPP) -- see modem_hal.h's
 *  modem_ppp_stop(). Optional; NULL (the default) means no-op. */
typedef void (*ota_pre_reboot_hook_t)(void);

/** Initialise the OTA manager (call once after NVS init). */
void ota_manager_init(void);

/** Optional: receive the same JSON events ota_manager already logs via
 *  ESP_LOGI, for forwarding to a UI (web_server's SSE, in a later step). */
void ota_manager_set_event_cb(ota_event_cb_t cb);

/** Optional: run this right before the post-OTA reboot. See
 *  ota_pre_reboot_hook_t above. */
void ota_manager_set_pre_reboot_hook(ota_pre_reboot_hook_t hook);

/**
 * @brief Start an OTA download+flash from @p url.
 *
 * If url ends with ".patch" -> delta OTA via esp_delta_ota.
 * Otherwise              -> full binary OTA via esp_https_ota.
 *
 * Runs in a background FreeRTOS task; progress/log events go through
 * ota_manager_set_event_cb()'s callback (if set) and ESP_LOGI regardless.
 * @return ESP_OK if the task was started, ESP_ERR_INVALID_STATE if an OTA
 *         is already running.
 */
esp_err_t ota_manager_start(const char *url);

/** Current OTA state. */
ota_state_t ota_manager_get_state(void);

#ifdef __cplusplus
}
#endif
