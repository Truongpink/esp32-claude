#!/usr/bin/env python3
"""Fetch the exact XiaoZhi revision and apply the ChatAI TPIOS overlay."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "xiaozhi-src"
REPOSITORY = "https://github.com/78/xiaozhi-esp32.git"
PINNED_COMMIT = "4de00aed740595a8713b01f8332b9b2cf30685bc"


def run(*args: str, cwd: Path | None = None) -> str:
    result = subprocess.run(
        args, cwd=cwd, check=True, text=True, stdout=subprocess.PIPE
    )
    return result.stdout.strip()


def main() -> None:
    if not (UPSTREAM / ".git").exists():
        if UPSTREAM.exists() and any(UPSTREAM.iterdir()):
            raise SystemExit(
                f"{UPSTREAM} already exists but is not a Git checkout; "
                "move it aside rather than overwriting user files."
            )
        UPSTREAM.mkdir(parents=True, exist_ok=True)
        run("git", "init", cwd=UPSTREAM)
        run("git", "remote", "add", "origin", REPOSITORY, cwd=UPSTREAM)
        run("git", "fetch", "--depth", "1", "origin", PINNED_COMMIT, cwd=UPSTREAM)
        run("git", "checkout", "--detach", "FETCH_HEAD", cwd=UPSTREAM)

    actual = run("git", "rev-parse", "HEAD", cwd=UPSTREAM)
    if actual != PINNED_COMMIT:
        raise SystemExit(
            f"Unexpected upstream revision: {actual}. Expected {PINNED_COMMIT}. "
            "No reset or overwrite was attempted."
        )

    subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "apply_upstream_patches.py")],
        cwd=ROOT,
        check=True,
    )
    print(f"Prepared XiaoZhi source at {PINNED_COMMIT}.")
    print("Next: cd xiaozhi-src && idf.py set-target esp32s3 && idf.py build")


if __name__ == "__main__":
    main()
