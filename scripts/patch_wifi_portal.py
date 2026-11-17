#!/usr/bin/env python3
"""Install the ChatAI page into the pinned esp-wifi-connect component before compilation."""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "xiaozhi-src"
SOURCE = ROOT / "firmware" / "board" / "chatai-tpios" / "web_admin.html"


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


if __name__ == "__main__":
    main()
