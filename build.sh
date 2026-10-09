#!/usr/bin/env bash
# Build dist/SuitPatcher.exe (Windows x64 GUI, also runs under Wine/Proton) and, with --test, the developer test tool.
# Needs zig (~/Tools/zig-*) to cross-compile; set ZIG=/path/to/zig otherwise.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
zig="${ZIG:-$(ls -d "$HOME"/Tools/zig-linux-x86_64-*/zig | tail -1)}"
mkdir -p "$here/dist"
common=(util.c zlib.c pak.c oodle.c json.c registry.c upkg.c game.c colors.c patcher.c)
flags=(-target x86_64-windows-gnu -O2 -Wall -Wno-unused-function -municode)
libs=(-lcomctl32 -lcomdlg32 -lshell32 -lole32 -ladvapi32 -luser32 -lgdi32)
cd "$here/src"
"$zig" cc "${flags[@]}" -s -Wl,--subsystem,windows -o "$here/dist/SuitPatcher.exe" gui.c "${common[@]}" ../res/app.rc "${libs[@]}"
if [ "${1:-}" = "--test" ]; then
  "$zig" cc "${flags[@]/-municode}" -o "$here/dist/testcli.exe" testcli.c "${common[@]}" "${libs[@]}"
fi
rm -f "$here"/dist/*.pdb "$here"/dist/*.lib
ls -la "$here"/dist/*.exe
