#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <esp_err.h>

namespace chatai::audio {

// PCM-only boundary between ChatAI's AudioManager and a future Xiaozhi
// AudioService port. This intentionally does not inherit Xiaozhi AudioCodec:
// that upstream type imports Board and its implementation persists Settings.
class ChatAIAudioCodecBridge final {
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
    esp_err_t Start();
    esp_err_t Stop();

    esp_err_t EnableInput(bool enable);
    esp_err_t EnableOutput(bool enable);

    // Runtime volume only. Persistence remains owned by ChatAI settings/UI.
    void SetOutputVolume(int volume_percent);

    // AudioManager currently exposes no microphone gain control.
    // Zero is accepted; non-zero gain is explicitly unsupported.
    esp_err_t SetInputGain(float gain);

    bool InputData(std::vector<int16_t>& pcm);
    esp_err_t OutputData(const std::vector<int16_t>& pcm);

    int Read(int16_t* destination, int samples);
    int Write(const int16_t* samples, int count);

    bool started() const { return started_.load(); }
    bool input_enabled() const { return input_enabled_.load(); }
    bool output_enabled() const { return output_enabled_.load(); }
    bool duplex() const { return true; }
    bool input_reference() const { return false; }
    int input_sample_rate() const { return kInputSampleRate; }
    int output_sample_rate() const { return kOutputSampleRate; }
    int input_channels() const { return kChannels; }
    int output_channels() const { return kChannels; }
    int output_volume() const;
    esp_err_t last_error() const { return last_error_.load(); }

private:
    std::mutex lifecycle_mutex_;
    std::atomic<bool> started_{false};
    std::atomic<bool> input_enabled_{false};
    std::atomic<bool> output_enabled_{false};
    std::atomic<esp_err_t> last_error_{ESP_OK};
};

}  // namespace chatai::audio
