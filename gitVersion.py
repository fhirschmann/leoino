# -*- coding: utf-8 -*-

"""PlatformIO script to generate a gitversion.h file and adds its path to
   CPPPATH.

Note: The PlatformIO documentation suggests to simply add the Git version as
dynamic build flag (=define). This however leads to constant rebuilds of the
complete project.
"""

import subprocess
from pathlib import Path

Import("env")  # pylint: disable=undefined-variable


OUTPUT_PATH = (
    Path(env.subst("$BUILD_DIR")) / "generated" / "gitrevision.h"
)  # pylint: disable=undefined-variable

TEMPLATE = """
#ifndef __GIT_REVISION_H__
    #define __GIT_REVISION_H__
    constexpr const char gitRevision[] = "Git-revision: {git_revision}";
    constexpr const char gitRevShort[] = "\\"{git_revision}\\"";
    constexpr const char buildRevision[] = "{build_revision}";
#endif
"""


def git_revision():
    """Returns Git revision or unknown."""
    try:
        return subprocess.check_output(
            ["git", "describe", "--always", "--dirty"],
            text=True,
            stderr=subprocess.PIPE,
        ).strip()
    except (subprocess.CalledProcessError, OSError) as err:
        # OSError (e.g. git not installed) has no .stderr; fall back to str(err)
        detail = getattr(err, "stderr", None) or str(err)
        print(f"  Warning: Setting Git revision to 'unknown': {detail}")
        return "unknown"


def build_revision():
    """Returns a rolling build revision like 'r249' (commit count), '-dirty' if applicable."""
    try:
        count = subprocess.check_output(
            ["git", "rev-list", "--count", "HEAD"],
            text=True,
            stderr=subprocess.PIPE,
        ).strip()
        dirty = subprocess.run(["git", "diff", "--quiet"]).returncode != 0
        return f"r{count}{'-dirty' if dirty else ''}"
    except (subprocess.CalledProcessError, OSError):
        return "r0"


def generate():
    """Generates header file."""
    print("GENERATING GIT REVISION HEADER FILE")
    gitrev = git_revision()
    buildrev = build_revision()
    print(f'  "{gitrev}" ({buildrev}) -> {OUTPUT_PATH}')
    OUTPUT_PATH.parent.mkdir(exist_ok=True, parents=True)
    with OUTPUT_PATH.open("w") as output_file:
        output_file.write(
            TEMPLATE.format(git_revision=gitrev, build_revision=buildrev)
        )


generate()
env.Append(CPPPATH=OUTPUT_PATH.parent)  # pylint: disable=undefined-variable
