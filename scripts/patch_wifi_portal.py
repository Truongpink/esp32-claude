#!/usr/bin/env python3
"""Install the ChatAI page into the pinned esp-wifi-connect component before compilation."""
from pathlib import Path
import shutil
import re

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "xiaozhi-src"
SOURCE = ROOT / "firmware" / "board" / "chatai-tpios" / "web_admin.html"


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"Expected one patch anchor in {path}: found {count}: {old[:100]!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def main() -> None:
    if not SOURCE.is_file():
        raise SystemExit(f"ChatAI Web Admin page not found: {SOURCE}")

    managed = UPSTREAM / "managed_components"
    candidates = []
    if managed.is_dir():
        for source in managed.glob("*/wifi_configuration_ap.cc"):
            manifest = source.parent / "idf_component.yml"
            if not manifest.is_file():
                continue
            metadata = manifest.read_text(encoding="utf-8")
            if "version: 3.3.3" in metadata and "78/esp-wifi-connect" in metadata:
                candidates.append(source)

    if len(candidates) != 1:
        raise SystemExit(
            "Expected exactly one managed 78/esp-wifi-connect v3.3.3 component; "
            f"found {len(candidates)}. No files were changed."
        )

    component = candidates[0].parent
    source_code = candidates[0].read_text(encoding="utf-8")
    required_routes = ('"/scan"', '"/submit"', '"/exit"')
    if "index_html_start" not in source_code or any(route not in source_code for route in required_routes):
        raise SystemExit(
            "The Wi-Fi component does not match the verified v3.3.3 portal layout. "
            "No files were changed."
        )

    destination = component / "assets" / "wifi_configuration.html"
    if not destination.is_file():
        raise SystemExit(f"Expected embedded portal asset not found: {destination}")

    shutil.copyfile(SOURCE, destination)
    print(f"Installed ChatAI Web Admin page into {component.name} (v3.3.3).")
    print("Wi-Fi provisioning handlers (/scan, /submit, /exit) remain unchanged.")

    # The WSS receive task executes the Gemini JSON/PCM callback. Give it
    # bounded stack and read-buffer headroom, and reserve the WebSocket parser
    # buffers requested by GeminiLiveProtocol.
    network_sources = []
    for source in managed.glob("*/src/esp/esp_ssl.cc"):
        component = source.parents[2]
        manifest = component / "idf_component.yml"
        if not manifest.is_file():
            continue
        metadata = manifest.read_text(encoding="utf-8")
        if "78/esp-ml307" in metadata or "github.com/78/esp-ml307" in metadata:
            if re.search(r"version:\s*3\.8\.\d+", metadata):
                network_sources.append(source)

    if len(network_sources) != 1:
        raise SystemExit(
            "Expected exactly one managed 78/esp-ml307 3.8.x component; "
            f"found {len(network_sources)}. No network files were changed."
        )

    ssl_source = network_sources[0]
    ssl_header = ssl_source.parent / "esp_ssl.h"
    websocket_source = ssl_source.parents[2] / "src" / "web_socket.cc"
    websocket_header = ssl_source.parents[2] / "include" / "web_socket.h"
    for required in (ssl_header, websocket_source, websocket_header):
        if not required.is_file():
            raise SystemExit(f"Expected esp-ml307 source file not found: {required}")

    replace_once(
        ssl_source,
        "#include <esp_log.h>\n",
        "#include <esp_log.h>\n#include <esp_heap_caps.h>\n",
    )
    replace_once(
        ssl_source,
        '    BaseType_t rc = xTaskCreate([](void* arg) {',
        '    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 16384 + 10000 ||\n'
        '        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 16384) {\n'
        '        ESP_LOGE(TAG, "Low heap, skip SSL receive task");\n'
        '        connected_ = false;\n'
        '        destroy_tls();\n'
        '        return Fail(NetworkError::FromErrno(ENOMEM));\n'
        '    }\n'
        '    BaseType_t rc = xTaskCreate([](void* arg) {',
    )
    replace_once(
        ssl_source,
        '}, "ssl_receive", 4096, this, 1, &receive_task_handle_);',
        '}, "ssl_receive", 16384, this, 1, &receive_task_handle_);',
    )
    replace_once(
        ssl_source,
        "        data.resize(1500);",
        "        data.resize(16384);",
    )
    replace_once(
        websocket_source,
        "void WebSocket::SetReceiveBufferSize(size_t size) {\n"
        "    receive_buffer_size_ = size;\n"
        "}",
        "void WebSocket::SetReceiveBufferSize(size_t size) {\n"
        "    if (size == 0) size = 2048;\n"
        "    receive_buffer_size_ = size > MAX_MESSAGE_SIZE ? MAX_MESSAGE_SIZE : size;\n"
        "    receive_buffer_.reserve(receive_buffer_size_);\n"
        "    current_message_.reserve(receive_buffer_size_);\n"
        "}",
    )
    print(f"Configured Gemini WSS receive buffers and stack in {ssl_source.parents[2].name}.")


if __name__ == "__main__":
    main()
