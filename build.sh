#!/usr/bin/env bash
# Cross-compile Foyer.exe from Linux/macOS with Zig (pip install ziglang).
set -euo pipefail
cd "$(dirname "$0")"
ZIG="${ZIG:-python3 -m ziglang}"
mkdir -p dist
$ZIG c++ -target x86_64-windows-gnu -std=c++17 -O2 -s -municode \
  -DUNICODE -D_UNICODE \
  -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers \
  src/*.cpp res/foyer.rc -o dist/Foyer.exe \
  -Wl,--subsystem,windows \
  -ld2d1 -ldwrite -lole32 -loleaut32 -luuid -ldwmapi -lshcore -lpowrprof -lwtsapi32 -lsecur32 -lshell32 -luser32 -lgdi32 -ladvapi32 -lshlwapi
rm -f dist/Foyer.pdb
ls -la dist/Foyer.exe
