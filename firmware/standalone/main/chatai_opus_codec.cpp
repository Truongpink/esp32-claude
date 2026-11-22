#include "chatai_opus_codec.h"

#include <decoder/impl/esp_opus_dec.h>
#include <encoder/impl/esp_opus_enc.h>

namespace chatai::audio {

ChatAIOpusCodec::~ChatAIOpusCodec() {
    Deinitialize();
}

esp_err_t ChatAIOpusCodec::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (encoder_ != nullptr) {
        return ESP_OK;
    }

    esp_opus_enc_config_t config = {
        .sample_rate = static_cast<int>(kInputSampleRate),
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = ESP_OPUS_BITRATE_AUTO,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
        .complexity = 0,
        .enable_fec = false,
        .enable_dtx = true,
        .enable_vbr = true,
    };

    const esp_audio_err_t result =
        esp_opus_enc_open(&config, sizeof(config), &encoder_);
    if (result != ESP_AUDIO_ERR_OK || encoder_ == nullptr) {
        encoder_ = nullptr;
        return ESP_FAIL;
    }

    const esp_audio_err_t frame_result = esp_opus_enc_get_frame_size(
        encoder_, &encoder_input_bytes_, &encoder_output_bytes_);
    if (frame_result != ESP_AUDIO_ERR_OK ||
        encoder_input_bytes_ != static_cast<int>(kInputFrameSamples * sizeof(int16_t)) ||
        encoder_output_bytes_ <= 0) {
        esp_opus_enc_close(encoder_);
        encoder_ = nullptr;
        encoder_input_bytes_ = 0;
        encoder_output_bytes_ = 0;
        return ESP_FAIL;
    }

    return ESP_OK;
}

void ChatAIOpusCodec::Deinitialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (decoder_ != nullptr) {
        esp_opus_dec_close(decoder_);
        decoder_ = nullptr;
    }
    if (encoder_ != nullptr) {
        esp_opus_enc_close(encoder_);
        encoder_ = nullptr;
    }
    decoder_sample_rate_ = 0;
    decoder_frame_duration_ms_ = 0;
    encoder_input_bytes_ = 0;
    encoder_output_bytes_ = 0;
}

esp_err_t ChatAIOpusCodec::EncodeFrame(
    const int16_t* pcm, size_t sample_count, std::vector<uint8_t>& encoded) {
    encoded.clear();
    if (pcm == nullptr || sample_count != kInputFrameSamples) {
        return ESP_ERR_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (encoder_ == nullptr || encoder_input_bytes_ <= 0 ||
        encoder_output_bytes_ <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    encoded.resize(static_cast<size_t>(encoder_output_bytes_));
    esp_audio_enc_in_frame_t input = {
        .buffer = reinterpret_cast<uint8_t*>(const_cast<int16_t*>(pcm)),
        .len = static_cast<uint32_t>(encoder_input_bytes_),
    };
    esp_audio_enc_out_frame_t output = {
        .buffer = encoded.data(),
        .len = static_cast<uint32_t>(encoded.size()),
        .encoded_bytes = 0,
    };

    const esp_audio_err_t result =
        esp_opus_enc_process(encoder_, &input, &output);
    if (result != ESP_AUDIO_ERR_OK ||
        output.encoded_bytes > encoded.size()) {
        encoded.clear();
        return ESP_FAIL;
    }

    encoded.resize(output.encoded_bytes);
    return encoded.empty() ? ESP_FAIL : ESP_OK;
}

esp_err_t ChatAIOpusCodec::DecodeFrame(
    const uint8_t* encoded, size_t encoded_size, uint32_t sample_rate,
    uint32_t frame_duration_ms, std::vector<int16_t>& pcm) {
    pcm.clear();
    if (encoded == nullptr || encoded_size == 0 ||
        encoded_size > UINT32_MAX || !IsSupportedSampleRate(sample_rate) ||
        !GetDecoderFrameDuration(frame_duration_ms, nullptr)) {
        return ESP_ERR_INVALID_ARG;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (encoder_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (decoder_ == nullptr || decoder_sample_rate_ != sample_rate ||
        decoder_frame_duration_ms_ != frame_duration_ms) {
        const esp_err_t open_result =
            OpenDecoderLocked(sample_rate, frame_duration_ms);
        if (open_result != ESP_OK) {
            return open_result;
        }
    }

    const uint64_t expected_samples =
        static_cast<uint64_t>(sample_rate) * frame_duration_ms / 1000;
    if (expected_samples == 0 ||
        expected_samples > static_cast<uint64_t>(SIZE_MAX / sizeof(int16_t)) ||
        expected_samples * sizeof(int16_t) > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    pcm.resize(static_cast<size_t>(expected_samples));
    esp_audio_dec_in_raw_t input = {
        .buffer = const_cast<uint8_t*>(encoded),
        .len = static_cast<uint32_t>(encoded_size),
        .consumed = 0,
        .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
    };
    esp_audio_dec_out_frame_t output = {
        .buffer = reinterpret_cast<uint8_t*>(pcm.data()),
        .len = static_cast<uint32_t>(pcm.size() * sizeof(int16_t)),
        .needed_size = 0,
        .decoded_size = 0,
    };
    esp_audio_dec_info_t info = {};

    const esp_audio_err_t result =
        esp_opus_dec_decode(decoder_, &input, &output, &info);
    if (result != ESP_AUDIO_ERR_OK || output.decoded_size == 0 ||
        output.decoded_size > output.len ||
        output.decoded_size % sizeof(int16_t) != 0 ||
        input.consumed != encoded_size) {
        pcm.clear();
        return ESP_FAIL;
    }

    pcm.resize(output.decoded_size / sizeof(int16_t));
    return ESP_OK;
}

esp_err_t ChatAIOpusCodec::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (encoder_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (esp_opus_enc_reset(encoder_) != ESP_AUDIO_ERR_OK) {
        return ESP_FAIL;
    }
    if (decoder_ != nullptr &&
        esp_opus_dec_reset(decoder_) != ESP_AUDIO_ERR_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool ChatAIOpusCodec::IsSupportedSampleRate(uint32_t sample_rate) {
    return sample_rate == 8000 || sample_rate == 12000 ||
           sample_rate == 16000 || sample_rate == 24000 ||
           sample_rate == 48000;
}

bool ChatAIOpusCodec::GetDecoderFrameDuration(
    uint32_t duration_ms, int* duration) {
    int value = ESP_OPUS_DEC_FRAME_DURATION_INVALID;
    switch (duration_ms) {
        case 5: value = ESP_OPUS_DEC_FRAME_DURATION_5_MS; break;
        case 10: value = ESP_OPUS_DEC_FRAME_DURATION_10_MS; break;
        case 20: value = ESP_OPUS_DEC_FRAME_DURATION_20_MS; break;
        case 40: value = ESP_OPUS_DEC_FRAME_DURATION_40_MS; break;
        case 60: value = ESP_OPUS_DEC_FRAME_DURATION_60_MS; break;
        case 80: value = ESP_OPUS_DEC_FRAME_DURATION_80_MS; break;
        case 100: value = ESP_OPUS_DEC_FRAME_DURATION_100_MS; break;
        case 120: value = ESP_OPUS_DEC_FRAME_DURATION_120_MS; break;
        default: return false;
    }
    if (duration != nullptr) {
        *duration = value;
    }
    return true;
}

esp_err_t ChatAIOpusCodec::OpenDecoderLocked(
    uint32_t sample_rate, uint32_t frame_duration_ms) {
    int duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID;
    if (!IsSupportedSampleRate(sample_rate) ||
        !GetDecoderFrameDuration(frame_duration_ms, &duration)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (decoder_ != nullptr) {
        esp_opus_dec_close(decoder_);
        decoder_ = nullptr;
    }

    esp_opus_dec_cfg_t config = {
        .sample_rate = sample_rate,
        .channel = ESP_AUDIO_MONO,
        .frame_duration =
            static_cast<esp_opus_dec_frame_duration_t>(duration),
        .self_delimited = false,
    };

    const esp_audio_err_t result =
        esp_opus_dec_open(&config, sizeof(config), &decoder_);
    if (result != ESP_AUDIO_ERR_OK || decoder_ == nullptr) {
        decoder_ = nullptr;
        decoder_sample_rate_ = 0;
        decoder_frame_duration_ms_ = 0;
        return ESP_FAIL;
    }

    decoder_sample_rate_ = sample_rate;
    decoder_frame_duration_ms_ = frame_duration_ms;
    return ESP_OK;
}

}  // namespace chatai::audio
