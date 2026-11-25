#include "audio_service.h"
#include "diagnostic_manager.h"

#include <esp_log.h>
#include <freertos/semphr.h>
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
    ReleaseResources();
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void AudioService::ReleaseResources() {
    if (audio_power_timer_ != nullptr) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_delete(audio_power_timer_);
        audio_power_timer_ = nullptr;
    }
    audio_engine_.reset();
    audio_engine_initialized_ = false;
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
        opus_encoder_ = nullptr;
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
        input_resampler_ = nullptr;
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
        output_resampler_ = nullptr;
    }
    // ReleaseResources() is also used by partial-start rollback and destruction,
    // where Stop() may have returned early because the service was already
    // marked stopped. Always leave the ChatAI-owned hardware channels disabled.
    if (codec_ != nullptr) {
        if (codec_->input_enabled()) {
            codec_->EnableInput(false);
        }
        if (codec_->output_enabled()) {
            codec_->EnableOutput(false);
        }
    }
    codec_ = nullptr;
    initialized_ = false;
}

esp_err_t AudioService::Initialize(AudioCodec* codec) {
    if (codec == nullptr || event_group_ == nullptr) return ESP_ERR_INVALID_ARG;
    if (initialized_) return ESP_OK;
    codec_ = codec;

    // Initialization runs before the audio tasks are started. If any resource
    // allocation below fails, release everything acquired so far so the
    // singleton Application can exit without retaining a half-built pipeline.
    const auto rollback_initialization = [this](esp_err_t error) {
        ReleaseResources();
        return error;
    };

    esp_opus_dec_cfg_t opus_dec_cfg =
        OPUS_DEC_CFG(codec->output_sample_rate(), OPUS_FRAME_DURATION_MS);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
        return rollback_initialization(ESP_FAIL);
    } else {
        decoder_sample_rate_ = codec->output_sample_rate();
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
        return rollback_initialization(ESP_FAIL);
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