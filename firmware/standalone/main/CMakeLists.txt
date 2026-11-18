idf_component_register(
    SRCS "app_main.cpp" "network_manager.cpp" "display_manager.cpp"
         "audio_manager.cpp" "gemini_settings.cpp" "gemini_live_client.cpp"
         "web_admin.cpp"
    INCLUDE_DIRS "."
    REQUIRES app_update nvs_flash esp_event esp_netif esp_wifi esp_http_server
             esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_websocket_client
             freertos mbedtls
)
