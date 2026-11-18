idf_component_register(
    SRCS "app_main.cpp" "network_manager.cpp" "display_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_event esp_netif esp_wifi esp_http_server
             esp_driver_i2c freertos
)
