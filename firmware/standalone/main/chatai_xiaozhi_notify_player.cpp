#include "chatai_xiaozhi_notify_player.h"
#include <algorithm>
#include <array>
#include <utility>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include "ogg_demuxer.h"
namespace chatai::xiaozhi {
namespace {
constexpr char kTag[] = "XiaozhiNotify";
constexpr int kHttpTimeoutMs = 5000;
constexpr size_t kReadBufferSize = 1024;
bool SupportedUrl(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}
}
NotifyPlayer::NotifyPlayer(chatai::audio::XiaozhiAudioRuntime& audio) : audio_(audio) {}
NotifyPlayer::~NotifyPlayer() { Stop(); }
bool NotifyPlayer::Start(std::string url, std::vector<NotifySubtitle> subtitles,
                         uint32_t id, SubtitleCallback subtitle_cb, FinishedCallback finished_cb) {
    if (!id || !SupportedUrl(url)) return false;
    std::stable_sort(subtitles.begin(), subtitles.end(),
        [](const auto& a, const auto& b) { return a.start_ms < b.start_ms; });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ || worker_running_) return false;
        audio_url_ = std::move(url); subtitles_ = std::move(subtitles);
        displayed_text_.clear(); subtitle_callback_ = std::move(subtitle_cb);
        finished_callback_ = std::move(finished_cb); playback_id_ = id;
        last_playback_position_ms_ = underrun_count_ = 0; next_subtitle_index_ = 0;
        active_ = worker_running_ = true; cancelled_ = http_finished_ = false;
        stream_started_ = playback_drained_ = completion_reported_ = false;
    }
    if (xTaskCreate(WorkerEntry, "xiaozhi_notify", 6144, this, 2, &task_handle_) != pdPASS) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = worker_running_ = false; cancelled_ = true; task_handle_ = nullptr;
        return false;
    }
    return true;
}
void NotifyPlayer::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true; active_ = false; subtitles_.clear();
    subtitle_callback_ = nullptr; finished_callback_ = nullptr;
}
bool NotifyPlayer::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_); return active_ || worker_running_;
}
bool NotifyPlayer::IsCancelled(uint32_t id) const {
    std::lock_guard<std::mutex> lock(mutex_); return cancelled_ || !active_ || id != playback_id_;
}
void NotifyPlayer::OnPlaybackProgress(uint32_t id, uint32_t position) {
    SubtitleCallback cb; std::string text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || id != playback_id_) return;
        last_playback_position_ms_ = position; bool changed = false;
        while (next_subtitle_index_ < subtitles_.size() &&
               subtitles_[next_subtitle_index_].start_ms <= position) {
            text = subtitles_[next_subtitle_index_++].text; changed = true;
        }
        if (!changed || text == displayed_text_) return;
        displayed_text_ = text; cb = subtitle_callback_;
    }
    if (cb) cb(id, text);
}
NotifyPlayer::FinishedCallback NotifyPlayer::CompleteLocked(uint32_t& id) {
    if (completion_reported_ || !active_) return nullptr;
    completion_reported_ = true; active_ = false; id = playback_id_; return finished_callback_;
}
void NotifyPlayer::OnPlaybackDrained() {
    FinishedCallback cb; uint32_t id = 0, underruns = 0, position = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) return;
        playback_drained_ = true;
        if (http_finished_) cb = CompleteLocked(id);
        else if (stream_started_) { underruns = ++underrun_count_; position = last_playback_position_ms_; }
    }
    if (underruns) ESP_LOGW(kTag, "Notify underrun #%lu at %lu ms",
        static_cast<unsigned long>(underruns), static_cast<unsigned long>(position));
    if (cb) cb(id, true);
}
void NotifyPlayer::WorkerEntry(void* arg) {
    static_cast<NotifyPlayer*>(arg)->WorkerTask(); vTaskDelete(nullptr);
}
void NotifyPlayer::WorkerTask() {
    std::string url; uint32_t id;
    { std::lock_guard<std::mutex> lock(mutex_); url = audio_url_; id = playback_id_; }
    bool success = false, packet_error = false, should_reset = false;
    esp_http_client_config_t cfg{};
    cfg.url = url.c_str(); cfg.timeout_ms = kHttpTimeoutMs; cfg.buffer_size = kReadBufferSize;
    cfg.crt_bundle_attach = esp_crt_bundle_attach; cfg.disable_auto_redirect = true;
    auto http = esp_http_client_init(&cfg);
    if (http) {
        esp_http_client_set_header(http, "Accept", "audio/ogg, application/ogg");
        esp_http_client_set_header(http, "Accept-Encoding", "identity");
        const esp_err_t opened = esp_http_client_open(http, 0);
        if (opened == ESP_OK) {
            (void)esp_http_client_fetch_headers(http);
            const int status = esp_http_client_get_status_code(http);
            if (status >= 200 && status < 300 && !IsCancelled(id)) {
                OggDemuxer demuxer; uint32_t media_ms = 0;
                demuxer.OnPacket([this, id, &media_ms, &packet_error](
                    const uint8_t* data, int rate, int duration, size_t len) {
                    if (packet_error || IsCancelled(id)) { packet_error = true; return; }
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (!active_ || cancelled_ || playback_id_ != id) { packet_error = true; return; }
                        // Mark the stream before enqueueing: the audio task may decode and
                        // drain this first packet immediately on another core.
                        stream_started_ = true;
                        playback_drained_ = false;
                    }
                    const esp_err_t result = audio_.SubmitNotificationFrame(
                        data, len, static_cast<uint32_t>(rate),
                        static_cast<uint32_t>(duration), id, media_ms);
                    if (result != ESP_OK) { packet_error = true; return; }
                    media_ms += static_cast<uint32_t>(duration);
                });
                std::array<char, kReadBufferSize> buffer{};
                while (!packet_error && !IsCancelled(id)) {
                    const int n = esp_http_client_read(http, buffer.data(), static_cast<int>(buffer.size()));
                    if (n < 0) { ESP_LOGE(kTag, "HTTP stream read failed"); break; }
                    if (n == 0) {
                        success = demuxer.Finish() && esp_http_client_is_complete_data_received(http);
                        if (!success) ESP_LOGW(kTag, "Invalid or incomplete Ogg Opus stream");
                        break;
                    }
                    demuxer.Process(reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<size_t>(n));
                    if (demuxer.HasError()) packet_error = true;
                }
            } else ESP_LOGW(kTag, "HTTP status %d for notify stream", status);
        } else ESP_LOGW(kTag, "HTTP open failed: %s", esp_err_to_name(opened));
        esp_http_client_close(http); esp_http_client_cleanup(http);
    } else ESP_LOGE(kTag, "HTTP client allocation failed");

    FinishedCallback cb; uint32_t completed_id = 0; bool report_success = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!cancelled_ && active_ && playback_id_ == id) {
            if (!success) { should_reset = stream_started_; cb = CompleteLocked(completed_id); }
            else { http_finished_ = true; if (playback_drained_) {
                cb = CompleteLocked(completed_id); report_success = true; } }
        }
    }
    // Keep worker_running_ set until cleanup finishes so a new notification
    // cannot enqueue audio just before a failed stream clears the old queue.
    if (should_reset) audio_.ResetNotificationPlayback();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_running_ = false;
        task_handle_ = nullptr;
    }
    if (cb) cb(completed_id, report_success);
}
}  // namespace chatai::xiaozhi
