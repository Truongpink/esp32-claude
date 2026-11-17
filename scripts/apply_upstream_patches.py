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

print("ChatAI TPIOS board overlay applied.")
