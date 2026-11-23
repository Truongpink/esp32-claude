#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "chatai_xiaozhi_audio_runtime.h"
namespace chatai::xiaozhi {
struct NotifySubtitle { uint32_t start_ms = 0; std::string text; };
class NotifyPlayer final {
public:
    using SubtitleCallback = std::function<void(uint32_t, const std::string&)>;
    using FinishedCallback = std::function<void(uint32_t, bool)>;
    explicit NotifyPlayer(chatai::audio::XiaozhiAudioRuntime& audio);
    ~NotifyPlayer();
    NotifyPlayer(const NotifyPlayer&) = delete;
    NotifyPlayer& operator=(const NotifyPlayer&) = delete;
    bool Start(std::string audio_url, std::vector<NotifySubtitle> subtitles,
               uint32_t playback_id, SubtitleCallback subtitle_callback,
               FinishedCallback finished_callback);
    void Stop();
    bool IsBusy() const;
    void OnPlaybackProgress(uint32_t playback_id, uint32_t media_position_ms);
    void OnPlaybackDrained();
private:
    chatai::audio::XiaozhiAudioRuntime& audio_;
    mutable std::mutex mutex_;
    std::string audio_url_;
    std::vector<NotifySubtitle> subtitles_;
    std::string displayed_text_;
    SubtitleCallback subtitle_callback_;
    FinishedCallback finished_callback_;
    TaskHandle_t task_handle_ = nullptr;
    uint32_t playback_id_ = 0, last_playback_position_ms_ = 0, underrun_count_ = 0;
    size_t next_subtitle_index_ = 0;
    bool active_ = false, worker_running_ = false, cancelled_ = false;
    bool http_finished_ = false, stream_started_ = false, playback_drained_ = false;
    bool completion_reported_ = false;
    static void WorkerEntry(void* arg);
    void WorkerTask();
    bool IsCancelled(uint32_t playback_id) const;
    FinishedCallback CompleteLocked(uint32_t& playback_id);
};
}  // namespace chatai::xiaozhi
