#include "chatai_xiaozhi_audio_codec.h"

#include <algorithm>
#include <utility>

namespace chatai::audio {

ChatAIXiaozhiAudioCodec::ChatAIXiaozhiAudioCodec() {
    duplex_ = true;
    input_reference_ = false;
    input_sample_rate_ = ChatAIAudioCodecBridge::kInputSampleRate;
    output_sample_rate_ = ChatAIAudioCodecBridge::kOutputSampleRate;
    input_channels_ = ChatAIAudioCodecBridge::kChannels;
    output_channels_ = ChatAIAudioCodecBridge::kChannels;
    output_volume_ = bridge_.output_volume();
    input_gain_ = 0.0f;
}

void ChatAIXiaozhiAudioCodec::Start() {
    last_error_.store(bridge_.Start());
}

esp_err_t ChatAIXiaozhiAudioCodec::Stop() {
    const esp_err_t result = bridge_.Stop();
    last_error_.store(result);
    if (result == ESP_OK) {
        input_enabled_ = false;
        output_enabled_ = false;
    }
    return result;
}

void ChatAIXiaozhiAudioCodec::SetOutputVolume(int volume) {
    output_volume_ = std::clamp(volume, 0, 100);
    bridge_.SetOutputVolume(output_volume_);
    last_error_.store(bridge_.last_error());
}

void ChatAIXiaozhiAudioCodec::SetInputGain(float gain) {
    const esp_err_t result = bridge_.SetInputGain(gain);
    last_error_.store(result);
    if (result == ESP_OK) {
        input_gain_ = gain;
    }
}

void ChatAIXiaozhiAudioCodec::EnableInput(bool enable) {
    const esp_err_t result = bridge_.EnableInput(enable);
    last_error_.store(result);
    if (result == ESP_OK) {
        input_enabled_ = enable;
    }
}

void ChatAIXiaozhiAudioCodec::EnableOutput(bool enable) {
    const esp_err_t result = bridge_.EnableOutput(enable);
    last_error_.store(result);
    if (result == ESP_OK) {
        output_enabled_ = enable;
    }
}

void ChatAIXiaozhiAudioCodec::OutputData(std::vector<int16_t>& data) {
    last_error_.store(bridge_.OutputData(data));
}

bool ChatAIXiaozhiAudioCodec::InputData(std::vector<int16_t>& data) {
    const bool received = bridge_.InputData(data);
    last_error_.store(bridge_.last_error());
    return received;
}

int ChatAIXiaozhiAudioCodec::Read(int16_t* destination, int samples) {
    const int received = bridge_.Read(destination, samples);
    last_error_.store(bridge_.last_error());
    return received;
}

int ChatAIXiaozhiAudioCodec::Write(const int16_t* samples, int count) {
    const int written = bridge_.Write(samples, count);
    last_error_.store(bridge_.last_error());
    return written;
}

}  // namespace chatai::audio
