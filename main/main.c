/*
 * main.c -- IoT Hub Multi-Modem, Step 8: web_server wired in.
 *
 * This is the point where the whole stack becomes end-to-end usable from a
 * browser: web_server_start() opens the "IOT-HUB-AP" Wi-Fi AP + UI, OTA
 * finally has a caller (POST /ota/start -> ota_manager_start()), and every
 * status/log message that used to only go to ESP_LOGI is now also pushed
 * to the web UI (push_log() + web_server_set_status()), matching the shape
 * of all three original projects' main().
 *
 * Still only includes modem_hal.h/ppp_manager.h/ota_manager.h/
 * web_server.h -- no backend-specific header -- so this file is unaffected
 * by which MODEM_MODULE is selected.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"

#include "config.h"
#include "modem_hal.h"
#include "ppp_manager.h"
#include "ota_manager.h"
#include "web_server.h"

static const char *TAG = "APP";

#define AP_NAME "IOT-HUB-AP"

static char s_fw[32] = "?";
static char s_ip[20] = "-";
static char s_op[32] = "-";
static int  s_rssi = 0;

/* Mirrors every log line to both the serial console (ESP_LOGI) and the web
 * UI's SSE log panel (web_server_push_event) -- same helper shape as all
 * three original projects' main.c. */
static void push_log(const char *fmt, ...)
{
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    ESP_LOGI(TAG, "%s", msg);

    char ev[256];
    snprintf(ev, sizeof(ev), "{\"type\":\"log\",\"msg\":\"%s\"}", msg);
    web_server_push_event(ev);
}

/* Registered with ota_manager as its pre-reboot hook -- called right
 * before esp_restart() on a successful OTA. A modem's own power supply is
 * usually independent of the ESP32 and doesn't reset alongside it, so
 * without this a modem mid-PPP-session (DATA mode) can be left in that
 * state across the reboot, confusing the fresh bring-up that starts
 * afterward. Goes through modem_hal.h only -- works unmodified regardless
 * of which backend is active. */
static void modem_hangup_before_reboot(void)
{
    if (modem_is_installed()) {
        push_log("OTA: returning modem to command mode (hang up PPP) before reboot");
        modem_ppp_stop();
    }
}

static bool ppp_bring_up(void)
{
    for (int attempt = 1; attempt <= MAX_PPP_RETRIES; attempt++) {
        push_log("PPP: attempt %d/%d (APN=%s)", attempt, MAX_PPP_RETRIES, CONFIG_MODEM_APN);

        if (!modem_is_installed()) {
            if (modem_install(CONFIG_MODEM_APN) != ESP_OK) {
                push_log("PPP: modem_install failed");
                goto backoff;
            }
        }

        if (modem_ppp_start() != ESP_OK) {
            push_log("PPP: modem_ppp_start failed");
            goto backoff;
        }

        web_server_set_status(s_fw, AP_NAME, 1, "-", s_op, s_rssi);
        push_log("PPP: dialing -- waiting for GOT_IP");
        if (ppp_manager_connect() == ESP_OK) {
            return true;
        }

        push_log("PPP: GOT_IP timeout");

backoff:
        if (attempt < MAX_PPP_RETRIES) {
            int delay_ms = RETRY_BASE_DELAY_MS << (attempt - 1);
            if (delay_ms > RETRY_MAX_DELAY_MS) {
                delay_ms = RETRY_MAX_DELAY_MS;
            }
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
    return false;
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    strncpy(s_fw, app->version, sizeof(s_fw) - 1);
    s_fw[sizeof(s_fw) - 1] = '\0';

    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  IoT Hub Multi-Modem fw=%s", s_fw);
    ESP_LOGI(TAG, "  Active module: %s", modem_module_name());
    ESP_LOGI(TAG, "=================================================");

    /* NVS must be initialised before anything that touches flash-backed
     * key/value storage (Wi-Fi config, and the modem/OTA code). Doing it
     * here, once, at the very start of app_main is the standard ESP-IDF
     * pattern -- carried over unchanged from all three source projects.
     * The erase+retry handles the case where NVS was formatted by an
     * older IDF version with an incompatible layout. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(web_server_start());
    ota_manager_init();
    ota_manager_set_event_cb(web_server_push_event);
    ota_manager_set_pre_reboot_hook(modem_hangup_before_reboot);
    ESP_ERROR_CHECK(ppp_manager_init_events());

    web_server_set_status(s_fw, AP_NAME, 1, "-", "-", 0);

    /* Unconditional PWRKEY power-cycle before the very first modem_install()
     * attempt -- not just on failure. Root cause this works around: the
     * ESP32's own USB host peripheral resets whenever the ESP32 itself
     * resets (OTA reboot, brownout, manual reset button -- any of them),
     * which looks like an abrupt USB disconnect to the modem. Observed in
     * practice: the A7672S doesn't always recover cleanly from that on its
     * own and gets stuck needing a physical PWRKEY cycle before it will
     * sync again -- previously that meant a manual power-cycle after every
     * ESP reset. A software PWRKEY cycle (modem_hal.h's
     * modem_hard_power_cycle(), same GPIO the manual recovery used) run
     * once here, before install, guarantees every boot starts from a known
     * clean modem power state regardless of what happened before the
     * reset -- including a true cold boot, where the OFF-pulse is simply a
     * no-op on an already-off module and the module still ends up powered
     * on afterward. The cost is a fixed ~8s added to every boot; the
     * previous failure-triggered recovery path (still below, for a modem
     * that gets wedged again mid-run) took several minutes to kick in by
     * comparison.
     *
     * MODEM_BOOT_POWER_CYCLE (config.h, per-module) gates this: confirmed
     * genuinely needed for SimCom/USB, but confirmed on 2026-09-03 (direct
     * A/B against the pre-unification standalone Quectel project, same
     * hardware/wiring/session) to actively break an already-fine Quectel
     * module instead of helping it -- see the comment on
     * MODEM_BOOT_POWER_CYCLE in the Quectel block of config.h for the
     * full story. Not every backend needs this, and for at least one it's
     * actively harmful, so it's opt-in per module rather than universal. */
#if MODEM_BOOT_POWER_CYCLE
    push_log("SYS: power-cycling modem before first install (clean state after any ESP reset)");
    modem_hard_power_cycle();

    /* Extra settle time beyond modem_install()'s own boot-wait -- see
     * MODEM_BOOT_POWER_CYCLE_SETTLE_MS in config.h for the full story.
     * In short: this power-cycle (unlike the mid-session recovery one
     * below, which always has a 60s buffer before the next attempt) was
     * observed going straight into a failing first install because the
     * modem's baseband hadn't finished a cold boot yet. */
    push_log("SYS: waiting %d ms for modem to finish booting after power-cycle...",
             MODEM_BOOT_POWER_CYCLE_SETTLE_MS);
    vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_POWER_CYCLE_SETTLE_MS));
#endif

    push_log("SYS: booted -- starting modem (%s)", modem_module_name());

    bool online = false;

    while (1) {
        if (!online) {
            online = ppp_bring_up();
            if (!online) {
                web_server_set_status(s_fw, AP_NAME, 0, "-", s_op, s_rssi);
                push_log("PPP: all attempts failed -- resetting modem (AT+CFUN=1,1)");
                modem_force_reset();
                modem_uninstall();
                push_log("PPP: hard power-cycling modem via PWRKEY");
                modem_hard_power_cycle();
                web_server_set_status(s_fw, AP_NAME, 1, "-", s_op, s_rssi);
                push_log("PPP: all attempts failed -- retry in 60 s");
                for (int w = 0; w < 60; w++) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    if (web_server_retry_requested()) {
                        push_log("PPP: manual retry");
                        break;
                    }
                }
                continue;
            }

            esp_netif_t *nif = modem_get_netif();
            esp_netif_ip_info_t ip_info = {0};
            if (nif && esp_netif_get_ip_info(nif, &ip_info) == ESP_OK) {
                esp_ip4addr_ntoa(&ip_info.ip, s_ip, sizeof(s_ip));
                esp_netif_set_default_netif(nif);
            }

            modem_read_status(s_op, sizeof(s_op), &s_rssi);
            web_server_set_status(s_fw, AP_NAME, 2, s_ip, s_op, s_rssi);
            push_log("PPP: ONLINE IP=%s op=%s rssi=%d", s_ip, s_op, s_rssi);

            if (ppp_manager_verify_connectivity(PPP_TEST_HOST, PPP_TEST_PORT) == ESP_OK) {
                push_log("PPP: internet OK -- OTA ready");
            } else {
                push_log("PPP: internet self-test FAILED");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5000));

        if (web_server_retry_requested()) {
            push_log("PPP: reconnect from Web UI");
            modem_uninstall();
            online = false;
            web_server_set_status(s_fw, AP_NAME, 1, "-", s_op, s_rssi);
            continue;
        }

        if (ota_manager_get_state() == OTA_STATE_RUNNING) {
            /* Don't fight an in-progress OTA -- a transient link hiccup
             * mid-download shouldn't trigger modem_uninstall() out from
             * under it. */
            continue;
        }

        if (!ppp_manager_is_connected()) {
            push_log("PPP: link lost -- recovering");
            modem_uninstall();
            online = false;
            web_server_set_status(s_fw, AP_NAME, 1, "-", s_op, s_rssi);
        }
    }
}
