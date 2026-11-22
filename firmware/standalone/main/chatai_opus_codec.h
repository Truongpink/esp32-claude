#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <esp_err.h>

namespace chatai::audio {

// Opus boundary for Xiaozhi voice sessions.
// Uplink is mono 16 kHz / 60 ms; downlink parameters come from server hello.
class ChatAIOpusCodec final {
public:
    static constexpr uint32_t kInputSampleRate = 16000;
    static constexpr uint32_t kInputFrameDurationMs = 60;
    static constexpr size_t kInputFrameSamples =
        kInputSampleRate * kInputFrameDurationMs / 1000;

    ChatAIOpusCodec() = default;
    ~ChatAIOpusCodec();

    ChatAIOpusCodec(const ChatAIOpusCodec&) = delete;
    ChatAIOpusCodec& operator=(const ChatAIOpusCodec&) = delete;

    esp_err_t Initialize();
    void Deinitialize();

    esp_err_t EncodeFrame(const int16_t* pcm, size_t sample_count,
                          std::vector<uint8_t>& encoded);

    esp_err_t DecodeFrame(const uint8_t* encoded, size_t encoded_size,
                          uint32_t sample_rate, uint32_t frame_duration_ms,
                          std::vector<int16_t>& pcm);

    esp_err_t Reset();

private:
    static bool IsSupportedSampleRate(uint32_t sample_rate);
    static bool GetDecoderFrameDuration(uint32_t duration_ms, int* duration);

    esp_err_t OpenDecoderLocked(uint32_t sample_rate,
                                uint32_t frame_duration_ms);

    std::mutex mutex_;
    void* encoder_ = nullptr;
    void* decoder_ = nullptr;
    uint32_t decoder_sample_rate_ = 0;
    uint32_t decoder_frame_duration_ms_ = 0;
    int encoder_input_bytes_ = 0;
    int encoder_output_bytes_ = 0;
};

}  // namespace chatai::audio
