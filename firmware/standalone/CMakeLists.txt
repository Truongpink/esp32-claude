cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{CHATAI_FIRMWARE_BUILD})
    set(CHATAI_FIRMWARE_BUILD "$ENV{CHATAI_FIRMWARE_BUILD}")
else()
    set(CHATAI_FIRMWARE_BUILD "0")
endif()
if(NOT CHATAI_FIRMWARE_BUILD MATCHES "^[0-9]+$")
    message(FATAL_ERROR "CHATAI_FIRMWARE_BUILD must be a non-negative integer")
endif()

# Keep ESP-IDF app metadata aligned with the build number embedded by the main component.
set(PROJECT_VER "${CHATAI_FIRMWARE_BUILD}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ChatAI_TPIOS)
