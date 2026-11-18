idf_component_register(
    SRCS "app_main.cpp" "network_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_event esp_netif esp_wifi
)
