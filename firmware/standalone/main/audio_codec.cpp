#include "audio_codec.h"

#include <climits>

AudioCodec::AudioCodec() = default;

AudioCodec::~AudioCodec() = default;

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
}

void AudioCodec::EnableInput(bool enable) {
    input_enabled_ = enable;
}

void AudioCodec::EnableOutput(bool enable) {
    output_enabled_ = enable;
}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    if (data.size() <= static_cast<size_t>(INT_MAX)) {
        Write(data.data(), static_cast<int>(data.size()));
    }
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    if (data.size() > static_cast<size_t>(INT_MAX)) {
        return false;
    }
    return Read(data.data(), static_cast<int>(data.size())) > 0;
}

void AudioCodec::Start() {
    // ChatAI owns persistent audio settings and hardware initialization.
}
