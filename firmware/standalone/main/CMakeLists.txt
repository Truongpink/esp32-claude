if(DEFINED ENV{CHATAI_FIRMWARE_BUILD})
    set(CHATAI_FIRMWARE_BUILD "$ENV{CHATAI_FIRMWARE_BUILD}")
else()
    set(CHATAI_FIRMWARE_BUILD "0")
endif()
if(NOT CHATAI_FIRMWARE_BUILD MATCHES "^[0-9]+$")
    message(FATAL_ERROR "CHATAI_FIRMWARE_BUILD must be a non-negative integer")
endif()

set(XIAOZHI_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/xiaozhi-esp32")
set(LANG_DIR "vi-VN")
set(LANG_JSON "${XIAOZHI_DIR}/main/assets/locales/${LANG_DIR}/language.json")
set(LANG_HEADER "${XIAOZHI_DIR}/main/assets/lang_config.h")

file(GLOB LANG_SOUNDS "${XIAOZHI_DIR}/main/assets/locales/${LANG_DIR}/*.ogg")
if(NOT LANG_DIR STREQUAL "en-US")
    file(GLOB EN_US_SOUNDS "${XIAOZHI_DIR}/main/assets/locales/en-US/*.ogg")
    set(EXISTING_NAMES "")
    foreach(SOUND_FILE ${LANG_SOUNDS})
        get_filename_component(FILENAME ${SOUND_FILE} NAME)
        list(APPEND EXISTING_NAMES ${FILENAME})
    endforeach()
    foreach(EN_SOUND ${EN_US_SOUNDS})
        get_filename_component(FILENAME ${EN_SOUND} NAME)
        if(NOT ${FILENAME} IN_LIST EXISTING_NAMES)
            list(APPEND LANG_SOUNDS ${EN_SOUND})
        endif()
    endforeach()
endif()
file(GLOB COMMON_SOUNDS "${XIAOZHI_DIR}/main/assets/common/*.ogg")

idf_component_register(
    SRCS "app_main.cpp" "network_manager.cpp" "bluetooth_manager.cpp" "display_manager.cpp"
         "audio_manager.cpp" "audio_codec.cpp" "chatai_audio_codec_bridge.cpp"
         "chatai_xiaozhi_audio_codec.cpp" "audio_service.cc" "audio_debugger.cc"
         "ogg_demuxer.cc" "afe_audio_engine.cc"
         "chatai_xiaozhi_application.cc" "chatai_xiaozhi_protocol.cc"
         "chatai_xiaozhi_mcp_server.cc" "chatai_xiaozhi_board.cpp"
         "ota_url.cc" "chatai_xiaozhi_client.cpp"
         "button_manager.cpp" "gemini_settings.cpp"
         "web_admin.cpp" "diagnostic_manager.cpp" "status_led_manager.cpp"
         "${XIAOZHI_DIR}/main/boards/common/board.cc"
         "${XIAOZHI_DIR}/main/settings.cc"
         "${XIAOZHI_DIR}/main/system_info.cc"
         "${XIAOZHI_DIR}/main/ota.cc"
         "${XIAOZHI_DIR}/main/device_state_machine.cc"
         "${XIAOZHI_DIR}/main/protocols/mqtt_protocol.cc"
         "${XIAOZHI_DIR}/main/protocols/websocket_protocol.cc"
         "chatai_xiaozhi_text_glyph_payload.cc"
         "${XIAOZHI_DIR}/main/display/text_glyph.cc"
         "${XIAOZHI_DIR}/main/notify/notify_player.cc"
    EMBED_FILES ${LANG_SOUNDS} ${COMMON_SOUNDS}
    INCLUDE_DIRS "." "${XIAOZHI_DIR}/main" "${XIAOZHI_DIR}/main/boards/common"
                 "${XIAOZHI_DIR}/main/display" "${XIAOZHI_DIR}/main/protocols"
    REQUIRES app_update spi_flash nvs_flash esp_event esp_netif esp_wifi lwip esp_http_server
             esp_http_client http_parser mdns esp_driver_i2c esp_driver_i2s esp_driver_gpio
             esp_driver_rmt esp_websocket_client esp_audio_codec esp_audio_effects esp-sr mqtt
             freertos mbedtls esp_timer esp-tls led_strip spiffs esp_psram esp_pm
             esp_app_format efuse pthread esp-ml307
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CHATAI_FIRMWARE_BUILD=${CHATAI_FIRMWARE_BUILD}
    CONFIG_USE_AUDIO_PROCESSOR=1
    CONFIG_USE_DEVICE_AEC=0
    CONFIG_USE_SERVER_AEC=0
    CONFIG_USE_AUDIO_DEBUGGER=0
    CONFIG_SEND_WAKE_WORD_DATA=0
    "BOARD_TYPE=\\\"wifi\\\""
    "BOARD_NAME=\\\"ChatAI TPIOS\\\""
    "BOARD_MANUFACTURER=\\\"TPIOS\\\""
    "CONFIG_OTA_URL=\\\"https://api.tenclass.net/xiaozhi/ota/\\\""
)

# ESP-IDF 6.1 adds fields to esp_timer_create_args_t; the pinned upstream
# MQTT source intentionally leaves optional fields zero-initialized. Keep
# -Werror for the rest of the component, but don't promote this warning in
# that upstream translation unit.
set_source_files_properties(
    "${XIAOZHI_DIR}/main/protocols/mqtt_protocol.cc"
    PROPERTIES COMPILE_OPTIONS "-Wno-error=missing-field-initializers"
)

target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>)

add_custom_command(
    OUTPUT ${LANG_HEADER}
    COMMAND python ${XIAOZHI_DIR}/scripts/gen_lang.py
            --language "${LANG_DIR}"
            --output "${LANG_HEADER}"
    DEPENDS
        ${LANG_JSON}
        ${XIAOZHI_DIR}/main/assets/locales/en-US/language.json
        ${XIAOZHI_DIR}/scripts/gen_lang.py
        ${LANG_SOUNDS}
        ${COMMON_SOUNDS}
    COMMENT "Generating ${LANG_DIR} Xiaozhi language config"
)
add_custom_target(lang_header ALL DEPENDS ${LANG_HEADER})
add_dependencies(${COMPONENT_LIB} lang_header)
