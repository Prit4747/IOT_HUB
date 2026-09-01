# Step 4: pick exactly ONE modem backend source file, based on the Kconfig
# module choice (main/Kconfig.projbuild).
#
# TWO ESP-IDF quirks found by actually running this build, not assumed:
#
# 1. REQUIRES/PRIV_REQUIRES in idf_component_register() must NOT depend on
#    CONFIG_* values. ESP-IDF computes the full component-requirements
#    graph (which determines include paths and link order for the whole
#    build) in an early pass that runs BEFORE Kconfig is resolved -- every
#    CONFIG_* is unresolved there. Requirements gated behind CONFIG_ silently
#    end up missing from that graph even once the real build reaches the
#    correct config, producing exactly the confusing "component X's headers
#    used without being in REQUIRES" error this project hit. Fix: every
#    backend's component deps are listed UNCONDITIONALLY below. Harmless --
#    an unused managed component just isn't referenced by the one backend
#    .c/.cpp file that's actually compiled in (see SRCS), so nothing of it
#    ends up in the final link.
#
# 2. SRCS has no such restriction -- CONFIG_* IS reliably resolved by the
#    time idf_component_register()'s SRCS argument is actually evaluated,
#    confirmed by this exact file: MODEM_BACKEND_SRCS below correctly picks
#    modem_simcom_a7672_usb.c when that module is selected.
set(MODEM_BACKEND_SRCS "")

if(CONFIG_MODEM_MODULE_SIMCOM_A7672_USB)
    set(MODEM_BACKEND_SRCS "modem_simcom_a7672_usb.c")
elseif(CONFIG_MODEM_MODULE_QUECTEL_EC200U_UART)
    set(MODEM_BACKEND_SRCS "modem_quectel_ec200u_uart.cpp")
endif()
# Quectel EC200U-CN USB branch lands here in its own step, the same way.

idf_component_register(
    SRCS
        "main.c"
        # Steps 6-8: generic, transport-agnostic -- compiled unconditionally,
        # not gated by MODEM_BACKEND_SRCS like the modem backend above.
        "ppp_manager.c"
        "ota_manager.c"
        "web_server.c"
        ${MODEM_BACKEND_SRCS}
    INCLUDE_DIRS "."
    EMBED_FILES
        "web_ui.html"
    REQUIRES
        nvs_flash
        esp_netif
        freertos
        esp_event
        driver
        lwip
        # ota_manager.c deps
        esp_https_ota
        esp_http_client
        esp_delta_ota
        app_update
        esp_app_format
        mbedtls
        # web_server.c deps
        esp_wifi
        esp_http_server
        # SimCom A7672S/USB backend deps (modem_simcom_a7672_usb.c) --
        # unconditional, see note (1) above.
        espressif__iot_usbh_modem
        espressif__iot_usbh_cdc
        espressif__modem_at
        espressif__iot_eth
        # Quectel EC200U-CN/UART backend deps (modem_quectel_ec200u_uart.cpp)
        # -- unconditional, same reason.
        espressif__esp_modem
)

# Safe here (after idf_component_register, in the fully-resolved pass):
# fires only for a module that's genuinely selected in sdkconfig but has no
# backend source yet.
if(NOT MODEM_BACKEND_SRCS)
    if(CONFIG_MODEM_MODULE_QUECTEL_EC200U_USB)
        message(FATAL_ERROR
            "Quectel EC200U-CN USB backend not implemented yet (coming in "
            "a later step). Select \"SimCom A7672S -- USB host\" or "
            "\"Quectel EC200U-CN -- UART PPPoS\" in `idf.py menuconfig` "
            "for now.")
    else()
        message(FATAL_ERROR "No MODEM_MODULE_* selected -- run `idf.py menuconfig`")
    endif()
endif()
