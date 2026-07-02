# -*- coding: utf-8 -*-

"""PlatformIO pre-script that patches unused VU/FFT processing out of the
ESP32-audioI2S library after it has been fetched into .pio/libdeps.

The library unconditionally runs a VU envelope follower for every decoded
sample (PSRAM delay lines + float AGC math, 44100x/s) and a spectrum FFT per
chunk. ESPuino consumes neither. Skipping both frees ~18% of core 1 during
playback (measured on the complete board), headroom that prevents audio
drop-outs while the webserver pushes big transfers over the shared SPI bus.

The patch is applied idempotently. If the library source changes so the
expected lines are no longer found, the build aborts loudly so the patch
cannot silently disappear — re-evaluate it against the new library version
(ideally it becomes a runtime flag upstream one day).
"""

import subprocess
import sys
from pathlib import Path

Import("env")  # pylint: disable=undefined-variable

MARKER = "// patched out by patchAudioLib.py (unused by ESPuino):"
PATCHES = [
    "calculateVUlevel(&m_outBuff[i * 2]);",
    "processSpectrum();",
]


def audio_cpp_path():
    libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV")
    return libdeps / "ESP32-audioI2S" / "src" / "Audio.cpp"


def ensure_lib_installed(path):
    """On a fresh checkout the pre-script runs before the library fetch."""
    if path.is_file():
        return
    print("patchAudioLib: ESP32-audioI2S not fetched yet, installing lib deps...")
    subprocess.check_call(
        [env.subst("$PYTHONEXE"), "-m", "platformio", "pkg", "install",
         "-e", env.subst("$PIOENV")],
        cwd=env.subst("$PROJECT_DIR"),
    )
    if not path.is_file():
        sys.stderr.write("patchAudioLib: ESP32-audioI2S/src/Audio.cpp not found after install\n")
        sys.exit(1)


def apply_patch():
    path = audio_cpp_path()
    ensure_lib_installed(path)
    source = path.read_text(encoding="utf-8")

    if MARKER in source:
        return  # already patched

    for call in PATCHES:
        if call not in source:
            sys.stderr.write(
                f"patchAudioLib: expected call '{call}' not found in {path}.\n"
                "The library changed - re-evaluate whether this patch is still "
                "needed/correct, then update patchAudioLib.py.\n"
            )
            sys.exit(1)
        source = source.replace(call, f"{MARKER} {call}")

    path.write_text(source, encoding="utf-8")
    print("patchAudioLib: disabled unused VU/FFT processing in ESP32-audioI2S")


apply_patch()
