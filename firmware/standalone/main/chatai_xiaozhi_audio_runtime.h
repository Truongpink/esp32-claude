#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>
#include <esp_err.h>
#include "audio_service.h"
#include "chatai_xiaozhi_audio_codec.h"
namespace chatai::audio {
struct XiaozhiUplinkFrame { uint32_t sample_rate=16000; uint32_t frame_duration_ms=60; uint32_t timestamp_ms=0; std::vector<uint8_t> payload; };
class XiaozhiAudioRuntime final {
public:
    XiaozhiAudioRuntime()=default; ~XiaozhiAudioRuntime();
    XiaozhiAudioRuntime(const XiaozhiAudioRuntime&)=delete;
    XiaozhiAudioRuntime& operator=(const XiaozhiAudioRuntime&)=delete;
    esp_err_t Initialize(); esp_err_t Start(); esp_err_t Stop(); void SetCaptureEnabled(bool enabled);
    bool WaitPopUplinkFrame(XiaozhiUplinkFrame& frame,uint32_t timeout_ms);
    esp_err_t SubmitDownlinkFrame(const uint8_t* payload,size_t payload_size,uint32_t sample_rate,uint32_t frame_duration_ms);
    bool IsRunning() const{return running_.load();}
private:
    static constexpr size_t kMaxOpusPacketBytes=4096;
    ChatAIXiaozhiAudioCodec codec_; AudioService service_;
    std::atomic<bool> initialized_{false},running_{false};
    bool capture_enabled_=false;
    std::mutex lifecycle_mutex_,send_mutex_; std::condition_variable send_cv_; bool send_queue_notified_=false;
};
}
