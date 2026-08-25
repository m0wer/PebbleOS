#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

"""Build the Moddable host toolchain and run mcconfig.

Builds the host-side Moddable tools (xsc, xsl, mcconfig, ...) for the
current platform, then runs mcconfig against the Pebble host manifest
to generate mc.xs.c and mc.resources.c. These files are then linked
into libxs by the firmware build.

Idempotent: make and mcconfig handle staleness internally, so running
this when nothing has changed is fast.

Usage:
    python tools/build_moddable_host.py
"""

import os
import pathlib
import subprocess
import sys


def platform_subdir():
    if sys.platform == "darwin":
        return "mac"
    if sys.platform.startswith("linux"):
        return "lin"
    if sys.platform.startswith("win"):
        return "win"
    raise SystemExit(f"Unsupported platform: {sys.platform}")


def build_tools(makefiles_dir, moddable_path, pdir, env):
    if pdir != "lin":
        subprocess.run(["make"], cwd=str(makefiles_dir), env=env, check=True)
        return

    # The aggregate Linux target also builds GTK debugger applications.
    makefiles = [
        moddable_path / "xs" / "makefiles" / "lin" / "xsc.mk",
        moddable_path / "xs" / "makefiles" / "lin" / "xsid.mk",
        moddable_path / "xs" / "makefiles" / "lin" / "xsl.mk",
        makefiles_dir / "tools.mk",
    ]
    for makefile in makefiles:
        subprocess.run(
            ["make", "GOAL=release", "-f", str(makefile)],
            cwd=str(makefiles_dir),
            env=env,
            check=True,
        )


def main():
    repo_root = pathlib.Path(__file__).resolve().parent.parent
    moddable_path = repo_root / "third_party" / "moddable" / "moddable"
    if not moddable_path.is_dir():
        raise SystemExit(
            f"Moddable submodule not found at {moddable_path}. "
            "Did you forget `git submodule update --init`?"
        )

    pdir = platform_subdir()
    tools_path = moddable_path / "build" / "bin" / pdir / "release"

    env = os.environ.copy()
    env["MODDABLE"] = str(moddable_path)
    env["XS_DIR"] = str(moddable_path / "xs")
    env["BUILD_DIR"] = str(moddable_path / "build")
    env["PATH"] = str(tools_path) + os.pathsep + env.get("PATH", "")

    makefiles_dir = moddable_path / "build" / "makefiles" / pdir
    print(f"Building Moddable tools in {makefiles_dir}...", flush=True)
    build_tools(makefiles_dir, moddable_path, pdir, env)

    host_dir = moddable_path / "build" / "devices" / "pebble" / "host"
    print(f"Compiling Moddable host app in {host_dir}...", flush=True)
    subprocess.run(
        ["mcconfig", "-m", "-p", "pebble"],
        cwd=str(host_dir),
        env=env,
        check=True,
    )


if __name__ == "__main__":
    main()
