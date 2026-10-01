#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build dist
x86_64-w64-mingw32-windres src/resources.rc -O coff -o build/resources.o
x86_64-w64-mingw32-g++ src/main.cpp build/resources.o -o dist/DesktopPrank.exe \
  -std=c++17 -O2 -Wall -Wextra -Wpedantic -municode -mwindows -static -static-libgcc -static-libstdc++ \
  -lole32 -loleaut32 -lshell32 -lshlwapi -luuid -lwinmm -lcomdlg32 -lgdi32
x86_64-w64-mingw32-objdump -p dist/DesktopPrank.exe | sed -n '/DLL Name:/p'
