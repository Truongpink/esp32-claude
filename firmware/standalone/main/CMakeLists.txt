if(DEFINED ENV{CHATAI_FIRMWARE_BUILD})
    set(CHATAI_FIRMWARE_BUILD "$ENV{CHATAI_FIRMWARE_BUILD}")
else()
    set(CHATAI_FIRMWARE_BUILD "0")
endif()
if(NOT CHATAI_FIRMWARE_BUILD MATCHES "^[0-9]+$")
    message(FATAL_ERROR "CHATAI_FIRMWARE_BUILD must be a non-negative integer")
endif()

idf_component_register(
    SRCS "app_main.cpp" "network_manager.cpp" "display_manager.cpp"
         "audio_manager.cpp" "audio_codec.cpp" "chatai_audio_codec_bridge.cpp"
         "chatai_xiaozhi_audio_codec.cpp" "chatai_opus_codec.cpp"
         "chatai_xiaozhi_audio_runtime.cpp" "chatai_xiaozhi_ota.cpp"
         "button_manager.cpp" "gemini_settings.cpp"
         "gemini_live_client.cpp" "web_admin.cpp" "diagnostic_manager.cpp"
         "status_led_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES app_update spi_flash nvs_flash esp_event esp_netif esp_wifi lwip esp_http_server
             esp_http_client mdns esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_driver_rmt
             esp_websocket_client esp_audio_codec freertos mbedtls esp_timer esp-tls
             led_strip spiffs esp_psram
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE CHATAI_FIRMWARE_BUILD=${CHATAI_FIRMWARE_BUILD})
