#include "audio_manager.h"

#include <algorithm>
#include <array>
#include <climits>
#include <mutex>

#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <esp_log.h>

namespace chatai::audio {
namespace {

constexpr char kTag[] = "ChatAIAudio";

// Provisional XH-S3E-AI V1.0 pin mapping copied from the existing board profile.
// Microphone pins are explicitly unverified on the physical board.
constexpr gpio_num_t kMicBclk = GPIO_NUM_5;
constexpr gpio_num_t kMicWs = GPIO_NUM_4;
constexpr gpio_num_t kMicData = GPIO_NUM_6;
constexpr gpio_num_t kSpeakerBclk = GPIO_NUM_15;
constexpr gpio_num_t kSpeakerWs = GPIO_NUM_16;
constexpr gpio_num_t kSpeakerData = GPIO_NUM_7;
constexpr gpio_num_t kSpeakerAmpEnable = GPIO_NUM_17;

constexpr uint32_t kMicSampleRate = 16000;
constexpr uint32_t kSpeakerSampleRate = 24000;
constexpr size_t kReadChunkSamples = 512;

i2s_chan_handle_t s_mic_rx = nullptr;
i2s_chan_handle_t s_speaker_tx = nullptr;
bool s_mic_enabled = false;
bool s_speaker_enabled = false;
std::mutex s_state_mutex;
std::mutex s_reference_mutex;
SpeakerReferenceCallback s_reference_callback = nullptr;
void* s_reference_context = nullptr;

void DeleteChannels() {
    if (s_mic_rx != nullptr) {
        i2s_del_channel(s_mic_rx);
        s_mic_rx = nullptr;
    }
    if (s_speaker_tx != nullptr) {
        i2s_del_channel(s_speaker_tx);
        s_speaker_tx = nullptr;
    }
    s_mic_enabled = false;
    s_speaker_enabled = false;
}

esp_err_t ConfigureSpeaker() {
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = 360;

    esp_err_t err = i2s_new_channel(&channel_config, &s_speaker_tx, nullptr);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSpeakerSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = kSpeakerBclk,
            .ws = kSpeakerWs,
            .dout = kSpeakerData,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    return i2s_channel_init_std_mode(s_speaker_tx, &standard_config);
}

esp_err_t ConfigureMicrophone() {
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = 240;

    esp_err_t err = i2s_new_channel(&channel_config, nullptr, &s_mic_rx);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kMicSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = kMicBclk,
            .ws = kMicWs,
            .dout = I2S_GPIO_UNUSED,
            .din = kMicData,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    standard_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    return i2s_channel_init_std_mode(s_mic_rx, &standard_config);
}

}  // namespace

esp_err_t Initialize() {
    std::lock_guard<std::mutex> lock(s_state_mutex);
    if (s_mic_rx != nullptr || s_speaker_tx != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = gpio_reset_pin(kSpeakerAmpEnable);
    if (err == ESP_OK) {
        err = gpio_set_direction(kSpeakerAmpEnable, GPIO_MODE_OUTPUT);
    }
    if (err == ESP_OK) {
        err = gpio_set_level(kSpeakerAmpEnable, 1);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Speaker amplifier GPIO setup failed: %s", esp_err_to_name(err));
        return err;
    }

    err = ConfigureSpeaker();
    if (err == ESP_OK) {
        err = ConfigureMicrophone();
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "I2S channel setup failed: %s", esp_err_to_name(err));
        DeleteChannels();
        gpio_set_level(kSpeakerAmpEnable, 0);
        return err;
    }

    ESP_LOGW(kTag, "I2S ready; microphone GPIO 5/4/6 remains a provisional, unverified mapping");
    ESP_LOGI(kTag, "Speaker: I2S0, 24 kHz PCM16; microphone: I2S1, 16 kHz 32-bit input");
    return ESP_OK;
}

esp_err_t EnableMicrophone(bool enable) {
    std::lock_guard<std::mutex> lock(s_state_mutex);
    if (s_mic_rx == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable == s_mic_enabled) {
        return ESP_OK;
    }

    const esp_err_t err = enable ? i2s_channel_enable(s_mic_rx)
                                : i2s_channel_disable(s_mic_rx);
    if (err == ESP_OK) {
        s_mic_enabled = enable;
    }
    return err;
}

esp_err_t EnableSpeaker(bool enable) {
    std::lock_guard<std::mutex> lock(s_state_mutex);
    if (s_speaker_tx == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable == s_speaker_enabled) {
        return ESP_OK;
    }

    esp_err_t err = enable ? i2s_channel_enable(s_speaker_tx)
                           : i2s_channel_disable(s_speaker_tx);
    if (err == ESP_OK) {
        s_speaker_enabled = enable;
    }
    return err;
}

esp_err_t ReadMicrophonePcm16(int16_t* destination, size_t requested_samples,
                              size_t* samples_read, uint32_t timeout_ms) {
    if (destination == nullptr || samples_read == nullptr || requested_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *samples_read = 0;

    if (s_mic_rx == nullptr || !s_mic_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    std::array<int32_t, kReadChunkSamples> raw_samples{};
    while (*samples_read < requested_samples) {
        const size_t chunk = std::min(kReadChunkSamples, requested_samples - *samples_read);
        size_t bytes_read = 0;
        const esp_err_t err = i2s_channel_read(
            s_mic_rx, raw_samples.data(), chunk * sizeof(int32_t),
            &bytes_read, timeout_ms);

        if (err != ESP_OK) {
            return (*samples_read > 0 && err == ESP_ERR_TIMEOUT) ? ESP_OK : err;
        }

        const size_t received = bytes_read / sizeof(int32_t);
        for (size_t i = 0; i < received; ++i) {
            // Match the legacy board's 32-bit I2S microphone to PCM16 conversion.
            const int32_t value = raw_samples[i] >> 12;
            destination[*samples_read + i] = static_cast<int16_t>(
                std::clamp<int32_t>(value, INT16_MIN, INT16_MAX));
        }
        *samples_read += received;

        if (received == 0) {
            break;
        }
    }

    return *samples_read > 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t WriteSpeakerPcm16(const int16_t* samples, size_t sample_count,
                            size_t* samples_written, uint32_t timeout_ms) {
    if (samples == nullptr || samples_written == nullptr || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *samples_written = 0;

    if (s_speaker_tx == nullptr || !s_speaker_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    std::array<int32_t, kReadChunkSamples> tx_samples{};
    while (*samples_written < sample_count) {
        const size_t chunk = std::min(kReadChunkSamples, sample_count - *samples_written);
        for (size_t i = 0; i < chunk; ++i) {
            tx_samples[i] = static_cast<int32_t>(
                static_cast<int64_t>(samples[*samples_written + i]) * 65536);
        }

        size_t bytes_written = 0;
        const esp_err_t err = i2s_channel_write(
            s_speaker_tx, tx_samples.data(), chunk * sizeof(int32_t),
            &bytes_written, timeout_ms);
        const size_t written = bytes_written / sizeof(int32_t);
        if (written > 0) {
            SpeakerReferenceCallback callback = nullptr;
            void* context = nullptr;
            {
                std::lock_guard<std::mutex> lock(s_reference_mutex);
                callback = s_reference_callback;
                context = s_reference_context;
            }
            if (callback != nullptr) {
                callback(samples + *samples_written, written, context);
            }
            *samples_written += written;
        }
        if (err != ESP_OK) {
            return err;
        }
        if (written == 0) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

void SetSpeakerReferenceCallback(SpeakerReferenceCallback callback, void* context) {
    std::lock_guard<std::mutex> lock(s_reference_mutex);
    s_reference_callback = callback;
    s_reference_context = context;
}

}  // namespace chatai::audio
