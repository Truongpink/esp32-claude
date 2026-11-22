#include "audio_service.h"
#include <esp_log.h>
#include <cstring>

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)                                        \
    (esp_ae_rate_cvt_cfg_t) {                                                                \
        .src_rate = (uint32_t)(_src_rate), .dest_rate = (uint32_t)(_dest_rate),              \
        .channel = (uint8_t)(_channel), .bits_per_sample = ESP_AUDIO_BIT16, .complexity = 2, \
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,                                        \
    }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                                     \
    (esp_opus_dec_cfg_t) {                                                                 \
        .sample_rate = (uint32_t)(_sample_rate), .channel = ESP_AUDIO_MONO,                \
        .frame_duration =                                                                  \
            (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms), \
        .self_delimited = false,                                                           \
    }

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
#include "afe_audio_engine.h"
#else
#include "engines/lite_audio_engine.h"
#endif

#define TAG "AudioService"

AudioService::AudioService() { event_group_ = xEventGroupCreate(); }

AudioService::~AudioService() {
    Stop();
    if (audio_power_timer_ != nullptr) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_delete(audio_power_timer_);
        audio_power_timer_ = nullptr;
    }
    audio_engine_.reset();
    if (opus_encoder_ != nullptr) { esp_opus_enc_close(opus_encoder_); opus_encoder_ = nullptr; }
    if (opus_decoder_ != nullptr) { esp_opus_dec_close(opus_decoder_); opus_decoder_ = nullptr; }
    if (input_resampler_ != nullptr) { esp_ae_rate_cvt_close(input_resampler_); input_resampler_ = nullptr; }
    if (output_resampler_ != nullptr) { esp_ae_rate_cvt_close(output_resampler_); output_resampler_ = nullptr; }
    if (event_group_ != nullptr) { vEventGroupDelete(event_group_); event_group_ = nullptr; }
}

esp_err_t AudioService::Initialize(AudioCodec* codec) {
    if (codec == nullptr || event_group_ == nullptr) return ESP_ERR_INVALID_ARG;
    if (initialized_) return ESP_OK;
    codec_ = codec;
    codec_->Start();

    esp_opus_dec_cfg_t opus_dec_cfg =
        OPUS_DEC_CFG(codec->output_sample_rate(), OPUS_FRAME_DURATION_MS);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
        return ESP_FAIL;
    } else {
        decoder_sample_rate_ = codec->output_sample_rate();
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
        return ESP_FAIL;
    } else {
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
    }

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
        auto resampler_ret = esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
        if (input_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create input resampler, error code: %d", resampler_ret);
            return ESP_FAIL;
        }
    }

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
    audio_engine_ = std::make_unique<AfeAudioEngine>();
#else
    audio_engine_ = std::make_unique<LiteAudioEngine>();
#endif
    audio_engine_->OnOutput([this](std::vector<int16_t>&& data) {
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });
    audio_engine_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });
    audio_engine_->OnWakeWordDetected([this](const std::string& wake_word) {
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        if (callbacks_.on_wake_word_detected) {
            callbacks_.on_wake_word_detected(wake_word);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback =
            [](void* arg) {
                AudioService* audio_service = (AudioService*)arg;
                audio_service->CheckAndUpdateAudioPowerState();
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    const esp_err_t timer_result = esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
    if (timer_result != ESP_OK || audio_power_timer_ == nullptr) return timer_result == ESP_OK ? ESP_FAIL : timer_result;
    initialized_ = true;
    return ESP_OK;
}

esp_err_t AudioService::Start() {
    if (!initialized_ || event_group_ == nullptr || audio_power_timer_ == nullptr) return ESP_ERR_INVALID_STATE;
    if (!service_stopped_.exchange(false)) return ESP_OK;
    const EventBits_t exits = AS_EVENT_INPUT_TASK_EXITED | AS_EVENT_OUTPUT_TASK_EXITED | AS_EVENT_CODEC_TASK_EXITED;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING | AS_EVENT_AUDIO_INPUT_STOP_REQUEST | exits);
    started_task_exit_bits_ = 0;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_encode_queue_.clear(); audio_decode_queue_.clear(); audio_send_queue_.clear();
        audio_playback_queue_.clear(); audio_testing_queue_.clear(); timestamp_queue_.clear();
        decode_in_flight_ = false; output_in_flight_ = false; playback_drained_notified_ = true;
    }
    const auto abort_start = [this]() {
        service_stopped_.store(true);
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
            AS_EVENT_AUDIO_PROCESSOR_RUNNING | AS_EVENT_AUDIO_INPUT_STOP_REQUEST);
        audio_queue_cv_.notify_all();
        if (started_task_exit_bits_) {
            xEventGroupWaitBits(event_group_, started_task_exit_bits_, pdTRUE, pdTRUE, portMAX_DELAY);
            vTaskDelay(1);
        }
        audio_input_task_handle_ = nullptr; audio_output_task_handle_ = nullptr;
        opus_codec_task_handle_ = nullptr; started_task_exit_bits_ = 0;
        return ESP_ERR_NO_MEM;
    };
#if CONFIG_USE_AUDIO_PROCESSOR
    const BaseType_t input_result = xTaskCreatePinnedToCore([](void* arg) {
        auto* s=static_cast<AudioService*>(arg); s->AudioInputTask();
        xEventGroupSetBits(s->event_group_,AS_EVENT_INPUT_TASK_EXITED); vTaskDelete(nullptr);
    },"audio_input",2048*3,this,8,&audio_input_task_handle_,0);
#else
    const BaseType_t input_result = xTaskCreate([](void* arg) {
        auto* s=static_cast<AudioService*>(arg); s->AudioInputTask();
        xEventGroupSetBits(s->event_group_,AS_EVENT_INPUT_TASK_EXITED); vTaskDelete(nullptr);
    },"audio_input",2048*2,this,8,&audio_input_task_handle_);
#endif
    if(input_result!=pdPASS)return abort_start();
    started_task_exit_bits_|=AS_EVENT_INPUT_TASK_EXITED;
#if CONFIG_USE_AUDIO_PROCESSOR
    const BaseType_t output_result=xTaskCreate([](void* arg) {
        auto* s=static_cast<AudioService*>(arg); s->AudioOutputTask();
        xEventGroupSetBits(s->event_group_,AS_EVENT_OUTPUT_TASK_EXITED); vTaskDelete(nullptr);
    },"audio_output",2048*2,this,4,&audio_output_task_handle_);
#else
    const BaseType_t output_result=xTaskCreate([](void* arg) {
        auto* s=static_cast<AudioService*>(arg); s->AudioOutputTask();
        xEventGroupSetBits(s->event_group_,AS_EVENT_OUTPUT_TASK_EXITED); vTaskDelete(nullptr);
    },"audio_output",2048,this,4,&audio_output_task_handle_);
#endif
    if(output_result!=pdPASS)return abort_start();
    started_task_exit_bits_|=AS_EVENT_OUTPUT_TASK_EXITED;
    const BaseType_t codec_result=xTaskCreate([](void* arg) {
        auto* s=static_cast<AudioService*>(arg); s->OpusCodecTask();
        xEventGroupSetBits(s->event_group_,AS_EVENT_CODEC_TASK_EXITED); vTaskDelete(nullptr);
    },"opus_codec",2048*12,this,2,&opus_codec_task_handle_);
    if(codec_result!=pdPASS)return abort_start();
    started_task_exit_bits_|=AS_EVENT_CODEC_TASK_EXITED;
    const esp_err_t timer_result=esp_timer_start_periodic(audio_power_timer_,1000000);
    if(timer_result!=ESP_OK){Stop();return timer_result;}
    return ESP_OK;
}

void AudioService::Stop() {
    if(event_group_==nullptr||service_stopped_.exchange(true))return;
    if (audio_engine_initialized_) {
        audio_engine_->EnableVoiceProcessing(false);
        voice_detected_ = false;
    }
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    if(audio_power_timer_!=nullptr)esp_timer_stop(audio_power_timer_);
    xEventGroupSetBits(event_group_,AS_EVENT_AUDIO_TESTING_RUNNING|AS_EVENT_WAKE_WORD_RUNNING|
        AS_EVENT_AUDIO_PROCESSOR_RUNNING|AS_EVENT_AUDIO_INPUT_STOP_REQUEST);
    audio_queue_cv_.notify_all();
    const EventBits_t wait_bits=started_task_exit_bits_;
    if(wait_bits){xEventGroupWaitBits(event_group_,wait_bits,pdTRUE,pdTRUE,portMAX_DELAY);vTaskDelay(1);}
    audio_input_task_handle_=nullptr;audio_output_task_handle_=nullptr;opus_codec_task_handle_=nullptr;
    started_task_exit_bits_=0;
    bool notify_drained=false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        ++playback_generation_;
        audio_encode_queue_.clear();audio_decode_queue_.clear();audio_send_queue_.clear();
        audio_playback_queue_.clear();audio_testing_queue_.clear();timestamp_queue_.clear();
        decode_in_flight_=false;output_in_flight_=false;
        notify_drained=MarkPlaybackDrainedLocked();audio_queue_cv_.notify_all();
    }
    if(notify_drained&&callbacks_.on_playback_drained)callbacks_.on_playback_drained();
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (input_resampler_ != nullptr) {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            uint32_t output_samples = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num,
                                                   &output_samples);
            auto resampled = std::vector<int16_t>(output_samples * codec_->input_channels());
            uint32_t actual_output = output_samples;
            esp_ae_rate_cvt_process(input_resampler_, (esp_ae_sample_t)data.data(), in_sample_num,
                                    (esp_ae_sample_t)resampled.data(), &actual_output);
            resampled.resize(actual_output * codec_->input_channels());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

void AudioService::AudioInputTask() {
    constexpr EventBits_t kAudioInputActiveBits = AS_EVENT_AUDIO_TESTING_RUNNING |
                                                  AS_EVENT_WAKE_WORD_RUNNING |
                                                  AS_EVENT_AUDIO_PROCESSOR_RUNNING;

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            event_group_, kAudioInputActiveBits | AS_EVENT_AUDIO_INPUT_STOP_REQUEST, pdFALSE,
            pdFALSE, portMAX_DELAY);

        if (service_stopped_.load()) {
            if (codec_->input_enabled()) {
                codec_->EnableInput(false);
            }
            break;
        }

        if (bits & AS_EVENT_AUDIO_INPUT_STOP_REQUEST) {
            xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);
            bits = xEventGroupGetBits(event_group_);
            if ((bits & kAudioInputActiveBits) == 0) {
                if (codec_->input_enabled()) {
                    codec_->EnableInput(false);
                }
                continue;
            }
        }

        if (audio_input_need_warmup_.exchange(false)) {
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            bool testing_queue_full = false;
            {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                testing_queue_full = audio_testing_queue_.full();
            }
            if (testing_queue_full) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
            int samples = 160;
            std::vector<int16_t> data;
            if (ReadAudioData(data, 16000, samples)) {
                audio_engine_->Feed(std::move(data));
            }
        }
    }
}

void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_.load() || !audio_playback_queue_.empty();
        });
        if (service_stopped_.load()) break;
        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        output_in_flight_ = true;
        lock.unlock();

        if (!codec_->output_enabled()) {
            codec_->EnableOutput(true);
        }
        if (codec_->output_enabled()) {
            codec_->OutputData(task.pcm);
        }

        lock.lock();
        output_in_flight_ = false;
        const bool notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
        lock.unlock();
        if (notify_drained && callbacks_.on_playback_drained) {
            callbacks_.on_playback_drained();
        }
    }
}

void AudioService::OpusCodecTask() {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    while (true) {
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_.load() || !audio_decode_queue_.empty() ||
                   !audio_encode_queue_.empty();
        });
        if (service_stopped_.load()) break;

        if (!audio_decode_queue_.empty()) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            decode_in_flight_ = true;
            const uint32_t generation = playback_generation_;
            AudioTask task;
            task.type = kAudioTaskTypeDecodeToPlaybackQueue;
            task.timestamp = packet->timestamp;
            task.playback_id = packet->playback_id;
            task.media_position_ms = packet->media_position_ms;

            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            bool decoded = false;
            if (opus_decoder_ != nullptr) {
                task.pcm.resize(decoder_frame_size_);
                esp_audio_dec_in_raw_t raw = {
                    .buffer = (uint8_t*)(packet->payload.data()),
                    .len = (uint32_t)(packet->payload.size()),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };
                esp_audio_dec_out_frame_t out_frame{};
                out_frame.buffer = reinterpret_cast<uint8_t*>(task.pcm.data());
                out_frame.len = static_cast<uint32_t>(task.pcm.size() * sizeof(int16_t));
                esp_audio_dec_info_t dec_info = {};
                std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
                auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                decoder_lock.unlock();
                if (ret == ESP_AUDIO_ERR_OK) {
                    task.pcm.resize(out_frame.decoded_size / sizeof(int16_t));
                    {
                        std::lock_guard<std::mutex> resampler_lock(output_resampler_mutex_);
                        if (decoder_sample_rate_ != codec_->output_sample_rate() &&
                            output_resampler_ != nullptr) {
                            uint32_t target_size = 0;
                            esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, task.pcm.size(),
                                                                   &target_size);
                            std::vector<int16_t> resampled(target_size);
                            uint32_t actual_output = target_size;
                            esp_ae_rate_cvt_process(output_resampler_, (esp_ae_sample_t)task.pcm.data(),
                                                    task.pcm.size(), (esp_ae_sample_t)resampled.data(),
                                                    &actual_output);
                            resampled.resize(actual_output);
                            task.pcm = std::move(resampled);
                        }
                    }
                    decoded = true;
                } else {
                    ESP_LOGE(TAG, "Failed to decode audio after resize, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG, "Audio decoder is not configured");
            }

            lock.lock();
            if (decoded && generation == playback_generation_ && !service_stopped_.load()) {
                audio_playback_queue_.push_back(std::move(task));
            }
            decode_in_flight_ = false;
            debug_statistics_.decode_count++;
            const bool notify_drained = MarkPlaybackDrainedLocked();
            audio_queue_cv_.notify_all();
            lock.unlock();
            if (notify_drained && callbacks_.on_playback_drained) {
                callbacks_.on_playback_drained();
            }
            lock.lock();
        }
        if (!audio_encode_queue_.empty()) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task.timestamp;

            if (opus_encoder_ != nullptr && task.pcm.size() == encoder_frame_size_) {
                packet->payload.resize(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t*)(task.pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out{};
                out.buffer = packet->payload.data();
                out.len = static_cast<uint32_t>(encoder_outbuf_size_);
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    packet->payload.resize(out.encoded_bytes);

                    if (task.type == kAudioTaskTypeEncodeToSendQueue) {
                        {
                            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                            if (audio_send_queue_.size() >= MAX_SEND_PACKETS_IN_QUEUE) {
                                audio_send_queue_.pop_front();
                            }
                            audio_send_queue_.push_back(std::move(packet));
                        }
                        if (callbacks_.on_send_queue_available) {
                            callbacks_.on_send_queue_available();
                        }
                    } else if (task.type == kAudioTaskTypeEncodeToTestingQueue) {
                        std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                        audio_testing_queue_.push_back(std::move(packet));
                    }
                }
            }

            lock.lock();
            debug_statistics_.encode_count++;
        }
    }
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration_ms) {
    std::lock_guard<std::mutex> decoder_lock(decoder_mutex_);
    if (decoder_sample_rate_ == sample_rate && decoder_duration_ms_ == frame_duration_ms) {
        return;
    }

    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }

    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration_ms);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to reconfigure audio decoder, error code: %d", ret);
        return;
    }

    decoder_sample_rate_ = sample_rate;
    decoder_duration_ms_ = frame_duration_ms;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * frame_duration_ms;

    std::lock_guard<std::mutex> resampler_lock(output_resampler_mutex_);
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
        output_resampler_ = nullptr;
    }

    if (decoder_sample_rate_ != codec_->output_sample_rate()) {
        esp_ae_rate_cvt_cfg_t output_resampler_cfg =
            RATE_CVT_CFG(decoder_sample_rate_, codec_->output_sample_rate(), 1);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
        }
    }
}

void AudioService::CheckAndUpdateAudioPowerState() {
    if (service_stopped_.load()) return;
    auto now = std::chrono::steady_clock::now();
    if (now - last_input_time_ > std::chrono::milliseconds(AUDIO_POWER_CHECK_INTERVAL_MS)) {
        RequestInputStop();
    }
}

void AudioService::RequestInputStop() {
    if (event_group_ == nullptr) return;
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);
    audio_queue_cv_.notify_all();
}

void AudioService::EnableAudioTesting(bool enable) {
    if (!initialized_ || event_group_ == nullptr) return;
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        RequestInputStop();
    }
}

bool AudioService::EnableVoiceProcessing(bool enable) {
    if (!initialized_ || event_group_ == nullptr) return false;
    if (enable && !audio_engine_initialized_) {
        audio_engine_initialized_ = audio_engine_->Initialize(codec_, OPUS_FRAME_DURATION_MS, nullptr);
        if (!audio_engine_initialized_) return false;
    }
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);
        audio_engine_->EnableVoiceProcessing(true);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
        audio_engine_->EnableVoiceProcessing(false);
        RequestInputStop();
    }
    return true;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    if (!initialized_ || event_group_ == nullptr) return;
    if (enable && !audio_engine_initialized_) {
        audio_engine_initialized_ = audio_engine_->Initialize(codec_, OPUS_FRAME_DURATION_MS, nullptr);
    }
    if (!audio_engine_initialized_) return;
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        RequestInputStop();
    }
    audio_engine_->EnableWakeWordDetection(enable);
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) return nullptr;
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    return packet;
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool front) {
    if (packet == nullptr) return false;
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (service_stopped_.load() || audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        return false;
    }
    if (front) audio_decode_queue_.push_front(std::move(packet));
    else audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}
