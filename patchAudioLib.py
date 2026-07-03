# -*- coding: utf-8 -*-

"""PlatformIO pre-script that patches the ESP32-audioI2S library after it has
been fetched into .pio/libdeps.

1. Unused VU/FFT processing: the library unconditionally runs a VU envelope
   follower for every decoded sample (PSRAM delay lines + float AGC math,
   44100x/s) and a spectrum FFT per chunk. ESPuino consumes neither. Skipping
   both frees ~18% of core 1 during playback (measured on the complete board),
   headroom that prevents audio drop-outs while the webserver pushes big
   transfers over the shared SPI bus.

2. Seek bookkeeping: after a resume/seek the library sets m_audioDataReadPtr
   to the ABSOLUTE file position (including the ID3 header offset), but the
   end-of-file check compares it against m_audioDataSize, which is RELATIVE
   to the audio block. On files with a large ID3 tag (Tonie music albums
   carry ~135 KB embedded covers) every resumed track therefore ended
   id3size/bitrate (~5-6 s) before its real end. A normal (non-seek) start
   keeps the pointer relative, so only seeks were affected; the patch makes
   the seek path consistent with that.

The patches are applied idempotently. If the library source changes so the
expected lines are no longer found, the build aborts loudly so a patch
cannot silently disappear — re-evaluate it against the new library version
(ideally both get fixed upstream one day).
"""

import subprocess
import sys
from pathlib import Path

Import("env")  # pylint: disable=undefined-variable

MARKER = "patched by patchAudioLib.py"
COMMENT_OUT = [
    "calculateVUlevel(&m_outBuff[i * 2]);",
    "processSpectrum();",
]
REPLACE = [
    # After a seek the playtime is recomputed from the new file position and the AVERAGE
    # bitrate. In the first ~1.5 s of a track that average is still swinging in (the very
    # first measurement bursts make it huge), so an early seek recomputed the time as ~0
    # and the whole progress display, the resume-settle detection and subsequent position
    # saves ran on a wrong clock. Wait for the library's own "bitrate stable" signal; the
    # pending m_haveNewFilePos simply stays queued until then (files with a Xing/Info
    # header use the nominal bitrate and are unaffected).
    (
        "if (m_haveNewFilePos && (m_cat.avrBitRate || m_cat.nominalBitRate)) {",
        "if (m_haveNewFilePos && (m_cat.avrBitrateStable || m_cat.nominalBitRate)) { // patched by patchAudioLib.py: recompute only with a stable avg bitrate",
    ),
    # newInBuffStart() returns 0 (seek rejected) or -1 (seek failed) without moving the
    # file; subtracting the header offset from those sentinels would wrap the unsigned
    # read pointer and fire an instant EOF, so only genuine positions are translated.
    (
        "m_audioDataReadPtr = m_prlf.newFilePos;",
        "m_audioDataReadPtr = (m_prlf.newFilePos > (int32_t) m_audioDataStart) ? (m_prlf.newFilePos - m_audioDataStart) : 0; // patched by patchAudioLib.py: keep read pointer relative to the audio block (EOF fired id3-size early after a seek)",
    ),
    (
        "m_audioDataReadPtr = m_pwf.newFilePos;",
        "m_audioDataReadPtr = (m_pwf.newFilePos > (int32_t) m_audioDataStart) ? (m_pwf.newFilePos - m_audioDataStart) : 0; // patched by patchAudioLib.py: see above",
    ),
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

    def require(needle):
        if needle not in source:
            sys.stderr.write(
                f"patchAudioLib: expected code '{needle}' not found in {path}.\n"
                "The library changed - re-evaluate whether this patch is still "
                "needed/correct, then update patchAudioLib.py.\n"
            )
            sys.exit(1)

    for call in COMMENT_OUT:
        require(call)
        source = source.replace(call, f"// patched by patchAudioLib.py (unused by ESPuino): {call}")
    for old, new in REPLACE:
        require(old)
        source = source.replace(old, new)

    path.write_text(source, encoding="utf-8")
    print("patchAudioLib: patched ESP32-audioI2S (VU/FFT off, seek read-pointer fix)")


apply_patch()
