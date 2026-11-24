if(DEFINED ENV{CHATAI_FIRMWARE_BUILD})
    set(CHATAI_FIRMWARE_BUILD "$ENV{CHATAI_FIRMWARE_BUILD}")
else()
    set(CHATAI_FIRMWARE_BUILD "0")
endif()
if(NOT CHATAI_FIRMWARE_BUILD MATCHES "^[0-9]+$")
    message(FATAL_ERROR "CHATAI_FIRMWARE_BUILD must be a non-negative integer")
endif()

idf_component_register(
    SRCS "app_main.cpp" "network_manager.cpp" "bluetooth_manager.cpp" "display_manager.cpp"
         "audio_manager.cpp" "audio_codec.cpp" "chatai_audio_codec_bridge.cpp"
         "chatai_xiaozhi_audio_codec.cpp" "audio_service.cc" "audio_debugger.cc"
         "ogg_demuxer.cc" "afe_audio_engine.cc"
         "chatai_xiaozhi_audio_runtime.cpp" "chatai_xiaozhi_notify_player.cpp"
         "chatai_xiaozhi_ota.cpp"
         "chatai_xiaozhi_mqtt.cpp" "chatai_xiaozhi_websocket.cpp"
         "chatai_xiaozhi_client.cpp"
         "button_manager.cpp" "gemini_settings.cpp"
         "web_admin.cpp" "diagnostic_manager.cpp"
         "status_led_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES app_update spi_flash nvs_flash esp_event esp_netif esp_wifi lwip esp_http_server
             esp_http_client mdns bt esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_driver_rmt
             esp_websocket_client esp_audio_codec esp_audio_effects esp-sr mqtt freertos
             mbedtls esp_timer esp-tls led_strip spiffs esp_psram
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CHATAI_FIRMWARE_BUILD=${CHATAI_FIRMWARE_BUILD}
    CONFIG_USE_AUDIO_PROCESSOR=1
    CONFIG_USE_DEVICE_AEC=0
    CONFIG_USE_SERVER_AEC=0
    CONFIG_USE_AUDIO_DEBUGGER=0
    CONFIG_SEND_WAKE_WORD_DATA=0
)
