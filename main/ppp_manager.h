#pragma once

/**
 * @file ppp_manager.h
 * @brief Waits for the PPP link to come up, tracks its state, and offers a
 *        plain TCP reachability self-test.
 *
 * Generic and transport-agnostic on purpose -- like modem_hal.h, this file
 * only talks to esp_netif/lwIP PPP events and modem_hal.h's
 * modem_ppp_stop()/modem_get_netif(), never to any backend-specific API.
 * It does not change per selected MODEM_MODULE, and is compiled
 * unconditionally (see main/CMakeLists.txt) regardless of which backend is
 * active.
 */

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the IP_EVENT_PPP_* and NETIF_PPP_STATUS handlers this
 *        module needs. Call once, after esp_event_loop_create_default(),
 *        before the first ppp_manager_connect().
 */
esp_err_t ppp_manager_init_events(void);

/**
 * @brief Wait for the PPP link to come up.
 *
 * Does NOT dial -- call modem_ppp_start() (modem_hal.h) first. This only
 * waits for the IP_EVENT_PPP_GOT_IP that dial should eventually produce,
 * up to PPP_CONNECT_TIMEOUT_MS (config.h). Returns immediately with ESP_OK
 * if the link is already up (e.g. a backend whose modem_install() already
 * triggered a connect before this is even called).
 * @return ESP_OK if connected, ESP_ERR_TIMEOUT if the wait expired,
 *         ESP_FAIL if an explicit LOST_IP arrived instead.
 */
esp_err_t ppp_manager_connect(void);

/** Hang up (via modem_hal.h's modem_ppp_stop()) and mark disconnected. */
esp_err_t ppp_manager_disconnect(void);

/** True from the last GOT_IP until the next LOST_IP/disconnect. */
bool ppp_manager_is_connected(void);

/**
 * @brief Plain TCP connect to host:port over the current default route, as
 *        a real internet reachability check (DNS + TCP), not just "PPP
 *        says it has an IP". Used as a pre-OTA sanity check.
 * @return ESP_OK if the TCP handshake completed, ESP_FAIL otherwise.
 *         ESP_ERR_INVALID_STATE if not currently connected.
 */
esp_err_t ppp_manager_verify_connectivity(const char *host, int port);

#ifdef __cplusplus
}
#endif
