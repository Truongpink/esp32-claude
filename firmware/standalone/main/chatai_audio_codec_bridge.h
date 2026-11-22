#pragma once

#include "pcm_audio_interface.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace chatai::audio {

// PCM-only adapter over ChatAI's AudioManager. It intentionally does not
// inherit Xiaozhi AudioCodec: that upstream type imports Board and Settings.
class ChatAIAudioCodecBridge final : public PcmAudioInterface {
public:
    static constexpr int kInputSampleRate = 16000;
    static constexpr int kOutputSampleRate = 24000;
    static constexpr int kChannels = 1;
    static constexpr uint32_t kIoTimeoutMs = 100;

    ChatAIAudioCodecBridge() = default;
    ChatAIAudioCodecBridge(const ChatAIAudioCodecBridge&) = delete;
    ChatAIAudioCodecBridge& operator=(const ChatAIAudioCodecBridge&) = delete;

    // AudioManager::Initialize() must already have succeeded.
    // Start only activates this adapter; it never initializes I2S hardware.
    esp_err_t Start() override;
    esp_err_t Stop() override;

    esp_err_t EnableInput(bool enable) override;
    esp_err_t EnableOutput(bool enable) override;

    // Runtime volume only. Persistence remains owned by ChatAI settings/UI.
    void SetOutputVolume(int volume_percent) override;

    // AudioManager currently exposes no microphone gain control.
    // Zero is accepted; non-zero gain is explicitly unsupported.
    esp_err_t SetInputGain(float gain) override;

    bool InputData(std::vector<int16_t>& pcm) override;
    esp_err_t OutputData(const std::vector<int16_t>& pcm) override;

    int Read(int16_t* destination, int samples) override;
    int Write(const int16_t* samples, int count) override;

    bool started() const override { return started_.load(); }
    bool input_enabled() const override { return input_enabled_.load(); }
    bool output_enabled() const override { return output_enabled_.load(); }
    bool duplex() const override { return true; }
    bool input_reference() const override { return false; }
    int input_sample_rate() const override { return kInputSampleRate; }
    int output_sample_rate() const override { return kOutputSampleRate; }
    int input_channels() const override { return kChannels; }
    int output_channels() const override { return kChannels; }
    int output_volume() const override;
    esp_err_t last_error() const override { return last_error_.load(); }

private:
    std::mutex lifecycle_mutex_;
    std::atomic<bool> started_{false};
    std::atomic<bool> input_enabled_{false};
    std::atomic<bool> output_enabled_{false};
    std::atomic<esp_err_t> last_error_{ESP_OK};
};

}  // namespace chatai::audio
