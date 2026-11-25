#include "chatai_xiaozhi_audio_runtime.h"

#include <chrono>
#include <memory>
#include <utility>

#include <esp_log.h>

#include "diagnostic_manager.h"

namespace chatai::audio {
namespace {

constexpr char kTag[] = "XiaozhiAudioRuntime";

bool SupportedRate(uint32_t rate) {
    return rate == 8000 || rate == 12000 || rate == 16000 ||
           rate == 24000 || rate == 48000;
}

bool SupportedDuration(uint32_t duration) {
    return duration == 5 || duration == 10 || duration == 20 ||
           duration == 40 || duration == 60 || duration == 80 ||
           duration == 100 || duration == 120;
}

}  // namespace

XiaozhiAudioRuntime::~XiaozhiAudioRuntime() {
    Stop();
    codec_.Stop();
}

esp_err_t XiaozhiAudioRuntime::Initialize() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (initialized_) {
        return ESP_OK;
    }

    AudioServiceCallbacks callbacks;
    callbacks.on_playback_started = [this]() {
        PlaybackStartedCallback callback;
        { std::lock_guard<std::mutex> lock(playback_callback_mutex_); callback = playback_started_callback_; }
        if (callback) callback();
    };
    callbacks.on_vad_change = [this](bool speaking) {
        VadCallback callback;
        { std::lock_guard<std::mutex> lock(playback_callback_mutex_); callback = vad_callback_; }
        if (callback) callback(speaking);
    };
    callbacks.on_playback_progress = [this](uint32_t id, uint32_t position) {
        PlaybackProgressCallback callback;
        { std::lock_guard<std::mutex> lock(playback_callback_mutex_); callback = playback_progress_callback_; }
        if (callback) callback(id, position);
    };
    callbacks.on_playback_drained = [this]() {
        PlaybackDrainedCallback callback;
        { std::lock_guard<std::mutex> lock(playback_callback_mutex_); callback = playback_drained_callback_; }
        if (callback) callback();
    };
    callbacks.on_send_queue_available = [this]() {
        {
            std::lock_guard<std::mutex> queue_lock(send_mutex_);
            send_queue_notified_ = true;
        }
        send_cv_.notify_one();
    };
    service_.SetCallbacks(callbacks);

    const esp_err_t result = service_.Initialize(&codec_);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "AudioService initialization failed: %s",
                 esp_err_to_name(result));
        codec_.Stop();
        return result;
    }

    const esp_err_t codec_error = codec_.last_error();
    if (codec_error != ESP_OK) {
        codec_.Stop();
        return codec_error;
    }

    initialized_ = true;
    return ESP_OK;
}

esp_err_t XiaozhiAudioRuntime::Start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (running_) {
        return ESP_OK;
    }

    {
        std::lock_guard<std::mutex> queue_lock(send_mutex_);
        send_queue_notified_ = false;
    }

    // AudioService::Stop() is paired with codec_.Stop() below. Re-arm the
    // ChatAI I2S bridge before restarting the service for a new Xiaozhi session.
    codec_.Start();
    const esp_err_t codec_error = codec_.last_error();
    if (codec_error != ESP_OK) {
        ESP_LOGE(kTag, "Audio codec restart failed: %s",
                 esp_err_to_name(codec_error));
        chatai::diagnostics::Add("AUDIO_RUNTIME", "CODEC_RESTART_FAILED",
                                 esp_err_to_name(codec_error));
        return codec_error;
    }

    const esp_err_t result = service_.Start();
    if (result == ESP_OK) {
        capture_enabled_.store(false);
        running_ = true;
        chatai::diagnostics::Add("AUDIO_RUNTIME", "STARTED",
                                 "audio codec and AudioService running");
    } else {
        codec_.Stop();
        chatai::diagnostics::Add("AUDIO_RUNTIME", "SERVICE_START_FAILED",
                                 esp_err_to_name(result));
    }
    return result;
}

esp_err_t XiaozhiAudioRuntime::Stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!running_.exchange(false)) {
        send_cv_.notify_all();
        return codec_.Stop();
    }

    capture_enabled_.store(false);
    service_.EnableVoiceProcessing(false);
    service_.RequestInputStop();
    service_.Stop();

    const esp_err_t codec_result = codec_.Stop();
    send_cv_.notify_all();
    return codec_result;
}

void XiaozhiAudioRuntime::SetCaptureEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!running_ || enabled == capture_enabled_.load()) {
        return;
    }

    if (enabled) {
        capture_enabled_.store(true);
        if (!service_.EnableVoiceProcessing(true)) {
            capture_enabled_.store(false);
            ESP_LOGE(kTag, "AFE initialization failed");
            chatai::diagnostics::Add("AFE_CAPTURE", "START_FAILED",
                "AudioService EnableVoiceProcessing(true) returned false");
            return;
        }
        chatai::diagnostics::Add("AFE_CAPTURE", "STARTED",
            "microphone capture and voice processing enabled");
    } else {
        capture_enabled_.store(false);
        service_.EnableVoiceProcessing(false);
        service_.RequestInputStop();
        chatai::diagnostics::Add("AFE_CAPTURE", "STOPPED",
            "microphone capture disabled");
    }
}

bool XiaozhiAudioRuntime::WaitPopUplinkFrame(XiaozhiUplinkFrame& frame,
                                             uint32_t timeout_ms) {
    if (!running_) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (running_) {
        auto packet = service_.PopPacketFromSendQueue();
        if (packet != nullptr) {
            frame.sample_rate = packet->sample_rate;
            frame.frame_duration_ms = packet->frame_duration;
            frame.timestamp_ms = packet->timestamp;
            frame.payload = std::move(packet->payload);
            return true;
        }

        if (timeout_ms == 0) {
            return false;
        }

        std::unique_lock<std::mutex> lock(send_mutex_);
        if (!send_cv_.wait_until(lock, deadline, [this]() {
                return send_queue_notified_ || !running_;
            })) {
            return false;
        }
        send_queue_notified_ = false;
    }

    return false;
}

esp_err_t XiaozhiAudioRuntime::SubmitDownlinkFrame(const uint8_t* payload,
                                                   size_t payload_size,
                                                   uint32_t sample_rate,
                                                   uint32_t frame_duration_ms) {
    if (!running_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload == nullptr || payload_size == 0 ||
        payload_size > kMaxOpusPacketBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!SupportedRate(sample_rate) || !SupportedDuration(frame_duration_ms)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = sample_rate;
    packet->frame_duration = frame_duration_ms;
    packet->payload.assign(payload, payload + payload_size);
    return service_.PushPacketToDecodeQueue(std::move(packet), false)
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

void XiaozhiAudioRuntime::SetPlaybackCallbacks(PlaybackProgressCallback progress,
                                               PlaybackDrainedCallback drained) {
    std::lock_guard<std::mutex> lock(playback_callback_mutex_);
    playback_progress_callback_ = std::move(progress);
    playback_drained_callback_ = std::move(drained);
}

void XiaozhiAudioRuntime::SetPlaybackStartedCallback(PlaybackStartedCallback started) {
    std::lock_guard<std::mutex> lock(playback_callback_mutex_);
    playback_started_callback_ = std::move(started);
}

void XiaozhiAudioRuntime::SetVadCallback(VadCallback callback) {
    std::lock_guard<std::mutex> lock(playback_callback_mutex_);
    vad_callback_ = std::move(callback);
}

esp_err_t XiaozhiAudioRuntime::SubmitNotificationFrame(
    const uint8_t* payload, size_t payload_size, uint32_t sample_rate,
    uint32_t frame_duration_ms, uint32_t playback_id, uint32_t media_position_ms) {
    if (!running_) return ESP_ERR_INVALID_STATE;
    if (payload == nullptr || payload_size == 0 || payload_size > kMaxOpusPacketBytes ||
        playback_id == 0 || !SupportedRate(sample_rate) ||
        !SupportedDuration(frame_duration_ms)) return ESP_ERR_INVALID_ARG;
    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = static_cast<int>(sample_rate);
    packet->frame_duration = static_cast<int>(frame_duration_ms);
    packet->playback_id = playback_id;
    packet->media_position_ms = media_position_ms;
    packet->payload.assign(payload, payload + payload_size);
    return service_.PushPacketToDecodeQueue(std::move(packet), true)
        ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void XiaozhiAudioRuntime::ResetNotificationPlayback() {
    if (running_) service_.ResetDecoder();
}

}  // namespace chatai::audio
