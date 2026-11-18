from pathlib import Path

def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"Expected one patch anchor in {path}: found {count}: {old[:100]!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def apply(root: Path, upstream: Path) -> None:
    # The upstream default disables wake-word detection. Explicitly enable
    # English MultiNet 7 for the ESP32-S3 and package its model into assets.bin.
    sdkconfig_defaults = upstream / "sdkconfig.defaults.esp32s3"
    defaults = sdkconfig_defaults.read_text(encoding="utf-8")
    wake_settings = {
        "CONFIG_USE_CUSTOM_WAKE_WORD": "y",
        "CONFIG_SR_MN_EN_MULTINET7_QUANT": "y",
        "CONFIG_CUSTOM_WAKE_WORD": '"hey chatai"',
        "CONFIG_CUSTOM_WAKE_WORD_DISPLAY": '"Hey ChatAI"',
        "CONFIG_CUSTOM_WAKE_WORD_THRESHOLD": "20",
        "CONFIG_ESP_TASK_WDT_TIMEOUT_S": "30",
    }
    lines = [line for line in defaults.splitlines()
             if not any(line.startswith(key + "=") for key in wake_settings)]
    lines.extend(("", "# ChatAI TPIOS audio watchdog and English MultiNet wake word", *(
        f"{key}={value}" for key, value in wake_settings.items()
    )))
    sdkconfig_defaults.write_text("\n".join(lines) + "\n", encoding="utf-8")

    # MultiNet command words are loaded from assets, then overridden by the
    # user's persisted phrase. The setter serializes dictionary updates with Feed().
    wake_h = upstream / "main/audio/wake_words/custom_wake_word.h"
    wake_cc = upstream / "main/audio/wake_words/custom_wake_word.cc"
    replace_once(
        wake_h,
        "    std::vector<std::string> GetWakeWords() const override;\n",
        "    std::vector<std::string> GetWakeWords() const override;\n"
        "    bool SetWakeWordPhrase(const std::string& phrase);\n",
    )
    replace_once(
        wake_cc,
        '#include "assets.h"\n',
        '#include "assets.h"\n#include "settings.h"\n#include <algorithm>\n#include <cctype>\n',
    )
    replace_once(
        wake_cc,
        "    } else {\n        models_ = models_list;\n        ParseWakenetModelConfig();\n    }\n\n    if (models_ == nullptr",
        "    } else {\n        models_ = models_list;\n        ParseWakenetModelConfig();\n    }\n\n"
        "    Settings saved_settings(\"chatai\", false);\n"
        "    const std::string saved_phrase = saved_settings.GetString(\"wake_word\");\n"
        "    if (!saved_phrase.empty()) {\n"
        "        std::string command = saved_phrase;\n"
        "        std::transform(command.begin(), command.end(), command.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });\n"
        "        commands_.clear();\n"
        "        commands_.push_back({command, saved_phrase, \"wake\"});\n"
        "    }\n\n"
        "    if (models_ == nullptr",
    )
    replace_once(
        wake_cc,
        "void CustomWakeWord::Start() {\n",
        "bool CustomWakeWord::SetWakeWordPhrase(const std::string& phrase) {\n"
        "    if (multinet_ == nullptr || multinet_model_data_ == nullptr || phrase.empty()) {\n"
        "        return false;\n"
        "    }\n"
        "    std::string command = phrase;\n"
        "    std::transform(command.begin(), command.end(), command.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });\n"
        "    std::lock_guard<std::mutex> lock(input_buffer_mutex_);\n"
        "    const bool was_running = running_.load();\n"
        "    running_ = false;\n"
        "    esp_mn_commands_clear();\n"
        "    esp_mn_commands_add(1, command.c_str());\n"
        "    esp_mn_commands_update();\n"
        "    multinet_->clean(multinet_model_data_);\n"
        "    commands_.clear();\n"
        "    commands_.push_back({command, phrase, \"wake\"});\n"
        "    input_buffer_.clear();\n"
        "    running_ = was_running;\n"
        "    ESP_LOGI(TAG, \"Wake phrase updated: %s\", phrase.c_str());\n"
        "    return true;\n"
        "}\n\n"
        "void CustomWakeWord::Start() {\n",
    )

    # Keep the engine interface compatible with both AFE and Lite engines.
    engine_h = upstream / "main/audio/audio_engine.h"
    replace_once(
        engine_h,
        "    virtual std::vector<std::string> GetWakeWords() const = 0;\n",
        "    virtual std::vector<std::string> GetWakeWords() const = 0;\n"
        "    virtual bool SetWakeWordPhrase(const std::string& phrase) { (void)phrase; return false; }\n",
    )
    afe_h = upstream / "main/audio/engines/afe_audio_engine.h"
    replace_once(
        afe_h,
        "    static constexpr size_t kProcessingTaskStackSize = 4096;\n",
        "    static constexpr size_t kProcessingTaskStackSize = 16384;\n",
    )
    afe_cc = upstream / "main/audio/engines/afe_audio_engine.cc"
    replace_once(
        afe_h,
        "    std::vector<std::string> GetWakeWords() const override;\n",
        "    std::vector<std::string> GetWakeWords() const override;\n"
        "    bool SetWakeWordPhrase(const std::string& phrase) override;\n",
    )
    replace_once(
        afe_cc,
        "std::vector<std::string> AfeAudioEngine::GetWakeWords() const {\n",
        "bool AfeAudioEngine::SetWakeWordPhrase(const std::string& phrase) {\n"
        "    return custom_wake_word_ && custom_wake_word_->SetWakeWordPhrase(phrase);\n"
        "}\n\n"
        "std::vector<std::string> AfeAudioEngine::GetWakeWords() const {\n",
    )
    service_h = upstream / "main/audio/audio_service.h"
    service_cc = upstream / "main/audio/audio_service.cc"
    replace_once(
        service_h,
        "    std::vector<std::string> GetWakeWords() const;\n",
        "    std::vector<std::string> GetWakeWords() const;\n"
        "    bool SetWakeWordPhrase(const std::string& phrase);\n",
    )
    replace_once(
        service_cc,
        "std::vector<std::string> AudioService::GetWakeWords() const {\n"
        "    return audio_engine_initialized_ ? audio_engine_->GetWakeWords() : std::vector<std::string>{};\n"
        "}\n",
        "std::vector<std::string> AudioService::GetWakeWords() const {\n"
        "    return audio_engine_initialized_ ? audio_engine_->GetWakeWords() : std::vector<std::string>{};\n"
        "}\n\n"
        "bool AudioService::SetWakeWordPhrase(const std::string& phrase) {\n"
        "    if (!audio_engine_initialized_ && !InitializeAudioEngine()) {\n"
        "        return false;\n"
        "    }\n"
        "    return audio_engine_ && audio_engine_->SetWakeWordPhrase(phrase);\n"
        "}\n",
    )

    # Persist microphone gain and apply it to captured PCM in software (0-20 dB).
    codec_cc = upstream / "main/audio/audio_codec.cc"
    replace_once(
        codec_cc,
        "#include <cstring>\n",
        "#include <cstring>\n#include <cmath>\n#include <algorithm>\n",
    )
    replace_once(
        codec_cc,
        "bool AudioCodec::InputData(std::vector<int16_t>& data) {\n"
        "    int samples = Read(data.data(), data.size());\n"
        "    if (samples > 0) {\n"
        "        return true;\n"
        "    }\n"
        "    return false;\n"
        "}\n",
        "bool AudioCodec::InputData(std::vector<int16_t>& data) {\n"
        "    int samples = Read(data.data(), data.size());\n"
        "    if (samples > 0) {\n"
        "        if (input_gain_ > 0.0f) {\n"
        "            const float multiplier = std::pow(10.0f, input_gain_ / 20.0f);\n"
        "            for (int i = 0; i < samples; ++i) {\n"
        "                const long amplified = std::lround(static_cast<float>(data[i]) * multiplier);\n"
        "                data[i] = static_cast<int16_t>(std::clamp(amplified, -32768L, 32767L));\n"
        "            }\n"
        "        }\n"
        "        return true;\n"
        "    }\n"
        "    return false;\n"
        "}\n",
    )
    replace_once(
        codec_cc,
        "    output_volume_ = settings.GetInt(\"output_volume\", output_volume_);\n",
        "    output_volume_ = settings.GetInt(\"output_volume\", output_volume_);\n"
        "    Settings chatai_settings(\"chatai\", false);\n"
        "    input_gain_ = static_cast<float>(chatai_settings.GetInt(\"mic_gain\", 0));\n",
    )
    replace_once(
        codec_cc,
        "void AudioCodec::SetInputGain(float gain) {\n"
        "    input_gain_ = gain;\n"
        "    ESP_LOGI(TAG, \"Set input gain to %.1f\", input_gain_);\n"
        "}\n",
        "void AudioCodec::SetInputGain(float gain) {\n"
        "    input_gain_ = std::clamp(gain, 0.0f, 20.0f);\n"
        "    Settings settings(\"chatai\", true);\n"
        "    settings.SetInt(\"mic_gain\", static_cast<int>(std::lround(input_gain_)));\n"
        "    ESP_LOGI(TAG, \"Set microphone software gain to %.1f dB\", input_gain_);\n"
        "}\n",
    )

    # Use the existing half-duplex state machine: stop AFE while the assistant
    # speaks, then restart listening after playback drains.
    application_cc = upstream / "main/application.cc"
    replace_once(
        application_cc,
        "ListeningMode Application::GetDefaultListeningMode() const {\n"
        "    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;\n"
        "}\n",
        "ListeningMode Application::GetDefaultListeningMode() const {\n"
        "    return kListeningModeAutoStop;\n"
        "}\n",
    )
