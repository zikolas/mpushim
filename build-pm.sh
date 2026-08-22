#!/bin/bash
# Build MPUSHIMP.EXE (the protected-mode launcher) with DJGPP in a container.
# Mounts ONLY this project directory and the toolchain, read-only where it can.
#   DJGPP_DIR - DJGPP cross toolchain (i586-pc-msdosdjgpp-*), default ~/djgpp
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DJGPP_DIR="${DJGPP_DIR:-$HOME/djgpp}"
if [ ! -f "$HERE/MPUSHIMP.C" ]; then
  echo "run from the mpushim repo (MPUSHIMP.C not found)" >&2
  exit 1
fi
docker run --rm --platform linux/amd64 \
  -v "$HERE":/build -v "$DJGPP_DIR":/opt/djgpp:ro \
  -w /build debian:stable-slim bash -c '
    export PATH=/opt/djgpp/bin:$PATH
    i586-pc-msdosdjgpp-gcc -x c -Os -Wall -s -o MPUSHIMP.EXE MPUSHIMP.C
    ls -la MPUSHIMP.EXE && echo "BUILD OK"
  '
