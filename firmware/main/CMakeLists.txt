idf_component_register(
    SRCS
        "main.c"
        "attention_audio.c"
        "wifi_manager.c"
        "attention_client.c"
        "attention_ui.c"
        "attention_display.c"
        "display_frame.c"
        "button_input.c"
        "voice_audio.c"
        "usb_microphone.c"
        "wireless_microphone_protocol.c"
        "wireless_microphone.c"
        "voice_control.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_timer
    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_event
        esp_lcd
        esp_codec_dev
        esp_http_client
        esp_netif
        lwip
        esp_wifi
        esp_websocket_client
        json
        nvs_flash
        usb_device_uac
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
