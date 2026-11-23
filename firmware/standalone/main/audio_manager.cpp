#include "audio_manager.h"
#include "diagnostic_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <mutex>

#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>

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
std::atomic<bool> s_mic_enabled{false};
std::atomic<bool> s_speaker_enabled{false};
std::atomic<int> s_output_volume{100};
std::atomic<int> s_input_gain_db{0};
std::atomic<bool> s_mic_raw_reported{false};
uint64_t s_mic_level_sum_squares = 0;
uint32_t s_mic_level_samples = 0;
uint32_t s_mic_level_clipped_samples = 0;
int32_t s_mic_level_peak = 0;
int64_t s_mic_level_window_started_us = 0;
std::mutex s_state_mutex;
std::timed_mutex s_mic_io_mutex;
std::mutex s_speaker_io_mutex;
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

    nvs_handle_t settings_handle;
    if (nvs_open("chatai_cfg", NVS_READONLY, &settings_handle) == ESP_OK) {
        int32_t saved_volume = 100;
        int32_t saved_mic_gain_db = 0;
        if (nvs_get_i32(settings_handle, "speaker_vol", &saved_volume) == ESP_OK) {
            s_output_volume.store(std::clamp<int32_t>(saved_volume, 0, 100));
        }
        if (nvs_get_i32(settings_handle, "mic_gain_db", &saved_mic_gain_db) == ESP_OK) {
            s_input_gain_db.store(std::clamp<int32_t>(saved_mic_gain_db, -6, 6));
        }
        nvs_close(settings_handle);
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
        chatai::diagnostics::Add("AMP_ENABLE", "FAIL", esp_err_to_name(err));
        return err;
    }
    chatai::diagnostics::Add("AMP_ENABLE", "OK", "speaker amplifier GPIO17 set HIGH");

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
    char volume_detail[48]{};
    std::snprintf(volume_detail, sizeof(volume_detail), "saved_output_volume=%d%%",
                  s_output_volume.load());
    chatai::diagnostics::Add("SPEAKER_VOLUME", "INFO", volume_detail);
    return ESP_OK;
}

esp_err_t EnableMicrophone(bool enable) {
    std::lock_guard<std::mutex> state_lock(s_state_mutex);
    std::lock_guard<std::timed_mutex> io_lock(s_mic_io_mutex);
    if (s_mic_rx == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable == s_mic_enabled.load()) {
        return ESP_OK;
    }

    const esp_err_t err = enable ? i2s_channel_enable(s_mic_rx)
                                : i2s_channel_disable(s_mic_rx);
    if (err == ESP_OK) {
        s_mic_enabled = enable;
    }
    return err;
}

bool IsMicrophoneEnabled() {
    return s_mic_enabled.load();
}

bool IsSpeakerEnabled() {
    return s_speaker_enabled.load();
}

esp_err_t EnableSpeaker(bool enable) {
    std::lock_guard<std::mutex> state_lock(s_state_mutex);
    std::lock_guard<std::mutex> io_lock(s_speaker_io_mutex);
    if (s_speaker_tx == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable == s_speaker_enabled.load()) {
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
    const int64_t call_started_us = esp_timer_get_time();
    uint64_t mutex_wait_us = 0;
    uint64_t driver_read_total_us = 0;
    uint64_t driver_read_max_us = 0;
    uint64_t conversion_total_us = 0;
    uint64_t conversion_max_us = 0;
    uint32_t driver_read_calls = 0;
    uint32_t samples_total = 0;
    uint32_t partial_reads = 0;
    uint32_t timeout_count = 0;
    std::unique_lock<std::timed_mutex> io_lock(s_mic_io_mutex, std::defer_lock);

    const auto finish = [&](esp_err_t result) {
        if (io_lock.owns_lock()) io_lock.unlock();
        const uint64_t call_elapsed_us = static_cast<uint64_t>(
            std::max<int64_t>(0, esp_timer_get_time() - call_started_us));
        static int64_t window_started_us = esp_timer_get_time();
        static uint64_t call_total_us = 0, call_max_us = 0;
        static uint64_t mutex_total_us = 0, mutex_max_us = 0;
        static uint64_t driver_total_us = 0, driver_max_us = 0;
        static uint64_t conversion_sum_us = 0, conversion_peak_us = 0;
        static uint32_t calls = 0, reads = 0, samples = 0, partials = 0, timeouts = 0;
        call_total_us += call_elapsed_us;
        call_max_us = std::max(call_max_us, call_elapsed_us);
        mutex_total_us += mutex_wait_us;
        mutex_max_us = std::max(mutex_max_us, mutex_wait_us);
        driver_total_us += driver_read_total_us;
        driver_max_us = std::max(driver_max_us, driver_read_max_us);
        conversion_sum_us += conversion_total_us;
        conversion_peak_us = std::max(conversion_peak_us, conversion_max_us);
        calls++;
        reads += driver_read_calls;
        samples += samples_total;
        partials += partial_reads;
        timeouts += timeout_count;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - window_started_us >= 5000000) {
            char detail[320]{};
            std::snprintf(detail, sizeof(detail),
                "calls=%lu call_avg_us=%llu call_max_us=%llu "
                "mutex_avg_us=%llu mutex_max_us=%llu i2s_calls=%lu "
                "i2s_avg_us=%llu i2s_max_us=%llu samples_per_read=%lu "
                "convert_avg_us=%llu convert_max_us=%llu partial=%lu timeout=%lu",
                static_cast<unsigned long>(calls),
                static_cast<unsigned long long>(calls ? call_total_us / calls : 0),
                static_cast<unsigned long long>(call_max_us),
                static_cast<unsigned long long>(calls ? mutex_total_us / calls : 0),
                static_cast<unsigned long long>(mutex_max_us),
                static_cast<unsigned long>(reads),
                static_cast<unsigned long long>(reads ? driver_total_us / reads : 0),
                static_cast<unsigned long long>(driver_max_us),
                static_cast<unsigned long>(reads ? samples / reads : 0),
                static_cast<unsigned long long>(reads ? conversion_sum_us / reads : 0),
                static_cast<unsigned long long>(conversion_peak_us),
                static_cast<unsigned long>(partials),
                static_cast<unsigned long>(timeouts));
            chatai::diagnostics::Add("MIC_IO", "INFO", detail);
            window_started_us = now_us;
            call_total_us = call_max_us = mutex_total_us = mutex_max_us = 0;
            driver_total_us = driver_max_us = conversion_sum_us = conversion_peak_us = 0;
            calls = reads = samples = partials = timeouts = 0;
        }
        return result;
    };

    if (destination == nullptr || samples_read == nullptr || requested_samples == 0) {
        return finish(ESP_ERR_INVALID_ARG);
    }
    *samples_read = 0;
    const int64_t mutex_started_us = esp_timer_get_time();
    if (!io_lock.try_lock_for(std::chrono::milliseconds(timeout_ms))) {
        mutex_wait_us = static_cast<uint64_t>(
            std::max<int64_t>(0, esp_timer_get_time() - mutex_started_us));
        ++timeout_count;
        return finish(ESP_ERR_TIMEOUT);
    }
    mutex_wait_us = static_cast<uint64_t>(
        std::max<int64_t>(0, esp_timer_get_time() - mutex_started_us));

    if (s_mic_rx == nullptr || !s_mic_enabled.load()) {
        return finish(ESP_ERR_INVALID_STATE);
    }

    std::array<int32_t, kReadChunkSamples> raw_samples{};
    const int input_gain_db = s_input_gain_db.load();
    const double input_gain = std::pow(10.0, input_gain_db / 20.0);
    while (*samples_read < requested_samples) {
        const size_t chunk = std::min(kReadChunkSamples, requested_samples - *samples_read);
        size_t bytes_read = 0;
        const int64_t driver_started_us = esp_timer_get_time();
        const esp_err_t err = i2s_channel_read(
            s_mic_rx, raw_samples.data(), chunk * sizeof(int32_t),
            &bytes_read, timeout_ms);
        const uint64_t driver_elapsed_us = static_cast<uint64_t>(
            std::max<int64_t>(0, esp_timer_get_time() - driver_started_us));
        driver_read_total_us += driver_elapsed_us;
        driver_read_max_us = std::max(driver_read_max_us, driver_elapsed_us);
        ++driver_read_calls;

        if (err != ESP_OK) {
            if (err == ESP_ERR_TIMEOUT) ++timeout_count;
            return finish((*samples_read > 0 && err == ESP_ERR_TIMEOUT) ? ESP_OK : err);
        }

        if (bytes_read % sizeof(int32_t) != 0 ||
            bytes_read > chunk * sizeof(int32_t)) {
            return finish(ESP_ERR_INVALID_SIZE);
        }
        const size_t received = bytes_read / sizeof(int32_t);
        samples_total += static_cast<uint32_t>(received);
        if (received < chunk) ++partial_reads;
        if (received > 0 && !s_mic_raw_reported.exchange(true)) {
            int32_t raw_min = INT32_MAX;
            int32_t raw_max = INT32_MIN;
            for (size_t i = 0; i < received; ++i) {
                raw_min = std::min(raw_min, raw_samples[i]);
                raw_max = std::max(raw_max, raw_samples[i]);
            }
            char raw_detail[160]{};
            std::snprintf(raw_detail, sizeof(raw_detail),
                          "samples=%lu raw_min=%ld raw_max=%ld first=%08lx/%08lx/%08lx/%08lx",
                          static_cast<unsigned long>(received),
                          static_cast<long>(raw_min), static_cast<long>(raw_max),
                          static_cast<unsigned long>(static_cast<uint32_t>(raw_samples[0])),
                          static_cast<unsigned long>(static_cast<uint32_t>(raw_samples[std::min<size_t>(1, received - 1)])),
                          static_cast<unsigned long>(static_cast<uint32_t>(raw_samples[std::min<size_t>(2, received - 1)])),
                          static_cast<unsigned long>(static_cast<uint32_t>(raw_samples[std::min<size_t>(3, received - 1)])));
            chatai::diagnostics::Add("MIC_RAW", "INFO", raw_detail);
        }
        const int64_t conversion_started_us = esp_timer_get_time();
        for (size_t i = 0; i < received; ++i) {
            // Preserve the current upstream-compatible I2S-to-PCM16 conversion.
            const int32_t value = raw_samples[i] >> 12;
            const int32_t gained_value = static_cast<int32_t>(std::lround(value * input_gain));
            if (gained_value > INT16_MAX || gained_value < INT16_MIN) {
                ++s_mic_level_clipped_samples;
            }
            const int16_t pcm = static_cast<int16_t>(
                std::clamp<int32_t>(gained_value, INT16_MIN, INT16_MAX));
            destination[*samples_read + i] = pcm;
            const int32_t magnitude = pcm < 0 ? -static_cast<int32_t>(pcm) : pcm;
            s_mic_level_peak = std::max(s_mic_level_peak, magnitude);
            s_mic_level_sum_squares +=
                static_cast<uint64_t>(static_cast<int64_t>(pcm) * pcm);
            ++s_mic_level_samples;
        }
        const int64_t level_now_us = esp_timer_get_time();
        if (s_mic_level_window_started_us == 0) {
            s_mic_level_window_started_us = level_now_us;
        }
        if (level_now_us - s_mic_level_window_started_us >= 2000000 &&
            s_mic_level_samples > 0) {
            const double rms = std::sqrt(
                static_cast<double>(s_mic_level_sum_squares) / s_mic_level_samples);
            const double rms_dbfs = 20.0 * std::log10(std::max(rms, 1.0) / 32768.0);
            const double peak_dbfs =
                20.0 * std::log10(std::max<int32_t>(s_mic_level_peak, 1) / 32768.0);
            char level_detail[160]{};
            std::snprintf(level_detail, sizeof(level_detail),
                "samples=%lu gain_db=%d rms=%.0f rms_dbfs=%.1f peak=%ld peak_dbfs=%.1f clipped=%lu",
                static_cast<unsigned long>(s_mic_level_samples), input_gain_db, rms, rms_dbfs,
                static_cast<long>(s_mic_level_peak), peak_dbfs,
                static_cast<unsigned long>(s_mic_level_clipped_samples));
            chatai::diagnostics::Add("MIC_LEVEL", "PCM", level_detail);
            s_mic_level_sum_squares = 0;
            s_mic_level_samples = 0;
            s_mic_level_clipped_samples = 0;
            s_mic_level_peak = 0;
            s_mic_level_window_started_us = level_now_us;
        }
        const uint64_t conversion_elapsed_us = static_cast<uint64_t>(
            std::max<int64_t>(0, esp_timer_get_time() - conversion_started_us));
        conversion_total_us += conversion_elapsed_us;
        conversion_max_us = std::max(conversion_max_us, conversion_elapsed_us);
        *samples_read += received;

        if (received == 0) {
            break;
        }
    }

    return finish(*samples_read > 0 ? ESP_OK : ESP_ERR_TIMEOUT);
}

esp_err_t WriteSpeakerPcm16(const int16_t* samples, size_t sample_count,
                            size_t* samples_written, uint32_t timeout_ms) {
    if (samples == nullptr || samples_written == nullptr || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *samples_written = 0;
    std::lock_guard<std::mutex> io_lock(s_speaker_io_mutex);

    if (s_speaker_tx == nullptr || !s_speaker_enabled.load()) {
        return ESP_ERR_INVALID_STATE;
    }

    std::array<int32_t, kReadChunkSamples> tx_samples{};
    while (*samples_written < sample_count) {
        const size_t chunk = std::min(kReadChunkSamples, sample_count - *samples_written);
        for (size_t i = 0; i < chunk; ++i) {
            const int scaled = static_cast<int>(
                static_cast<int64_t>(samples[*samples_written + i]) * s_output_volume.load() / 100);
            tx_samples[i] = static_cast<int32_t>(static_cast<int64_t>(scaled) * 65536);
        }

        size_t bytes_written = 0;
        const esp_err_t err = i2s_channel_write(
            s_speaker_tx, tx_samples.data(), chunk * sizeof(int32_t),
            &bytes_written, timeout_ms);
        if (bytes_written % sizeof(int32_t) != 0 ||
            bytes_written > chunk * sizeof(int32_t)) {
            return ESP_ERR_INVALID_SIZE;
        }
        const size_t written = bytes_written / sizeof(int32_t);
        if (written > 0) {
            // Keep the callback context alive until invocation completes: a setter
            // cannot replace/unregister it while this lock is held. The callback
            // must not call SetSpeakerReferenceCallback() recursively.
            std::lock_guard<std::mutex> reference_lock(s_reference_mutex);
            if (s_reference_callback != nullptr) {
                s_reference_callback(samples + *samples_written, written,
                                     s_reference_context);
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

void SetOutputVolume(int volume_percent) {
    s_output_volume.store(std::clamp(volume_percent, 0, 100));
}

int GetOutputVolume() {
    return s_output_volume.load();
}

void SetInputGainDb(int gain_db) {
    s_input_gain_db.store(std::clamp(gain_db, -6, 6));
}

int GetInputGainDb() {
    return s_input_gain_db.load();
}

esp_err_t SaveInputGainDb(int gain_db) {
    if (gain_db < -6 || gain_db > 6) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t result = nvs_open("chatai_cfg", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_i32(handle, "mic_gain_db", gain_db);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result == ESP_OK) SetInputGainDb(gain_db);
    return result;
}

esp_err_t SaveOutputVolume(int volume_percent) {
    if (volume_percent < 0 || volume_percent > 100) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t result = nvs_open("chatai_cfg", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;

    result = nvs_set_i32(handle, "speaker_vol", volume_percent);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result == ESP_OK) SetOutputVolume(volume_percent);
    return result;
}

void SetSpeakerReferenceCallback(SpeakerReferenceCallback callback, void* context) {
    std::lock_guard<std::mutex> lock(s_reference_mutex);
    s_reference_callback = callback;
    s_reference_context = context;
}

}  // namespace chatai::audio
