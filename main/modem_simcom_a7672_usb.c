/*
 * modem_simcom_a7672_usb.c -- modem_hal.h implementation for the SimCom
 * A7672S / A7670 / A7600 family, over USB host (Espressif iot_usbh_modem).
 *
 * Only compiled in when CONFIG_MODEM_MODULE_SIMCOM_A7672_USB is selected
 * (see main/CMakeLists.txt). Carried over from the original standalone
 * "USB Modem" project's modem.c, adapted to the modem_hal.h contract:
 *
 *   - modem_ppp_start() is a documented no-op here (see the "why" below) --
 *     it does NOT call usbh_modem_ppp_start(). Was previously a public
 *     wrapper around it, but nothing ever called that wrapper externally.
 *   - modem_hard_reset() returns ESP_ERR_NOT_SUPPORTED -- this module has
 *     no RST line wired (PWRKEY only), unlike the Quectel backends.
 *   - usbh_modem_get_atparser() / usbh_modem_ppp_auto_connect() stay
 *     internal to this file (used by modem_install()/modem_read_status()/
 *     modem_force_reset() below) -- not part of the public modem_hal.h
 *     surface, since nothing outside the modem layer ever called them.
 *
 * Official flow (Espressif's own usb_cdc_4g_module example):
 *   1. usbh_cdc_driver_install()
 *   2. usbh_modem_install()  -- VID 0x1E0E PID 0x9011, modem ITF 5
 *   3. usbh_modem_ppp_auto_connect(true)  [default]
 *   4. wait for IP_EVENT_PPP_GOT_IP (do not call ppp_start with auto-connect)
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"

#include "iot_usbh_cdc.h"
#include "iot_usbh_modem.h"
#include "at_3gpp_ts_27_007.h"

#include "modem_hal.h"
#include "config.h"

static const char *TAG = "MODEM_SIMCOM";

static bool s_cdc_installed = false;
static bool s_modem_installed = false;

/*
 * Same IDs as Espressif usb_cdc_4g_module for A7600/A7670.
 * A7672S enumerates as 0x1E0E:0x9011 (modem interface 5).
 */
static const usb_modem_id_t s_modem_id_list[] = {
    {
        .match_id = {
            .match_flags = USB_DEVICE_ID_MATCH_VID_PID,
            .idVendor = 0x1E0E,
            .idProduct = 0x9011,
        },
        .modem_itf_num = 5,
        .at_itf_num = -1,
        .name = "SIMCOM A7600/A7670/A7672",
    },
    {
        .match_id = {0},
        .modem_itf_num = 0,
        .at_itf_num = -1,
        .name = NULL,
    },
};

esp_err_t modem_install(const char *apn)
{
    if (s_modem_installed) {
        return ESP_ERR_INVALID_STATE;
    }

    /* ESP32-S3-USB-OTG DevKit: GPIO19/20 are muxed between the USB_DEV
     * and USB_HOST Type-A connectors. USB_SEL defaults low (USB_DEV) on
     * power-up, so without this the A7672S on USB_HOST is electrically
     * disconnected and will never enumerate. VBUS_EN/BOOST_EN are left
     * untouched: the modem is expected to be independently powered
     * (USB_HOST's own supply is capped at 500 mA, well under the
     * A7672S's >2A TX bursts), and driving VBUS from both sides at once
     * risks two 5V sources fighting on the same bus. */
    ESP_LOGI(TAG, "Routing native USB to USB_HOST connector (GPIO%d high)...", OTG_USB_SEL_GPIO);
    gpio_reset_pin(OTG_USB_SEL_GPIO);
    gpio_set_direction(OTG_USB_SEL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(OTG_USB_SEL_GPIO, 1);

    ESP_LOGI(TAG, "Installing USB CDC host driver...");
    usbh_cdc_driver_config_t cdc_cfg = {
        .task_stack_size = 1024 * 4,
        .task_priority = configMAX_PRIORITIES - 1,
        .task_coreid = 0,
        .skip_init_usb_host_driver = false,
    };
    esp_err_t err = usbh_cdc_driver_install(&cdc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usbh_cdc_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    s_cdc_installed = true;

    /* Optional PWRKEY hold -- pulse later only if needed */
    gpio_reset_pin(MODEM_PWRKEY_GPIO);
    gpio_set_direction(MODEM_PWRKEY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(MODEM_PWRKEY_GPIO, 1);

    ESP_LOGI(TAG, "Installing iot_usbh_modem (A7672S 0x1E0E:0x9011 if=5)...");
    if (apn && apn[0]) {
        ESP_LOGI(TAG, "  PDP APN: %s", apn);
    }

    usbh_modem_config_t modem_cfg = {
        .modem_id_list = s_modem_id_list,
        .at_tx_buffer_size = 512,
        .at_rx_buffer_size = 512,
        .pdp = {
            .enable = (apn != NULL && apn[0] != '\0'),
            .cid = 1,
            .type = "IP",
            .apn = (apn && apn[0]) ? apn : "",
        },
    };

    err = usbh_modem_install(&modem_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usbh_modem_install failed: %s", esp_err_to_name(err));
        usbh_cdc_driver_uninstall();
        s_cdc_installed = false;
        return err;
    }
    s_modem_installed = true;

    /* Auto-reconnect on hot-plug / recovery (Espressif default = on). This
     * is why modem_ppp_start() below is a no-op: the daemon dials itself
     * once the USB device enumerates, and calling usbh_modem_ppp_start()
     * on top of that returns ESP_ERR_INVALID_STATE. */
    err = usbh_modem_ppp_auto_connect(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "usbh_modem_ppp_auto_connect: %s", esp_err_to_name(err));
    }

    esp_netif_t *nif = usbh_modem_get_netif();
    if (nif) {
        (void)esp_netif_ppp_set_auth(nif, NETIF_PPP_AUTHTYPE_NONE, "", "");
    }

    ESP_LOGI(TAG, "usbh_modem installed -- waiting %d ms for USB enum / boot",
             MODEM_USB_BOOT_WAIT_MS);
    vTaskDelay(pdMS_TO_TICKS(MODEM_USB_BOOT_WAIT_MS));
    return ESP_OK;
}

esp_err_t modem_uninstall(void)
{
    esp_err_t err = ESP_OK;
    if (s_modem_installed) {
        /* usbh_modem_uninstall() stops PPP if STAGE_RUNNING. Calling
         * usbh_modem_ppp_stop() first can block forever when hang-up
         * fails (ATH while still in data mode). */
        err = usbh_modem_uninstall();
        s_modem_installed = false;
    }
    if (s_cdc_installed) {
        esp_err_t e2 = usbh_cdc_driver_uninstall();
        if (err == ESP_OK) {
            err = e2;
        }
        s_cdc_installed = false;
    }
    ESP_LOGW(TAG, "USB modem + CDC uninstalled");
    return err;
}

bool modem_is_installed(void)
{
    return s_modem_installed;
}

esp_netif_t *modem_get_netif(void)
{
    return usbh_modem_get_netif();
}

esp_err_t modem_ppp_start(void)
{
    /* Deliberate no-op -- see the file-header comment and the
     * usbh_modem_ppp_auto_connect(true) call in modem_install() above.
     * ppp_manager (a later step) waits for IP_EVENT_PPP_GOT_IP the same
     * way regardless of which backend is active; this backend just never
     * needs an explicit dial trigger to produce that event. */
    ESP_LOGD(TAG, "modem_ppp_start: no-op (auto-connect already dialing)");
    return ESP_OK;
}

esp_err_t modem_ppp_stop(void)
{
    return usbh_modem_ppp_stop();
}

void modem_read_status(char *op_buf, size_t op_len, int *rssi_out)
{
    if (op_buf && op_len) {
        strncpy(op_buf, "-", op_len - 1);
        op_buf[op_len - 1] = '\0';
    }
    if (rssi_out) {
        *rssi_out = 99;
    }

    at_handle_t at = usbh_modem_get_atparser();
    if (at == NULL) {
        return;
    }

    if (op_buf && op_len) {
        char tmp[64] = {0};
        if (at_cmd_get_operator_name(at, tmp, sizeof(tmp)) == ESP_OK && tmp[0]) {
            /* Response may include quotes / COPS fields -- copy printable. */
            const char *q1 = strchr(tmp, '"');
            if (q1) {
                q1++;
                const char *q2 = strchr(q1, '"');
                if (q2 && q2 > q1) {
                    size_t n = (size_t)(q2 - q1);
                    if (n >= op_len) {
                        n = op_len - 1;
                    }
                    memcpy(op_buf, q1, n);
                    op_buf[n] = '\0';
                }
            } else {
                strncpy(op_buf, tmp, op_len - 1);
                op_buf[op_len - 1] = '\0';
            }
        }
    }

    if (rssi_out) {
        esp_modem_at_csq_t csq = {.rssi = 99, .ber = 99};
        if (at_cmd_get_signal_quality(at, &csq) == ESP_OK) {
            *rssi_out = csq.rssi;
        }
    }
}

esp_err_t modem_hard_power_cycle(void)
{
    ESP_LOGW(TAG, "Hard power-cycling modem via PWRKEY (GPIO%d)", MODEM_PWRKEY_GPIO);

    gpio_reset_pin(MODEM_PWRKEY_GPIO);
    gpio_set_direction(MODEM_PWRKEY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(MODEM_PWRKEY_GPIO, 1);

    ESP_LOGW(TAG, "PWRKEY low %d ms (power off)", MODEM_PWRKEY_OFF_PULSE_MS);
    gpio_set_level(MODEM_PWRKEY_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(MODEM_PWRKEY_OFF_PULSE_MS));
    gpio_set_level(MODEM_PWRKEY_GPIO, 1);

    ESP_LOGW(TAG, "Settling %d ms (shutdown + mandatory off->on buffer)", MODEM_PWRKEY_SETTLE_MS);
    vTaskDelay(pdMS_TO_TICKS(MODEM_PWRKEY_SETTLE_MS));

    ESP_LOGW(TAG, "PWRKEY low %d ms (power on)", MODEM_PWRKEY_ON_PULSE_MS);
    gpio_set_level(MODEM_PWRKEY_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(MODEM_PWRKEY_ON_PULSE_MS));
    gpio_set_level(MODEM_PWRKEY_GPIO, 1);

    return ESP_OK;
}

esp_err_t modem_hard_reset(void)
{
    /* No RST line wired for this module -- PWRKEY is the only hardware
     * reset path (see MIGRATION_GUIDE.md / README.md of the original
     * project: "A7672S project only had PWRKEY"). Documented failure
     * rather than a missing symbol, so main.c's recovery ladder (a later
     * step) can call modem_hard_reset() unconditionally on every backend
     * and just skip ahead on this specific error. */
    ESP_LOGW(TAG, "modem_hard_reset: not supported on this module (no RST line wired) -- use modem_hard_power_cycle() instead");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t modem_force_reset(void)
{
    at_handle_t at = usbh_modem_get_atparser();
    if (at == NULL) {
        ESP_LOGW(TAG, "modem_force_reset: no AT parser available, skipping");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "Sending AT+CFUN=1,1 (modem reboot) -- recovering wedged state");
    esp_err_t err = at_send_command_response_ok(at, "AT+CFUN=1,1");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AT+CFUN=1,1 failed: %s", esp_err_to_name(err));
    }
    return err;
}
