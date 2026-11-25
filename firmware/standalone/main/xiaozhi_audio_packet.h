#pragma once
#include <cstdint>
#include <vector>
struct AudioStreamPacket {
    int sample_rate=0;
    int frame_duration=0;
    uint32_t timestamp=0;
    uint32_t playback_id=0;
    uint32_t media_position_ms=0;
    std::vector<uint8_t> payload;
};
