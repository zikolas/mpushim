#!/bin/bash
# Build MPUSHIM.EXE (the unified V86+PM facade) - nasm assembles the
# embedded real-mode core natively, DJGPP compiles in a container.
# Mounts ONLY this project directory and the toolchain, read-only where it can.
#   DJGPP_DIR - DJGPP cross toolchain (i586-pc-msdosdjgpp-*), default ~/djgpp
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DJGPP_DIR="${DJGPP_DIR:-$HOME/djgpp}"
if [ ! -f "$HERE/MPUSHIM.C" ]; then
  echo "run from the mpushim repo (MPUSHIM.C not found)" >&2
  exit 1
fi

# the embedded V86 resident core: assemble flat, embed as a C byte array
nasm -f bin "$HERE/MPUSHIMR.ASM" -o "$HERE/MPUSHIMR.BIN"
( cd "$HERE" && xxd -i -n mpushimr_bin MPUSHIMR.BIN > mpushimr.h )

# the 16-bit protected-mode half: its own .COM, because only a 16-bit DPMI
# client can register with HDPMI16i (see MPUSHM16.ASM)
nasm -f bin "$HERE/MPUSHM16.ASM" -o "$HERE/MPUSHM16.COM"
echo "MPUSHM16.COM: $(wc -c < "$HERE/MPUSHM16.COM") bytes"

docker run --rm --platform linux/amd64 \
  -v "$HERE":/build -v "$DJGPP_DIR":/opt/djgpp:ro \
  -w /build debian:stable-slim bash -c '
    export PATH=/opt/djgpp/bin:$PATH
    i586-pc-msdosdjgpp-gcc -x c -Os -Wall -s -o MPUSHIM.EXE MPUSHIM.C
    ls -la MPUSHIM.EXE && echo "BUILD OK"
  '
