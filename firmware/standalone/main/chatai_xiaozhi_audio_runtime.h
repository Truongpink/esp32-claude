#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "chatai_opus_codec.h"

namespace chatai::audio {

struct XiaozhiUplinkFrame {
    uint32_t sample_rate = ChatAIOpusCodec::kInputSampleRate;
    uint32_t frame_duration_ms = ChatAIOpusCodec::kInputFrameDurationMs;
    std::vector<uint8_t> payload;
};

// Owns session-time audio tasks and bounded packet queues. AudioManager remains
// the sole owner of the existing I2S channels.
class XiaozhiAudioRuntime final {
public:
    XiaozhiAudioRuntime() = default;
    ~XiaozhiAudioRuntime();

    XiaozhiAudioRuntime(const XiaozhiAudioRuntime&) = delete;
    XiaozhiAudioRuntime& operator=(const XiaozhiAudioRuntime&) = delete;

    esp_err_t Initialize();
    esp_err_t Start();
    esp_err_t Stop();

    // Network task retrieves encoded microphone frames without blocking capture.
    bool WaitPopUplinkFrame(XiaozhiUplinkFrame& frame, uint32_t timeout_ms);

    // Network task submits one complete Opus packet from the server.
    // The current speaker path is 24 kHz mono; other negotiated rates are
    // rejected until a resampler is deliberately integrated.
    esp_err_t SubmitDownlinkFrame(const uint8_t* payload, size_t payload_size,
                                  uint32_t sample_rate,
                                  uint32_t frame_duration_ms);

    bool IsRunning() const { return running_.load(); }

private:
    struct DownlinkFrame {
        uint32_t sample_rate = 0;
        uint32_t frame_duration_ms = 0;
        std::vector<uint8_t> payload;
    };

    static void CaptureTaskEntry(void* context);
    static void PlaybackTaskEntry(void* context);
    void CaptureTask();
    void PlaybackTask();

    static constexpr size_t kQueueDepth = 4;
    static constexpr EventBits_t kCaptureExited = 1 << 0;
    static constexpr EventBits_t kPlaybackExited = 1 << 1;
    static constexpr size_t kSpeakerFrameSamples = 24000 * 60 / 1000;

    ChatAIOpusCodec opus_;
    EventGroupHandle_t task_events_ = nullptr;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};

    std::mutex lifecycle_mutex_;
    std::mutex queue_mutex_;
    std::condition_variable uplink_cv_;
    std::condition_variable downlink_cv_;
    std::deque<XiaozhiUplinkFrame> uplink_queue_;
    std::deque<DownlinkFrame> downlink_queue_;
};

}  // namespace chatai::audio
