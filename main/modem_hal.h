#pragma once

/**
 * @file modem_hal.h
 * @brief The one interface every modem backend implements.
 *
 * This project supports three physical modem/transport combinations
 * (see Kconfig.projbuild -> MODEM_MODULE): SimCom A7672S over USB,
 * Quectel EC200U-CN over UART, and Quectel EC200U-CN over USB
 * (experimental). Exactly ONE backend source file is compiled into any
 * given build (main/CMakeLists.txt picks it based on the Kconfig choice),
 * and that file must define every function declared here:
 *
 *   modem_simcom_a7672_usb.c        (Step 4a)
 *   modem_quectel_ec200u_uart.cpp   (Step 4b)
 *   modem_quectel_ec200u_usb.c      (Step 4c)
 *
 * Nothing outside the modem layer -- main.c, ppp_manager, ota_manager,
 * web_server -- includes anything other than THIS header. They never see
 * usbh_modem_*, esp_modem::*, or any other transport-specific API
 * directly. That's what makes the module selection a menuconfig choice
 * instead of a fork of the whole app, the way the three original
 * standalone projects were.
 *
 * Two functions below are intentionally "wider" than any single backend's
 * native API, because the three original projects' real behavior genuinely
 * differed and had to be reconciled into one contract:
 *
 * - modem_ppp_start(): iot_usbh_modem (both USB backends) auto-dials once
 *   the USB device enumerates -- calling its own ppp_start while
 *   auto-connect is on is an error. esp_modem (the UART backend) never
 *   dials on its own -- an explicit set_mode(DATA_MODE) call is required.
 *   Rather than leak that difference to main.c (which would then need an
 *   #ifdef per module), every backend implements modem_ppp_start(): the
 *   UART backend actually dials, the USB backends make it a documented
 *   no-op (auto-connect already did the work back in modem_install()).
 *   Callers always call it; what it does depends on the backend.
 *
 * - modem_hard_reset(): only the two Quectel backends have an RST line
 *   wired. The SimCom backend implements this too, but returns
 *   ESP_ERR_NOT_SUPPORTED with a log line explaining why, rather than not
 *   existing at all -- so main.c's recovery ladder can always call it
 *   without caring which module is active.
 *
 * Deliberately NOT in this interface (kept out to keep the contract
 * minimal, matching what main.c/ppp_manager.c actually used across all
 * three original projects):
 *   - modem_get_atparser() / modem_ppp_auto_connect() -- iot_usbh_modem
 *     internals, only ever used *inside* a USB backend's own modem_install(),
 *     never called from outside the modem layer in any original project.
 *   - modem_set_uart_baud_persist() -- a one-off UART migration utility,
 *     not part of the init/OTA/recovery flow this HAL exists to unify.
 *     If/when the web UI's "migrate baud" feature is ported over, it can
 *     be added as a UART-backend-specific extra call, guarded by
 *     `#ifdef CONFIG_MODEM_MODULE_QUECTEL_EC200U_UART`, without touching
 *     this shared interface.
 */

#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the modem: power-on sequencing, driver/DTE install, DCE
 *        creation, and (backend-dependent) an initial AT sanity check.
 *        Does NOT dial PPP -- call modem_ppp_start() afterwards for that.
 * @param apn Carrier APN (e.g. "airtelgprs.com"). Applied via PDP config.
 * @return ESP_OK on success. ESP_ERR_INVALID_STATE if already installed.
 */
esp_err_t modem_install(const char *apn);

/** Tear down whatever modem_install() set up (DCE/DTE, driver, netif). */
esp_err_t modem_uninstall(void);

/** True after a successful modem_install(), until modem_uninstall(). */
bool modem_is_installed(void);

/** The PPP esp_netif created by modem_install(), or NULL if not installed. */
esp_netif_t *modem_get_netif(void);

/**
 * @brief Switch into data mode / trigger a PPP dial, if this backend
 *        requires an explicit step -- a no-op if it doesn't (see the
 *        file-header note above). Callers wait for IP_EVENT_PPP_GOT_IP
 *        afterwards regardless (ppp_manager, in a later step) -- this call
 *        only starts the attempt, it does not block until connected.
 */
esp_err_t modem_ppp_start(void);

/** Return to command mode / hang up. Safe to call even if never dialed. */
esp_err_t modem_ppp_stop(void);

/**
 * @brief Best-effort operator name + RSSI, for status display only.
 *        May legitimately fail to populate either value (e.g. mid-PPP-sync
 *        on a backend with no secondary AT channel) -- callers should treat
 *        the outputs as "best effort", not guaranteed fresh.
 * @param op_buf Buffer for the operator name; written "-" if unavailable.
 * @param op_len Size of op_buf.
 * @param rssi_out Written 99 (3GPP "unknown") if unavailable.
 */
void modem_read_status(char *op_buf, size_t op_len, int *rssi_out);

/**
 * @brief Soft-reset the modem's baseband over AT (e.g. AT+CFUN=1,1).
 *        Requires the AT channel to be reachable -- will fail on a modem
 *        that's already wedged badly enough to need modem_hard_power_cycle()
 *        instead. Call only while installed and not mid-dial.
 */
esp_err_t modem_force_reset(void);

/**
 * @brief Hardware power-cycle via the PWRKEY line. Works even when the
 *        modem's AT/USB/UART stack is unresponsive, since PWRKEY is
 *        serviced by the module's power-management IC, not its baseband
 *        firmware. Blocks for several seconds (datasheet timing per
 *        module -- see each backend's own comments).
 */
esp_err_t modem_hard_power_cycle(void);

/**
 * @brief Hardware reset via the RST line, where wired. NOT a graceful
 *        shutdown (no network deregistration) -- reserve for a modem that
 *        doesn't recover via modem_hard_power_cycle() either.
 * @return ESP_ERR_NOT_SUPPORTED on a backend with no RST line wired
 *         (documented per-backend, e.g. the SimCom A7672S backend).
 */
esp_err_t modem_hard_reset(void);

/**
 * @brief Human-readable name of the active module, for log banners and the
 *        web UI status display. Pure Kconfig-derived compile-time constant
 *        -- no backend implementation needed, hence defined inline here
 *        rather than requiring every backend file to duplicate it.
 */
static inline const char *modem_module_name(void)
{
#if defined(CONFIG_MODEM_MODULE_SIMCOM_A7672_USB)
    return "SimCom A7672S (USB host)";
#elif defined(CONFIG_MODEM_MODULE_QUECTEL_EC200U_UART)
    return "Quectel EC200U-CN (UART PPPoS)";
#elif defined(CONFIG_MODEM_MODULE_QUECTEL_EC200U_USB)
    return "Quectel EC200U-CN (USB host, EXPERIMENTAL)";
#else
#error "No MODEM_MODULE_* selected -- run `idf.py menuconfig` " \
       "-> IoT Hub Multi-Modem Configuration -> Modem module"
#endif
}

#ifdef __cplusplus
}
#endif
