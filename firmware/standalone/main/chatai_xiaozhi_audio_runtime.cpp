#include "chatai_xiaozhi_audio_runtime.h"

#include "audio_manager.h"

#include <array>
#include <chrono>
#include <utility>

#include <esp_log.h>
#include <esp_timer.h>

namespace chatai::audio {
namespace {

constexpr char kTag[] = "XiaozhiAudio";
constexpr uint32_t kIoTimeoutMs = 100;
constexpr uint32_t kSpeakerSampleRate = 24000;
constexpr uint32_t kFrameDurationMs = 60;

}  // namespace

XiaozhiAudioRuntime::~XiaozhiAudioRuntime() {
    Stop();
    if (task_events_ != nullptr) {
        vEventGroupDelete(task_events_);
        task_events_ = nullptr;
    }
}

esp_err_t XiaozhiAudioRuntime::Initialize() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (initialized_.load()) {
        return ESP_OK;
    }

    task_events_ = xEventGroupCreate();
    if (task_events_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t result = opus_.Initialize();
    if (result != ESP_OK) {
        vEventGroupDelete(task_events_);
        task_events_ = nullptr;
        return result;
    }

    initialized_.store(true);
    return ESP_OK;
}

esp_err_t XiaozhiAudioRuntime::Start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!initialized_.load() || task_events_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (running_.load()) {
        return ESP_OK;
    }

    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        uplink_queue_.clear();
        downlink_queue_.clear();
    }
    xEventGroupClearBits(task_events_, kCaptureExited | kPlaybackExited);

    esp_err_t result = EnableSpeaker(true);
    if (result != ESP_OK) {
        return result;
    }

    // Keep the microphone peripheral disabled until listen-start.
    // This prevents stale DMA samples accumulating while idle or during TTS.
    capture_enabled_.store(false);
    session_start_us_ = esp_timer_get_time();
    running_.store(true);

    if (xTaskCreate(CaptureTaskEntry, "xiaozhi_mic", 6144, this, 6, nullptr) != pdPASS) {
        running_.store(false);
        EnableMicrophone(false);
        EnableSpeaker(false);
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(PlaybackTaskEntry, "xiaozhi_play", 4096, this, 5, nullptr) != pdPASS) {
        running_.store(false);
        uplink_cv_.notify_all();
        downlink_cv_.notify_all();
        xEventGroupWaitBits(task_events_, kCaptureExited, pdTRUE, pdTRUE, portMAX_DELAY);
        EnableMicrophone(false);
        EnableSpeaker(false);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag, "Audio runtime started: mono 16 kHz uplink / 24 kHz downlink");
    return ESP_OK;
}

esp_err_t XiaozhiAudioRuntime::Stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!initialized_.load()) {
        return ESP_OK;
    }

    capture_enabled_.store(false);
    const bool was_running = running_.exchange(false);
    if (was_running) {
        uplink_cv_.notify_all();
        downlink_cv_.notify_all();
        xEventGroupWaitBits(task_events_, kCaptureExited | kPlaybackExited,
                            pdTRUE, pdTRUE, portMAX_DELAY);
    }

    esp_err_t mic_result = ESP_OK;
    esp_err_t speaker_result = ESP_OK;
    // Stop may be called after a failed transport/hello before this runtime
    // ever owned I2S. Do not disable peripherals still owned by Gemini then.
    if (was_running) {
        mic_result = EnableMicrophone(false);
        speaker_result = EnableSpeaker(false);
    }
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        uplink_queue_.clear();
        downlink_queue_.clear();
    }

    session_start_us_ = 0;
    const esp_err_t codec_result = opus_.Reset();
    if (mic_result != ESP_OK && mic_result != ESP_ERR_INVALID_STATE) {
        return mic_result;
    }
    if (speaker_result != ESP_OK && speaker_result != ESP_ERR_INVALID_STATE) {
        return speaker_result;
    }
    if (codec_result != ESP_OK && codec_result != ESP_ERR_INVALID_STATE) {
        return codec_result;
    }

    ESP_LOGI(kTag, "Audio runtime stopped");
    return ESP_OK;
}

void XiaozhiAudioRuntime::SetCaptureEnabled(bool enabled) {
    if (!running_.load()) {
        capture_enabled_.store(false);
        return;
    }
    if (!enabled) {
        capture_enabled_.store(false);
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            uplink_queue_.clear();
        }
        const esp_err_t result = EnableMicrophone(false);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(kTag, "Could not pause microphone: %s", esp_err_to_name(result));
        }
    } else {
        const esp_err_t result = EnableMicrophone(true);
        if (result != ESP_OK) {
            capture_enabled_.store(false);
            ESP_LOGW(kTag, "Could not enable microphone: %s", esp_err_to_name(result));
            return;
        }
        capture_enabled_.store(true);
    }
    uplink_cv_.notify_all();
}

bool XiaozhiAudioRuntime::WaitPopUplinkFrame(
    XiaozhiUplinkFrame& frame, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    const bool ready = uplink_cv_.wait_for(
        lock, std::chrono::milliseconds(timeout_ms),
        [this]() { return !uplink_queue_.empty() || !running_.load(); });
    if (!ready || uplink_queue_.empty()) {
        return false;
    }

    frame = std::move(uplink_queue_.front());
    uplink_queue_.pop_front();
    return true;
}

esp_err_t XiaozhiAudioRuntime::SubmitDownlinkFrame(
    const uint8_t* payload, size_t payload_size, uint32_t sample_rate,
    uint32_t frame_duration_ms) {
    if (!running_.load()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload == nullptr || payload_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_rate != kSpeakerSampleRate ||
        frame_duration_ms != kFrameDurationMs) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    DownlinkFrame frame;
    frame.sample_rate = sample_rate;
    frame.frame_duration_ms = frame_duration_ms;
    frame.payload.assign(payload, payload + payload_size);

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!running_.load()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (downlink_queue_.size() >= kQueueDepth) {
            downlink_queue_.pop_front();
        }
        downlink_queue_.push_back(std::move(frame));
    }
    downlink_cv_.notify_one();
    return ESP_OK;
}

void XiaozhiAudioRuntime::CaptureTaskEntry(void* context) {
    auto* runtime = static_cast<XiaozhiAudioRuntime*>(context);
    runtime->CaptureTask();
    xEventGroupSetBits(runtime->task_events_, kCaptureExited);
    vTaskDelete(nullptr);
}

void XiaozhiAudioRuntime::PlaybackTaskEntry(void* context) {
    auto* runtime = static_cast<XiaozhiAudioRuntime*>(context);
    runtime->PlaybackTask();
    xEventGroupSetBits(runtime->task_events_, kPlaybackExited);
    vTaskDelete(nullptr);
}

void XiaozhiAudioRuntime::CaptureTask() {
    std::array<int16_t, ChatAIOpusCodec::kInputFrameSamples> pcm{};
    while (running_.load()) {
        if (!capture_enabled_.load()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t collected = 0;
        while (collected < pcm.size() && running_.load()) {
            size_t received = 0;
            const esp_err_t result = ReadMicrophonePcm16(
                pcm.data() + collected, pcm.size() - collected,
                &received, kIoTimeoutMs);
            if (received > 0) {
                collected += received;
            }
            if (result != ESP_OK && received == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                break;
            }
        }

        if (!running_.load()) {
            break;
        }
        if (!capture_enabled_.load() || collected != pcm.size()) {
            continue;
        }

        XiaozhiUplinkFrame frame;
        frame.timestamp_ms = static_cast<uint32_t>(
            (esp_timer_get_time() - session_start_us_) / 1000);
        const esp_err_t result =
            opus_.EncodeFrame(pcm.data(), pcm.size(), frame.payload);
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "Opus encode failed: %s", esp_err_to_name(result));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!running_.load()) {
                break;
            }
            if (uplink_queue_.size() >= kQueueDepth) {
                uplink_queue_.pop_front();
            }
            uplink_queue_.push_back(std::move(frame));
        }
        uplink_cv_.notify_one();
    }
}

void XiaozhiAudioRuntime::PlaybackTask() {
    while (running_.load()) {
        DownlinkFrame frame;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            downlink_cv_.wait(lock, [this]() {
                return !downlink_queue_.empty() || !running_.load();
            });
            if (!running_.load()) {
                break;
            }
            frame = std::move(downlink_queue_.front());
            downlink_queue_.pop_front();
        }

        std::vector<int16_t> pcm;
        const esp_err_t decode_result = opus_.DecodeFrame(
            frame.payload.data(), frame.payload.size(), frame.sample_rate,
            frame.frame_duration_ms, pcm);
        if (decode_result != ESP_OK) {
            ESP_LOGW(kTag, "Opus decode failed: %s", esp_err_to_name(decode_result));
            continue;
        }

        size_t written = 0;
        const esp_err_t write_result = WriteSpeakerPcm16(
            pcm.data(), pcm.size(), &written, kIoTimeoutMs);
        if (write_result != ESP_OK || written != pcm.size()) {
            ESP_LOGW(kTag, "Speaker write incomplete: %s (%u/%u)",
                     esp_err_to_name(write_result),
                     static_cast<unsigned>(written),
                     static_cast<unsigned>(pcm.size()));
        }
    }
}

}  // namespace chatai::audio
