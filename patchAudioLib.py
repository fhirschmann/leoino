# -*- coding: utf-8 -*-

"""PlatformIO pre-script that validates the pinned ESP32-audioI2S library.

The fork used to patch audioI2S 3.x to skip unconditional VU/FFT work and to
delay seek-time recomputation until the measured bitrate stabilized. audioI2S
4.0 implements both behaviours itself: VU and spectrum processing are guarded
by settings flags (disabled by default), and the average bitrate is not exposed
until its stability counter has settled. It also retains the corrected,
audio-block-relative read pointer after a seek.

No source rewrite is needed anymore. The checks below deliberately fail the
build if a later library update removes one of those guarantees, so the CPU and
resume regressions cannot silently return.
"""

import subprocess
import sys
from pathlib import Path

Import("env")  # pylint: disable=undefined-variable

REQUIRED_V4_GUARANTEES = [
    "if (settings.VU_LEVEL) calculateVUlevel(",
    "if (settings.SPECTRUM) calculateSpectrum(",
    "if (m_cab.brCounter > 10)",
    "m_avr_bitrate = calculate_average_bitrate(",
    "m_audioDataReadPtr = (resumeFilePos + offset) - m_audioDataStart;",
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


def validate_audio_library():
    path = audio_cpp_path()
    ensure_lib_installed(path)
    source = path.read_text(encoding="utf-8")

    def require(needle):
        if needle not in source:
            sys.stderr.write(
                f"patchAudioLib: expected code '{needle}' not found in {path}.\n"
                "The library changed - re-evaluate the VU/spectrum and seek "
                "guarantees, then update patchAudioLib.py.\n"
            )
            sys.exit(1)

    for guarantee in REQUIRED_V4_GUARANTEES:
        require(guarantee)

    print("patchAudioLib: validated ESP32-audioI2S v4 VU/spectrum gates and seek bookkeeping")


validate_audio_library()
