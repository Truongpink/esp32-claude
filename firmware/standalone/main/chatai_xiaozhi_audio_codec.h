#pragma once

#include "audio_codec.h"
#include "chatai_audio_codec_bridge.h"

#include <atomic>

namespace chatai::audio {

class ChatAIXiaozhiAudioCodec final : public ::AudioCodec {
public:
    ChatAIXiaozhiAudioCodec();

    void Start() override;
    esp_err_t Stop();
    void SetOutputVolume(int volume) override;
    void SetInputGain(float gain) override;
    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;
    void OutputData(std::vector<int16_t>& data) override;
    bool InputData(std::vector<int16_t>& data) override;

    esp_err_t last_error() const { return last_error_.load(); }

protected:
    int Read(int16_t* destination, int samples) override;
    int Write(const int16_t* samples, int count) override;

private:
    ChatAIAudioCodecBridge bridge_;
    std::atomic<esp_err_t> last_error_{ESP_OK};
};

}  // namespace chatai::audio
