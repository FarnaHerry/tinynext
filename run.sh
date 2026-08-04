#!/usr/bin/env bash
# run.sh — launch tinynext under the SYSTEM glibc loader.
#
# Why: the mcpp toolchain links the binary against its private glibc 2.39
# (PT_INTERP points at xim-x-glibc/2.39/ld-linux), but the only usable
# OpenGL/GLX stack on this machine is the system Mesa, which requires
# GLIBC_2.43. Loading system Mesa into a 2.39 process fails with
# "version `GLIBC_2.43' not found". Running through the system ld.so with
# /usr/lib64 ahead of RUNPATH loads system glibc 2.43 + system Mesa, and the
# bundled X11 libs (built against 2.39) are forward-compatible — so both
# sides work. (`mcpp run` uses the mcpp interp and exits silently with -1.)
set -euo pipefail
cd "$(dirname "$0")"

BIN=$(ls -dt target/x86_64-linux-gnu/*/bin 2>/dev/null | head -1)
if [ -z "$BIN" ] || [ ! -x "$BIN/tinynext" ]; then
    echo "binary not found — run \`mcpp build\` first" >&2
    exit 1
fi

exec /lib64/ld-linux-x86-64.so.2 --library-path "/usr/lib64:$PWD/$BIN" "$PWD/$BIN/tinynext" "$@"
