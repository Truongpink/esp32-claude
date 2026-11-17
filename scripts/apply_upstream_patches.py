#!/usr/bin/env python3
"""Apply the ChatAI TPIOS board overlay to the pinned XiaoZhi source tree."""
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "xiaozhi-src"
BOARD_SRC = ROOT / "firmware" / "board" / "chatai-tpios"
BOARD_DST = UPSTREAM / "main" / "boards" / "chatai-tpios"

VERSION = (ROOT / "firmware" / "VERSION").read_text(encoding="utf-8").strip()
if not re.fullmatch(r"\\d+\\.\\d+\\.\\d+", VERSION):
    raise SystemExit("firmware/VERSION must contain a semantic version such as 0.1.2")


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"Expected exactly one patch anchor in {path}: {old!r}; found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")

if not (UPSTREAM / "CMakeLists.txt").is_file():
    raise SystemExit("Pinned upstream source not found at ./xiaozhi-src")

# Keep the ESP application descriptor version aligned with firmware/VERSION.
replace_once(
    UPSTREAM / "CMakeLists.txt",
    'set(PROJECT_VER "2.6.0")',
    f'set(PROJECT_VER "{VERSION}")',
)

shutil.copytree(BOARD_SRC, BOARD_DST, dirs_exist_ok=True)
PROTOCOL_SRC = ROOT / "firmware" / "protocols"
PROTOCOL_DST = UPSTREAM / "main" / "protocols"
shutil.copytree(PROTOCOL_SRC, PROTOCOL_DST, dirs_exist_ok=True)

# Brand the first-boot provisioning network and keep OTA out of the basic release UI.
wifi_board = UPSTREAM / "main" / "boards" / "common" / "wifi_board.cc"
replace_once(
    wifi_board,
    'config.ssid_prefix = "Xiaozhi";',
    'config.ssid_prefix = "ChatAI-TPIOS";',
)
replace_once(
    wifi_board,
    "config.show_ota_config = true;",
    "config.show_ota_config = false;",
)

kconfig = UPSTREAM / "main" / "Kconfig.projbuild"
replace_once(
    kconfig,
    "    default BOARD_TYPE_BREAD_COMPACT_WIFI if IDF_TARGET_ESP32S3",
    "    default BOARD_TYPE_CHAT_AI_TPIOS if IDF_TARGET_ESP32S3",
)
replace_once(
    kconfig,
    "    config BOARD_TYPE_BREAD_COMPACT_WIFI\n",
    '    config BOARD_TYPE_CHAT_AI_TPIOS\n'
    '        bool "ChatAI TPIOS (ESP32-S3, SSD1306 128x64)"\n'
    '        depends on IDF_TARGET_ESP32S3\n'
    '    config BOARD_TYPE_BREAD_COMPACT_WIFI\n',
)

# Make the upstream OLED type choice available to this board and select its
# verified 128x64 SSD1306 panel by default.
replace_once(
    kconfig,
    '    depends on BOARD_TYPE_BREAD_COMPACT_WIFI || BOARD_TYPE_BREAD_COMPACT_ML307 || BOARD_TYPE_BREAD_COMPACT_NT26 || BOARD_TYPE_BREAD_COMPACT_ESP32 || BOARD_TYPE_HU_087 || (BOARD_TYPE_WK_ESP32S3_DEV && WK_ESP32S3_DEV_DISPLAY_OLED)\n',
    '    depends on BOARD_TYPE_CHAT_AI_TPIOS || BOARD_TYPE_BREAD_COMPACT_WIFI || BOARD_TYPE_BREAD_COMPACT_ML307 || BOARD_TYPE_BREAD_COMPACT_NT26 || BOARD_TYPE_BREAD_COMPACT_ESP32 || BOARD_TYPE_HU_087 || (BOARD_TYPE_WK_ESP32S3_DEV && WK_ESP32S3_DEV_DISPLAY_OLED)\n',
)
replace_once(
    kconfig,
    '    default OLED_SSD1306_128X32\n',
    '    default OLED_SSD1306_128X64 if BOARD_TYPE_CHAT_AI_TPIOS\n'
    '    default OLED_SSD1306_128X32\n',
)

cmake = UPSTREAM / "main" / "CMakeLists.txt"
replace_once(
    cmake,
    'set(MAIN_PRIV_REQUIRES_EXTRA "")\n',
    'set(MAIN_PRIV_REQUIRES_EXTRA "")\nlist(APPEND MAIN_PRIV_REQUIRES_EXTRA esp_http_server)\n',
)

replace_once(
    cmake,
    "if(CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI)\n",
    'if(CONFIG_BOARD_TYPE_CHAT_AI_TPIOS)\n'
    '    set(BOARD_DIR "chatai-tpios")\n'
    '    set(BUILTIN_TEXT_FONT font_noto_sans_basic_14_1)\n'
    '    set(BUILTIN_ICON_FONT font_material_symbols_14_1)\n'
    'elseif(CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI)\n',
)


# Add the dedicated Gemini Live transport to the upstream component build.
replace_once(
    cmake,
    '            "protocols/websocket_protocol.cc"\n',
    '            "protocols/websocket_protocol.cc"\n'
    '            "protocols/gemini_live_protocol.cc"\n',
)

replace_once(
    UPSTREAM / "main" / "application.cc",
    '#include "websocket_protocol.h"\n',
    '#include "websocket_protocol.h"\n'
    '#include "gemini_live_protocol.h"\n',
)
replace_once(
    UPSTREAM / "main" / "application.cc",
    '    if (ota_->HasMqttConfig()) {\n'
    '        protocol_ = std::make_unique<MqttProtocol>();\n'
    '    } else if (ota_->HasWebsocketConfig()) {\n'
    '        protocol_ = std::make_unique<WebsocketProtocol>();\n'
    '    } else {\n'
    '        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");\n'
    '        protocol_ = std::make_unique<MqttProtocol>();\n'
    '    }\n',
    '    Settings gemini_settings("gemini", false);\n'
    '    if (gemini_settings.GetString("api_key").empty()) {\n'
    '        ESP_LOGW(TAG, "Gemini API key is not configured; use the local Web Admin after Wi-Fi setup");\n'
    '    }\n'
    '    protocol_ = std::make_unique<GeminiLiveProtocol>(audio_service_);\n',
)


# Keep the basic firmware independent from XiaoZhi cloud version/assets services.
replace_once(
    UPSTREAM / "main" / "application.cc",
    '    // Create OTA object for activation process\n'
    '    ota_ = std::make_unique<Ota>();\n'
    '\n'
    '    // Check for new assets version\n'
    '    CheckAssetsVersion();\n'
    '\n'
    '    // Check for new firmware version\n'
    '    CheckNewVersion();\n'
    '\n'
    '    // Initialize the protocol\n'
    '    InitializeProtocol();\n',
    '    // Keep the OTA object only for local version/status compatibility.\n'
    '    // Cloud firmware and assets updates are deferred for the basic release.\n'
    '    ota_ = std::make_unique<Ota>();\n'
    '\n'
    '    // Start Gemini Live without contacting XiaoZhi cloud services.\n'
    '    InitializeProtocol();\n',
)

# The basic release skips upstream cloud version checks, so validate a pending
# local OTA image after the application has initialized its selected protocol.
replace_once(
    UPSTREAM / "main" / "application.cc",
    '    // Start Gemini Live without contacting XiaoZhi cloud services.\n'
    '    InitializeProtocol();\n\n'
    '    // Signal completion to main loop\n',
    '    // Start Gemini Live without contacting XiaoZhi cloud services.\n'
    '    InitializeProtocol();\n\n'
    '    // Confirm the pending OTA image only after core startup succeeds.\n'
    '    if (last_error_message_.empty()) {\n'
    '        ota_->MarkCurrentVersionValid();\n'
    '    }\n\n'
    '    // Signal completion to main loop\n',
)

# Show the ChatAI TPIOS wordmark briefly before the ready status.
replace_once(
    UPSTREAM / "main" / "application.cc",
    '    // Setup the display\n'
    '    auto display = board.GetDisplay();\n'
    '    display->SetupUI();\n'
    '    // Print board name/version info\n'
    '    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());\n',
    '    // Setup the display\n'
    '    auto display = board.GetDisplay();\n'
    '    display->SetupUI();\n'
    '    display->SetChatMessage("system", "ChatAI TPIOS");\n'
    '    vTaskDelay(pdMS_TO_TICKS(1500));\n'
    '    display->SetChatMessage("system", "");\n'
    '    display->SetStatus("ChatAI sẵn sàng");\n',
)

# Add a bounded raw-PCM bridge for Gemini Live while preserving the default Opus path.
def patch_pcm_audio_bridge() -> None:
    header = UPSTREAM / "main" / "audio" / "audio_service.h"
    source = UPSTREAM / "main" / "audio" / "audio_service.cc"

    # Notify the application event loop when raw PCM is ready for transport.
    replace_once(
        header,
        "    std::function<void(void)> on_send_queue_available;\n",
        "    std::function<void(void)> on_send_queue_available;\n"
        "    std::function<void(void)> on_pcm_input_available;\n",
    )
    replace_once(
        header,
        "    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();\n",
        "    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();\n"
        "    // Optional raw PCM bridge for non-Opus transports (Gemini Live).\n"
        "    // Disabled by default; the existing XiaoZhi Opus path remains unchanged.\n"
        "    void EnablePcmBridge(bool enable);\n    void EnablePcmPlaybackMode(bool enable);\n"
        "    bool PopPcmInput(std::vector<int16_t>& pcm);\n"
        "    bool PushPcmPlayback(std::vector<int16_t>&& pcm);\n"
        "    void ClearPcmPlayback();\n",
    )
    replace_once(
        header,
        "    FixedQueue<AudioTask, MAX_PLAYBACK_TASKS_IN_QUEUE> audio_playback_queue_;\n",
        "    FixedQueue<AudioTask, 8> audio_playback_queue_;\n"
        "    // PCM playback queue holds eight 60 ms chunks; Opus remains limited to its original two tasks.\n"
        "    // Four 60 ms mono PCM frames bound memory while tolerating short network stalls.\n"
        "    FixedQueue<std::vector<int16_t>, 4> pcm_input_queue_;\n"
        "    std::atomic<bool> pcm_bridge_enabled_{false};\n"
        "    std::atomic<bool> pcm_playback_mode_{false};\n",
    )
    replace_once(
        source,
        "    audio_engine_->OnOutput([this](std::vector<int16_t>&& data) {\n"
        "        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));\n"
        "    });",
        "    audio_engine_->OnOutput([this](std::vector<int16_t>&& data) {\n"
        "        if (pcm_bridge_enabled_.load()) {\n"
        "            std::lock_guard<std::mutex> lock(audio_queue_mutex_);\n"
        "            if (pcm_input_queue_.full()) {\n"
        "                pcm_input_queue_.pop_front();  // Drop oldest real-time capture frame.\n"
        "            }\n"
        "            pcm_input_queue_.push_back(std::move(data));\n"
        "            audio_queue_cv_.notify_all();\n"
        "            if (callbacks_.on_pcm_input_available) {\n"
        "                callbacks_.on_pcm_input_available();\n"
        "            }\n"
        "            return;\n"
        "        }\n"
        "        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));\n"
        "    });",
    )
    replace_once(
        source,
        "void AudioService::EncodeWakeWord() {",
        "void AudioService::EnablePcmPlaybackMode(bool enable) {\n"
        "    pcm_playback_mode_.store(enable);\n"
        "}\n"
        "\nvoid AudioService::EnablePcmBridge(bool enable) {\n"
        "    pcm_bridge_enabled_.store(enable);\n"
        "    if (!enable) {\n"
        "        std::lock_guard<std::mutex> lock(audio_queue_mutex_);\n"
        "        pcm_input_queue_.clear();\n"
        "    }\n"
        "}\n"
        "\nbool AudioService::PopPcmInput(std::vector<int16_t>& pcm) {\n"
        "    std::lock_guard<std::mutex> lock(audio_queue_mutex_);\n"
        "    if (pcm_input_queue_.empty()) {\n"
        "        return false;\n"
        "    }\n"
        "    pcm = std::move(pcm_input_queue_.front());\n"
        "    pcm_input_queue_.pop_front();\n"
        "    return true;\n"
        "}\n"
        "\nbool AudioService::PushPcmPlayback(std::vector<int16_t>&& pcm) {\n"
        "    if (pcm.empty() || service_stopped_.load()) {\n"
        "        return false;\n"
        "    }\n"
        "    std::lock_guard<std::mutex> lock(audio_queue_mutex_);\n"
        "    if (audio_playback_queue_.full()) {\n"
        "        audio_playback_queue_.pop_front();  // Keep latency bounded for live speech.\n"
        "    }\n"
        "    AudioTask task;\n"
        "    task.type = kAudioTaskTypeDecodeToPlaybackQueue;\n"
        "    task.pcm = std::move(pcm);\n"
        "    audio_playback_queue_.push_back(std::move(task));\n"
        "    playback_drained_notified_ = false;\n"
        "    audio_queue_cv_.notify_all();\n"
        "    return true;\n"
        "}\n"
        "\nvoid AudioService::ClearPcmPlayback() {\n"
        "    bool notify_drained = false;\n"
        "    {\n"
        "        std::lock_guard<std::mutex> lock(audio_queue_mutex_);\n"
        "        audio_playback_queue_.clear();\n"
        "        notify_drained = MarkPlaybackDrainedLocked();\n"
        "        audio_queue_cv_.notify_all();\n"
        "    }\n"
        "    if (notify_drained && callbacks_.on_playback_drained) {\n"
        "        callbacks_.on_playback_drained();\n"
        "    }\n"
        "}\n"
        "\nvoid AudioService::EncodeWakeWord() {",
    )
    replace_once(
        source,
        "        audio_encode_queue_.clear();\n"
        "        audio_decode_queue_.clear();",
        "        pcm_bridge_enabled_.store(false);\n"
        "        pcm_playback_mode_.store(false);\n"
        "        audio_encode_queue_.clear();\n"
        "        pcm_input_queue_.clear();\n"
        "        audio_decode_queue_.clear();",
    )

    # Keep raw PCM speaker data when the application resets the Opus decoder on TTS start.
    replace_once(
        source,
        "        timestamp_queue_.clear();\n"
        "        audio_decode_queue_.clear();\n"
        "        audio_playback_queue_.clear();\n"
        "        audio_testing_queue_.clear();\n",
        "        timestamp_queue_.clear();\n"
        "        audio_decode_queue_.clear();\n"
        "        if (!pcm_playback_mode_.load()) {\n"
        "            audio_playback_queue_.clear();\n"
        "        }\n"
        "        audio_testing_queue_.clear();\n",
    )

patch_pcm_audio_bridge()

# Route PCM-ready notifications through the existing Application event loop.
def patch_pcm_application_event() -> None:
    application = UPSTREAM / "main" / "application.cc"
    replace_once(
        application,
        "    callbacks.on_send_queue_available = [this]() {\n"
        "        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);\n"
        "    };\n",
        "    callbacks.on_send_queue_available = [this]() {\n"
        "        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);\n"
        "    };\n"
        "    callbacks.on_pcm_input_available = [this]() {\n"
        "        xEventGroupSetBits(event_group_, MAIN_EVENT_PROTOCOL_PENDING);\n"
        "    };\n",
    )


patch_pcm_application_event()

print("ChatAI TPIOS board overlay applied.")
