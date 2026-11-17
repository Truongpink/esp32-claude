#!/usr/bin/env python3
"""Apply the ChatAI TPIOS board overlay to the pinned XiaoZhi source tree."""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = Path("xiaozhi-src")
BOARD_SRC = ROOT / "firmware" / "board" / "chatai-tpios"
BOARD_DST = UPSTREAM / "main" / "boards" / "chatai-tpios"

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

shutil.copytree(BOARD_SRC, BOARD_DST, dirs_exist_ok=True)

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

cmake = UPSTREAM / "main" / "CMakeLists.txt"
replace_once(
    cmake,
    "if(CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI)\n",
    'if(CONFIG_BOARD_TYPE_CHAT_AI_TPIOS)\n'
    '    set(BOARD_DIR "chatai-tpios")\n'
    '    set(BUILTIN_TEXT_FONT font_noto_sans_basic_14_1)\n'
    '    set(BUILTIN_ICON_FONT font_material_symbols_14_1)\n'
    'elseif(CONFIG_BOARD_TYPE_BREAD_COMPACT_WIFI)\n',
)


# Add a bounded raw-PCM bridge for Gemini Live while preserving the default Opus path.
def patch_pcm_audio_bridge() -> None:
    header = UPSTREAM / "main" / "audio" / "audio_service.h"
    source = UPSTREAM / "main" / "audio" / "audio_service.cc"

    replace_once(
        header,
        "    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();\n",
        "    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();\n"
        "    // Optional raw PCM bridge for non-Opus transports (Gemini Live).\n"
        "    // Disabled by default; the existing XiaoZhi Opus path remains unchanged.\n"
        "    void EnablePcmBridge(bool enable);\n"
        "    bool PopPcmInput(std::vector<int16_t>& pcm);\n"
        "    bool PushPcmPlayback(std::vector<int16_t>&& pcm);\n"
        "    void ClearPcmPlayback();\n",
    )
    replace_once(
        header,
        "    FixedQueue<AudioTask, MAX_PLAYBACK_TASKS_IN_QUEUE> audio_playback_queue_;\n",
        "    FixedQueue<AudioTask, MAX_PLAYBACK_TASKS_IN_QUEUE> audio_playback_queue_;\n"
        "    // Four 60 ms mono PCM frames bound memory while tolerating short network stalls.\n"
        "    FixedQueue<std::vector<int16_t>, 4> pcm_input_queue_;\n"
        "    std::atomic<bool> pcm_bridge_enabled_{false};\n",
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
        "            return;\n"
        "        }\n"
        "        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));\n"
        "    });",
    )
    replace_once(
        source,
        "void AudioService::EncodeWakeWord() {",
        "void AudioService::EnablePcmBridge(bool enable) {\n"
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
        "    std::lock_guard<std::mutex> lock(audio_queue_mutex_);\n"
        "    audio_playback_queue_.clear();\n"
        "    audio_queue_cv_.notify_all();\n"
        "}\n"
        "\nvoid AudioService::EncodeWakeWord() {",
    )
    replace_once(
        source,
        "        audio_encode_queue_.clear();\n"
        "        audio_decode_queue_.clear();",
        "        audio_encode_queue_.clear();\n"
        "        pcm_input_queue_.clear();\n"
        "        audio_decode_queue_.clear();",
    )

patch_pcm_audio_bridge()

print("ChatAI TPIOS board overlay applied.")
