#pragma once

/**
 * @file config.h
 * @brief Constants for whichever modem module is selected in menuconfig.
 *
 * Two kinds of values live here:
 *
 * 1. COMMON block -- identical across all three original source projects
 *    (USB Modem / Quectel_EC200U / Quectel_EC200U_USB), so it's not
 *    module-specific at all: PPP connect timeout/retry backoff, and the
 *    plain TCP reachability self-test target. These apply no matter which
 *    MODEM_MODULE is selected.
 *
 * 2. Per-module #if block -- GPIOs and power-sequencing timings that
 *    genuinely differ per module (confirmed by diffing the three originals'
 *    own config.h files). Only the block matching the active
 *    CONFIG_MODEM_MODULE_* is compiled; the others don't exist in the
 *    resulting binary at all.
 *
 * SimCom A7672S/USB (Step 4a) and Quectel EC200U-CN/UART (Step 9) blocks
 * exist so far. Selecting Quectel EC200U-CN/USB before its own step lands
 * is a deliberate, clear build-time error below rather than silently
 * compiling something wrong.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- */
/*  Common to every module                                               */
/* -------------------------------------------------------------------- */

#define PPP_CONNECT_TIMEOUT_MS      60000
#define MAX_PPP_RETRIES             5
#define PPP_POST_RESET_DELAY_MS     10000
#define PPP_TEST_HOST               "example.com"
#define PPP_TEST_PORT               80

/* Applied to the PPP netif in ppp_manager.c's apply_got_ip() -- see the
 * comment there for why (HTTPS OTA connect failures over cellular PPP
 * that plain HTTP didn't hit, traced to MTU). Started at 1400 (worked for
 * the SimCom/USB backend); the Quectel/UART backend hit the identical
 * "delayed connect error: Software caused connection abort" failure
 * even with that value in place -- UART PPPoS framing overhead differs
 * from USB CDC's, so the same nominal MTU doesn't guarantee the same
 * effective path MTU. Lowered to 1300, shared across both backends
 * (a smaller MTU is always safe, just marginally less throughput-
 * efficient for the backend that didn't strictly need it). Lower further
 * (1250/1200) if HTTPS connects are still unreliable at this value. */
#define PPP_MTU                     1300

#define RETRY_BASE_DELAY_MS         3000
#define RETRY_MAX_DELAY_MS          60000

/* -------------------------------------------------------------------- */
/*  Per-module                                                           */
/* -------------------------------------------------------------------- */

#if defined(CONFIG_MODEM_MODULE_SIMCOM_A7672_USB)

/*
 * ESP32-S3 + SimCom A7672S via Espressif iot_usbh_modem (USB Host PPP).
 *
 * Wiring:
 *   A7672S USB D+/D- -> ESP32-S3 USB Host D+/D-
 *   GND shared, modem 5V from a dedicated >= 2A supply
 *   Optional PWRKEY on GPIO4
 */

#define MODEM_PWRKEY_GPIO           4
#define MODEM_USB_BOOT_WAIT_MS      3000

/* A7672S PWRKEY timing (SIMCom A7672S Hardware Design, Tables 12-14):
 *   Toff    min 2.5s active-low pulse to power OFF from a running state
 *   Toff-on min 2s   mandatory buffer between power-off and next power-on
 *   Ton     typ 30ms active-low pulse to power ON from OFF
 * Values below add margin over each datasheet minimum/typical. Only use
 * this for a genuinely wedged modem (USB re-enumerates but never gets an
 * IP) -- it's a full power-cycle, not a quick nudge. */
#define MODEM_PWRKEY_OFF_PULSE_MS   3000
#define MODEM_PWRKEY_SETTLE_MS      5000
#define MODEM_PWRKEY_ON_PULSE_MS    150

/* ESP32-S3-USB-OTG DevKit only: routes native USB (GPIO19/20) to the
 * USB_HOST connector instead of USB_DEV. High = USB_HOST active. Not
 * needed on a custom PCB where GPIO19/20 go straight to the modem. */
#define OTG_USB_SEL_GPIO            18

#elif defined(CONFIG_MODEM_MODULE_QUECTEL_EC200U_UART)

/*
 * ESP32-S3 + Quectel EC200U-CN (Vanix TracX-1b carrier) via Espressif
 * esp_modem (UART PPPoS).
 *
 * Wiring:
 *   TracX-1b Tx  -> ESP32-S3 UART Rx  (MODEM_UART_RX_GPIO)
 *   TracX-1b Rx  -> ESP32-S3 UART Tx  (MODEM_UART_TX_GPIO)
 *   TracX-1b GND -> ESP32-S3 GND
 *   No MAIN_RTS/MAIN_CTS wiring: the EC200U-CN chip supports HW flow
 *   control, but this carrier board's header doesn't expose those two
 *   pins -- flow control stays at None regardless of baud.
 *   Carrier board VBAT <- dedicated >= 2A, 3.7-4V supply, independent of
 *                          the ESP32-S3 entirely
 *   PWRKEY on GPIO4, RST on GPIO5
 */

/* UART pins/baud/flow-control come straight from Kconfig (Step 2's
 * "Quectel EC200U-CN UART pins/baud" sub-menu) -- these #defines just give
 * modem_quectel_ec200u_uart.cpp the same MODEM_UART_* names the other
 * modules' code style uses, rather than referencing CONFIG_MODEM_UART_*
 * directly. */
#define MODEM_UART_PORT_NUM         CONFIG_MODEM_UART_PORT_NUM
#define MODEM_UART_TX_GPIO          CONFIG_MODEM_UART_TX_PIN
#define MODEM_UART_RX_GPIO          CONFIG_MODEM_UART_RX_PIN
#define MODEM_UART_RTS_GPIO         CONFIG_MODEM_UART_RTS_PIN
#define MODEM_UART_CTS_GPIO         CONFIG_MODEM_UART_CTS_PIN
#define MODEM_UART_BAUD_RATE        CONFIG_MODEM_UART_BAUD_RATE

#if defined(CONFIG_MODEM_FLOW_CONTROL_NONE)
#define MODEM_FLOW_CONTROL          ESP_MODEM_FLOW_CONTROL_NONE
#elif defined(CONFIG_MODEM_FLOW_CONTROL_HW)
#define MODEM_FLOW_CONTROL          ESP_MODEM_FLOW_CONTROL_HW
#endif

#define MODEM_PWRKEY_GPIO           4
#define MODEM_RST_GPIO              5
#define MODEM_BOOT_WAIT_MS          5000

/* EC200U-CN control-signal timing (Vanix TracX-1b / Quectel EC200U-CN
 * User Manual, section 4.3.1):
 *   - The module auto-powers-on as soon as VBAT/USB is applied -- no
 *     PWRKEY pulse is required on a cold power-up.
 *   - Power ON from power-down: PWRKEY low >= 2s.
 *   - Power OFF: PWRKEY low >= 3s (module then runs its own graceful
 *     power-down sequence after release). AT+QPOWD does the same thing
 *     in software and is preferred when the AT channel is reachable.
 *   - RST: low pulse >= 100ms hard-resets the baseband. NOT a graceful
 *     shutdown (no detach/dereg) -- use PWRKEY/AT+QPOWD first and reserve
 *     RST for a genuinely wedged modem.
 * SETTLE_MS: the datasheet gives no bound on how long the module's own
 * power-down procedure takes after PWRKEY release. If the ON pulse lands
 * before shutdown has actually finished, the module can be left stuck
 * powered-off -- observed in practice on the original hardware, not
 * hypothetical. Generous on purpose since there's no spec value to size
 * this against. */
#define MODEM_PWRKEY_ON_PULSE_MS    2500
#define MODEM_PWRKEY_OFF_PULSE_MS   3200
#define MODEM_PWRKEY_SETTLE_MS      8000
#define MODEM_RST_PULSE_MS          150

/* modem_install() polls AT+CGATT (network attachment) for up to this long
 * before returning, so the first modem_ppp_start() dial lands after the
 * modem has actually attached instead of racing it. LTE registration after
 * a cold boot/reset commonly takes 10-30+ s -- dialing before that just
 * burns a PPP retry cycle on a guaranteed failure. */
#define MODEM_NET_ATTACH_TIMEOUT_MS 30000
#define MODEM_NET_ATTACH_POLL_MS    1000

#elif defined(CONFIG_MODEM_MODULE_QUECTEL_EC200U_USB)

#error "config.h: Quectel EC200U-CN USB pin/timing block not implemented " \
       "yet -- coming in a later step. Select \"SimCom A7672S -- USB host\" " \
       "in `idf.py menuconfig` for now."

#else

#error "config.h: No MODEM_MODULE_* selected -- run `idf.py menuconfig` " \
       "-> IoT Hub Multi-Modem Configuration -> Modem module"

#endif

#ifdef __cplusplus
}
#endif
