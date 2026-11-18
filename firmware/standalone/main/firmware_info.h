#pragma once

#include <cstdint>

#ifndef CHATAI_FIRMWARE_BUILD
#define CHATAI_FIRMWARE_BUILD 0
#endif

#define CHATAI_STRINGIFY_INNER(value) #value
#define CHATAI_STRINGIFY(value) CHATAI_STRINGIFY_INNER(value)

namespace chatai::firmware {
inline constexpr uint32_t kBuildNumber = CHATAI_FIRMWARE_BUILD;
inline constexpr char kBuildMarker[] =
    "CHATAI_BUILD_NUMBER=" CHATAI_STRINGIFY(CHATAI_FIRMWARE_BUILD);
}  // namespace chatai::firmware
