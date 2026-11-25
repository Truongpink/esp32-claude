#include "chatai_audio_codec_bridge.h"

#include "audio_manager.h"

#include <algorithm>
#include <climits>
#include <cmath>

namespace chatai::audio {

esp_err_t ChatAIAudioCodecBridge::Start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (started_.load()) {
        return ESP_OK;
    }
    // The caller must initialize AudioManager first. Do not create another I2S
    // channel or change the existing hardware ownership model here.
    started_.store(true);
    last_error_.store(ESP_OK);
    return ESP_OK;
}

esp_err_t ChatAIAudioCodecBridge::Stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    esp_err_t result = ESP_OK;

    if (input_enabled_.load()) {
        result = chatai::audio::EnableMicrophone(false);
        if (result == ESP_OK) {
            input_enabled_.store(false);
        }
    }
    if (output_enabled_.load()) {
        const esp_err_t output_result = chatai::audio::EnableSpeaker(false);
        if (output_result == ESP_OK) {
            output_enabled_.store(false);
        }
        if (result == ESP_OK) {
            result = output_result;
        }
    }

    if (result == ESP_OK) {
        started_.store(false);
    }
    last_error_.store(result);
    return result;
}

esp_err_t ChatAIAudioCodecBridge::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!started_.load()) {
        last_error_.store(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if (enable == input_enabled_.load()) {
        return ESP_OK;
    }

    const esp_err_t result = chatai::audio::EnableMicrophone(enable);
    last_error_.store(result);
    if (result == ESP_OK) {
        input_enabled_.store(enable);
    }
    return result;
}

esp_err_t ChatAIAudioCodecBridge::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!started_.load()) {
        last_error_.store(ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if (enable == output_enabled_.load()) {
        return ESP_OK;
    }

    const esp_err_t result = chatai::audio::EnableSpeaker(enable);
    last_error_.store(result);
    if (result == ESP_OK) {
        output_enabled_.store(enable);
    }
    return result;
}

void ChatAIAudioCodecBridge::SetOutputVolume(int volume_percent) {
    chatai::audio::SetOutputVolume(std::clamp(volume_percent, 0, 100));
    last_error_.store(ESP_OK);
}

esp_err_t ChatAIAudioCodecBridge::SetInputGain(float gain) {
    if (!std::isfinite(gain) ||
        (gain != 0.0f && (gain < 0.5012f || gain > 1.9953f))) {
        last_error_.store(ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }
    const int gain_db = gain == 0.0f ? 0 : static_cast<int>(std::lround(20.0f * std::log10(gain)));
    chatai::audio::SetInputGainDb(gain_db);
    last_error_.store(ESP_OK);
    return ESP_OK;
}

bool ChatAIAudioCodecBridge::InputData(std::vector<int16_t>& pcm) {
    if (pcm.empty() || pcm.size() > static_cast<size_t>(INT_MAX)) {
        last_error_.store(ESP_ERR_INVALID_SIZE);
        return false;
    }

    const int samples_read = Read(pcm.data(), static_cast<int>(pcm.size()));
    if (samples_read <= 0) {
        return false;
    }

    pcm.resize(static_cast<size_t>(samples_read));
    return true;
}

esp_err_t ChatAIAudioCodecBridge::OutputData(const std::vector<int16_t>& pcm) {
    if (pcm.empty() || pcm.size() > static_cast<size_t>(INT_MAX)) {
        last_error_.store(ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    const int samples_written = Write(pcm.data(), static_cast<int>(pcm.size()));
    if (samples_written == static_cast<int>(pcm.size())) {
        return ESP_OK;
    }
    return last_error_.load() == ESP_OK ? ESP_ERR_TIMEOUT : last_error_.load();
}

int ChatAIAudioCodecBridge::Read(int16_t* destination, int samples) {
    if (!started_.load() || !input_enabled_.load()) {
        last_error_.store(ESP_ERR_INVALID_STATE);
        return 0;
    }
    if (destination == nullptr || samples <= 0) {
        last_error_.store(ESP_ERR_INVALID_ARG);
        return 0;
    }

    size_t samples_read = 0;
    const esp_err_t result = ReadMicrophonePcm16(
        destination, static_cast<size_t>(samples), &samples_read, kIoTimeoutMs);
    last_error_.store(result);
    if (result != ESP_OK || samples_read > static_cast<size_t>(samples)) {
        return 0;
    }
    return static_cast<int>(samples_read);
}

int ChatAIAudioCodecBridge::Write(const int16_t* samples, int count) {
    if (!started_.load() || !output_enabled_.load()) {
        last_error_.store(ESP_ERR_INVALID_STATE);
        return 0;
    }
    if (samples == nullptr || count <= 0) {
        last_error_.store(ESP_ERR_INVALID_ARG);
        return 0;
    }

    size_t samples_written = 0;
    const esp_err_t result = WriteSpeakerPcm16(
        samples, static_cast<size_t>(count), &samples_written, kIoTimeoutMs);
    last_error_.store(result);
    if (samples_written > static_cast<size_t>(count)) {
        last_error_.store(ESP_ERR_INVALID_SIZE);
        return 0;
    }
    return static_cast<int>(samples_written);
}

int ChatAIAudioCodecBridge::output_volume() const {
    return chatai::audio::GetOutputVolume();
}

}  // namespace chatai::audio
