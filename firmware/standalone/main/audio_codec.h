#pragma once

// Minimal AudioCodec contract matching Xiaozhi v2.6.0's audio/audio_codec.h.
// The upstream header includes board.h, which would pull the Xiaozhi board
// abstraction into ChatAI. Keep this interface board-neutral so the injected
// ChatAI adapter remains the only hardware path.
#include <driver/i2s_std.h>
#include <vector>

class AudioCodec {
public:
    AudioCodec() = default;
    virtual ~AudioCodec() = default;

    virtual void SetOutputVolume(int volume) { output_volume_ = volume; }
    virtual void SetInputGain(float gain) { input_gain_ = gain; }
    virtual void EnableInput(bool enable) { input_enabled_ = enable; }
    virtual void EnableOutput(bool enable) { output_enabled_ = enable; }

    virtual void OutputData(std::vector<int16_t>& data) { Write(data.data(), data.size()); }
    virtual bool InputData(std::vector<int16_t>& data) {
        return Read(data.data(), static_cast<int>(data.size())) > 0;
    }
    virtual void Start() {}

    bool duplex() const { return duplex_; }
    bool input_reference() const { return input_reference_; }
    int input_sample_rate() const { return input_sample_rate_; }
    int output_sample_rate() const { return output_sample_rate_; }
    int input_channels() const { return input_channels_; }
    int output_channels() const { return output_channels_; }
    int output_volume() const { return output_volume_; }
    float input_gain() const { return input_gain_; }
    bool input_enabled() const { return input_enabled_; }
    bool output_enabled() const { return output_enabled_; }

protected:
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;

    bool duplex_ = false;
    bool input_reference_ = false;
    bool input_enabled_ = false;
    bool output_enabled_ = false;
    int input_sample_rate_ = 0;
    int output_sample_rate_ = 0;
    int input_channels_ = 1;
    int output_channels_ = 1;
    int output_volume_ = 70;
    float input_gain_ = 0.0f;

    virtual int Read(int16_t* destination, int samples) = 0;
    virtual int Write(const int16_t* samples, int count) = 0;
};
