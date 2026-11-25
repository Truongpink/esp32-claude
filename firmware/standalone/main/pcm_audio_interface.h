#pragma once

#include <cstdint>
#include <vector>

#include <esp_err.h>

namespace chatai::audio {

// Hardware-independent PCM contract. Implementations must not create or own
// I2S channels; the platform AudioManager remains the hardware owner.
class PcmAudioInterface {
public:
    virtual ~PcmAudioInterface() = default;

    virtual esp_err_t Start() = 0;
    virtual esp_err_t Stop() = 0;
    virtual esp_err_t EnableInput(bool enable) = 0;
    virtual esp_err_t EnableOutput(bool enable) = 0;

    virtual void SetOutputVolume(int volume_percent) = 0;
    virtual esp_err_t SetInputGain(float gain) = 0;

    // Read/Write return the number of PCM16 samples transferred, or zero.
    virtual bool InputData(std::vector<int16_t>& pcm) = 0;
    virtual esp_err_t OutputData(const std::vector<int16_t>& pcm) = 0;
    virtual int Read(int16_t* destination, int samples) = 0;
    virtual int Write(const int16_t* samples, int count) = 0;

    virtual bool started() const = 0;
    virtual bool input_enabled() const = 0;
    virtual bool output_enabled() const = 0;
    virtual bool duplex() const = 0;
    virtual bool input_reference() const = 0;
    virtual int input_sample_rate() const = 0;
    virtual int output_sample_rate() const = 0;
    virtual int input_channels() const = 0;
    virtual int output_channels() const = 0;
    virtual int output_volume() const = 0;
    virtual esp_err_t last_error() const = 0;
};

}  // namespace chatai::audio
