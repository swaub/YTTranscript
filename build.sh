#!/usr/bin/env bash
#
# WSL cross-build for YTTranscript.exe (mingw-w64 -> Windows x86_64).
#
# Prerequisite (one time):
#   sudo apt-get install -y gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64
#
# Produces: dist/YTTranscript.exe
#
set -euo pipefail

CC=x86_64-w64-mingw32-gcc
WINDRES=x86_64-w64-mingw32-windres

CFLAGS=(
  -std=c11 -municode -mwindows -static -O2
  -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00
  -Wall -Wextra
)

SRCS=(
  src/main.c src/ui.c src/util.c src/proc.c src/pipeline.c src/strutil.c
  src/summarize.c src/download.c src/bootstrap.c src/clipboard.c src/hwdetect.c
  src/settings.c src/progressbar.c
)

LDLIBS=(
  -lcomctl32 -lwinhttp -lshlwapi -ldxgi -ldxguid
  -luser32 -lgdi32 -lkernel32
)

mkdir -p dist

# -I res so the bare "icon.ico"/"clipboard.ico"/"app.manifest" names in
# res/app.rc resolve relative to the res/ directory.
"$WINDRES" -c 65001 -I res res/app.rc -O coff -o dist/app.res

"$CC" "${CFLAGS[@]}" \
  -o dist/YTTranscript.exe \
  "${SRCS[@]}" \
  dist/app.res \
  "${LDLIBS[@]}"

echo "BUILT dist/YTTranscript.exe"
