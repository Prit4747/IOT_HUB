/*
 * modem_quectel_ec200u_uart.cpp -- modem_hal.h implementation for the
 * Quectel EC200U-CN (Vanix TracX-1b carrier), over UART PPPoS
 * (Espressif esp_modem).
 *
 * Only compiled in when CONFIG_MODEM_MODULE_QUECTEL_EC200U_UART is
 * selected (see main/CMakeLists.txt). Carried over from the original
 * Quectel_EC200U project's modem.cpp -- the modem_hal.h contract already
 * matched this backend's native shape almost exactly (modem_ppp_start()
 * already took no arguments here, modem_hard_reset() already existed),
 * so unlike the SimCom backend, nothing had to be reshaped to fit.
 *
 * Why C++ (esp_modem's native C++ API, not its C shim esp_modem_api.h):
 * the C shim's command declarations are generated from the same
 * macro-based list for both C and C++ callers, and one of them
 * (set_pdp_context's STRUCT_OUT) references a bare `PdpContext` type only
 * in scope from a handful of internal headers -- esp_modem_api.h fails to
 * compile from a .cpp file for that reason alone. This is also why
 * esp_modem's own C-shim example (pppos_client) is a plain .c file. Every
 * function below is still declared extern "C" (via modem_hal.h), so
 * main.c (plain C) links against this exactly like it does the SimCom
 * backend -- the C++-ness is fully contained inside this one file.
 *
 * Reference flow (esp_modem's own examples/pppos_client, adapted to the
 * generic-DCE case since EC200U-CN isn't one of esp_modem's named devices
 * -- BG96/SIM800/SIM7000/SIM7070/SIM7600/EC20/SQNGM02S):
 *   1. esp_netif_new(ESP_NETIF_DEFAULT_PPP())
 *   2. esp_modem::create_uart_dte(&dte_config)
 *   3. esp_modem::create_generic_dce(&dce_config, dte, netif)
 *   4. dce->set_mode(DATA_MODE)    -- dials PPP (modem_ppp_start())
 *   5. wait for IP_EVENT_PPP_GOT_IP (ppp_manager.c)
 *   6. dce->set_mode(COMMAND_MODE) -- hang up (modem_ppp_stop())
 */

#include <cstring>
#include <cstdio>
#include <memory>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_modem_config.h"
#include "cxx_include/esp_modem_api.hpp"

#include "modem_hal.h"
#include "config.h"

static const char *TAG = "MODEM_QUECTEL_UART";

static esp_netif_t *s_netif = nullptr;
static std::shared_ptr<esp_modem::DTE> s_dte;
static std::unique_ptr<esp_modem::DCE> s_dce;

esp_err_t modem_install(const char *apn)
{
    if (s_dce) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Optional PWRKEY / RST -- hold idle-high, pulse later only if needed.
     * The EC200U-CN auto-powers-on when VBAT/USB is applied, so neither
     * pin needs to be touched for a normal cold boot. */
    gpio_reset_pin((gpio_num_t)MODEM_PWRKEY_GPIO);
    gpio_set_direction((gpio_num_t)MODEM_PWRKEY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)MODEM_PWRKEY_GPIO, 1);

    gpio_reset_pin((gpio_num_t)MODEM_RST_GPIO);
    gpio_set_direction((gpio_num_t)MODEM_RST_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)MODEM_RST_GPIO, 1);

    ESP_LOGI(TAG, "Waiting %d ms for modem boot before opening UART...", MODEM_BOOT_WAIT_MS);
    vTaskDelay(pdMS_TO_TICKS(MODEM_BOOT_WAIT_MS));

    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    s_netif = esp_netif_new(&netif_ppp_config);
    if (!s_netif) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return ESP_FAIL;
    }

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    /* Default task_priority (5) ties with ota_task's priority
     * (ota_manager.c) -- under sustained OTA load (TLS decrypt + flash
     * writes), that can starve the UART driver task long enough for the
     * 128-byte hardware FIFO to overflow before anything drains it,
     * corrupting PPP/AT traffic mid-transfer. Raised well above ota_task
     * and other app-level tasks, but still below WiFi's priority (23) and
     * other system-critical tasks. */
    dte_config.task_priority = 15;
    dte_config.uart_config.rx_buffer_size = 8192;
    dte_config.uart_config.port_num = (uart_port_t)MODEM_UART_PORT_NUM;
    dte_config.uart_config.tx_io_num = MODEM_UART_TX_GPIO;
    dte_config.uart_config.rx_io_num = MODEM_UART_RX_GPIO;
    dte_config.uart_config.rts_io_num = MODEM_UART_RTS_GPIO;
    dte_config.uart_config.cts_io_num = MODEM_UART_CTS_GPIO;
    dte_config.uart_config.flow_control = MODEM_FLOW_CONTROL;
    dte_config.uart_config.baud_rate = MODEM_UART_BAUD_RATE;

    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(apn ? apn : "");

    ESP_LOGI(TAG, "Initializing esp_modem (generic DCE) for Quectel EC200U-CN on UART%d "
             "(tx=%d rx=%d rts=%d cts=%d baud=%d)...",
             MODEM_UART_PORT_NUM, MODEM_UART_TX_GPIO, MODEM_UART_RX_GPIO,
             MODEM_UART_RTS_GPIO, MODEM_UART_CTS_GPIO, MODEM_UART_BAUD_RATE);
    if (apn && apn[0]) {
        ESP_LOGI(TAG, "  PDP APN: %s", apn);
    }

    s_dte = esp_modem::create_uart_dte(&dte_config);
    if (!s_dte) {
        ESP_LOGE(TAG, "create_uart_dte failed");
        esp_netif_destroy(s_netif);
        s_netif = nullptr;
        return ESP_FAIL;
    }

    s_dce = esp_modem::create_generic_dce(&dce_config, s_dte, s_netif);
    if (!s_dce) {
        ESP_LOGE(TAG, "create_generic_dce failed");
        s_dte.reset();
        esp_netif_destroy(s_netif);
        s_netif = nullptr;
        return ESP_FAIL;
    }

    int rssi = 0, ber = 0;
    if (s_dce->get_signal_quality(rssi, ber) == esp_modem::command_result::OK) {
        ESP_LOGI(TAG, "Modem responding: signal quality rssi=%d ber=%d", rssi, ber);
    } else {
        ESP_LOGW(TAG, "get_signal_quality failed -- check UART wiring/baud");
    }

    /* Don't dial PPP until the modem has actually attached to the network
     * (AT+CGATT). Skipping this and dialing right after the fixed boot
     * delay means the first attempt (and often several more) races LTE
     * registration and fails every time -- not a wiring/signal problem,
     * just bad timing. */
    ESP_LOGI(TAG, "Waiting for network attachment (AT+CGATT) up to %d ms...",
             MODEM_NET_ATTACH_TIMEOUT_MS);
    int waited_ms = 0;
    bool attached = false;
    while (waited_ms < MODEM_NET_ATTACH_TIMEOUT_MS) {
        int state = 0;
        if (s_dce->get_network_attachment_state(state) == esp_modem::command_result::OK &&
                state == 1) {
            attached = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(MODEM_NET_ATTACH_POLL_MS));
        waited_ms += MODEM_NET_ATTACH_POLL_MS;
    }
    if (attached) {
        ESP_LOGI(TAG, "Network attached after %d ms", waited_ms);
    } else {
        ESP_LOGW(TAG, "Network attachment timed out after %d ms -- dialing anyway",
                 MODEM_NET_ATTACH_TIMEOUT_MS);
    }

    return ESP_OK;
}

esp_err_t modem_uninstall(void)
{
    s_dce.reset();
    s_dte.reset();
    if (s_netif) {
        esp_netif_destroy(s_netif);
        s_netif = nullptr;
    }
    ESP_LOGW(TAG, "Modem DCE + PPP netif destroyed");
    return ESP_OK;
}

bool modem_is_installed(void)
{
    return (bool)s_dce;
}

esp_netif_t *modem_get_netif(void)
{
    return s_netif;
}

esp_err_t modem_ppp_start(void)
{
    if (!s_dce) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "set_mode(DATA_MODE) -- dialing PPP...");
    return s_dce->set_mode(esp_modem::modem_mode::DATA_MODE) ? ESP_OK : ESP_FAIL;
}

esp_err_t modem_ppp_stop(void)
{
    if (!s_dce) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_dce->set_mode(esp_modem::modem_mode::COMMAND_MODE) ? ESP_OK : ESP_FAIL;
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

    if (!s_dce) {
        return;
    }

    if (op_buf && op_len) {
        std::string name;
        int act = 0;
        if (s_dce->get_operator_name(name, act) == esp_modem::command_result::OK && !name.empty()) {
            strncpy(op_buf, name.c_str(), op_len - 1);
            op_buf[op_len - 1] = '\0';
        }
    }

    if (rssi_out) {
        int rssi = 99, ber = 99;
        if (s_dce->get_signal_quality(rssi, ber) == esp_modem::command_result::OK) {
            *rssi_out = rssi;
        }
    }
}

esp_err_t modem_hard_power_cycle(void)
{
    ESP_LOGW(TAG, "Hard power-cycling modem via PWRKEY (GPIO%d)", MODEM_PWRKEY_GPIO);

    gpio_reset_pin((gpio_num_t)MODEM_PWRKEY_GPIO);
    gpio_set_direction((gpio_num_t)MODEM_PWRKEY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)MODEM_PWRKEY_GPIO, 1);

    ESP_LOGW(TAG, "PWRKEY low %d ms (power off)", MODEM_PWRKEY_OFF_PULSE_MS);
    gpio_set_level((gpio_num_t)MODEM_PWRKEY_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(MODEM_PWRKEY_OFF_PULSE_MS));
    gpio_set_level((gpio_num_t)MODEM_PWRKEY_GPIO, 1);

    ESP_LOGW(TAG, "Settling %d ms (shutdown)", MODEM_PWRKEY_SETTLE_MS);
    vTaskDelay(pdMS_TO_TICKS(MODEM_PWRKEY_SETTLE_MS));

    ESP_LOGW(TAG, "PWRKEY low %d ms (power on)", MODEM_PWRKEY_ON_PULSE_MS);
    gpio_set_level((gpio_num_t)MODEM_PWRKEY_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(MODEM_PWRKEY_ON_PULSE_MS));
    gpio_set_level((gpio_num_t)MODEM_PWRKEY_GPIO, 1);

    return ESP_OK;
}

esp_err_t modem_hard_reset(void)
{
    ESP_LOGW(TAG, "Hard-resetting modem via RST (GPIO%d) -- ungraceful, last resort", MODEM_RST_GPIO);

    gpio_reset_pin((gpio_num_t)MODEM_RST_GPIO);
    gpio_set_direction((gpio_num_t)MODEM_RST_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)MODEM_RST_GPIO, 1);

    gpio_set_level((gpio_num_t)MODEM_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(MODEM_RST_PULSE_MS));
    gpio_set_level((gpio_num_t)MODEM_RST_GPIO, 1);

    return ESP_OK;
}

esp_err_t modem_force_reset(void)
{
    if (!s_dce) {
        ESP_LOGW(TAG, "modem_force_reset: no DCE installed, skipping");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "Sending AT+CFUN=1,1 (modem reboot) -- recovering wedged state");
    std::string out;
    auto res = s_dce->at("AT+CFUN=1,1", out, 10000);
    if (res != esp_modem::command_result::OK) {
        ESP_LOGW(TAG, "AT+CFUN=1,1 failed (result=%d)", (int)res);
        return ESP_FAIL;
    }
    return ESP_OK;
}
