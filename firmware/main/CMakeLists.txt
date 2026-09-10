idf_component_register(
    SRCS
        "main.c"
        "attention_audio.c"
        "wifi_manager.c"
        "attention_client.c"
        "attention_pairing.c"
        "attention_pairing_record.c"
        "attention_connection.c"
        "attention_connection_status.c"
        "attention_confirmation.c"
        "attention_provisioning.c"
        "attention_ui.c"
        "attention_display.c"
        "display_frame.c"
        "button_input.c"
        "voice_audio.c"
        "usb_microphone.c"
        "wireless_microphone_protocol.c"
        "wireless_microphone.c"
        "wireless_certificate.c"
        "voice_control.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_timer
    REQUIRES
        esp_driver_uart
        esp_hw_support
        esp_driver_gpio
        esp_driver_i2c
        esp_event
        esp_lcd
        esp_codec_dev
        esp_http_client
        esp_netif
        lwip
        mbedtls
        esp_wifi
        esp_websocket_client
        json
        nvs_flash
        nvs_sec_provider
        mdns
        usb_device_uac
        espressif__tinyusb
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
# Network worker frames must leave room for HTTP and LVGL call chains, even
# with the maximum configured card count. Large models belong in PSRAM.
set_source_files_properties(main.c PROPERTIES COMPILE_OPTIONS "-Wframe-larger-than=2048;-fstack-usage")
