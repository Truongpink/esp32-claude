#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_err.h>

namespace chatai::audio {

using SpeakerReferenceCallback = void (*)(const int16_t* pcm24k, size_t samples, void* context);

// Hardware pins are provisional until verified on the physical XH-S3E-AI V1.0 board.
esp_err_t Initialize();
esp_err_t EnableMicrophone(bool enable);
bool IsMicrophoneEnabled();
esp_err_t EnableSpeaker(bool enable);

esp_err_t ReadMicrophonePcm16(int16_t* destination, size_t requested_samples,
                              size_t* samples_read, uint32_t timeout_ms = 100);
esp_err_t WriteSpeakerPcm16(const int16_t* samples, size_t sample_count,
                            size_t* samples_written, uint32_t timeout_ms = 100);
void SetOutputVolume(int volume_percent);
int GetOutputVolume();
esp_err_t SaveOutputVolume(int volume_percent);

// Supplies the exact 24 kHz PCM sent to the speaker to a future AEC/reference adapter.
// The callback runs synchronously in the caller's task context.
void SetSpeakerReferenceCallback(SpeakerReferenceCallback callback, void* context);

}  // namespace chatai::audio
