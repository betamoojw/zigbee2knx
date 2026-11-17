#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Dirk Tostmann
#
# Builds knxgw-berry, the Berry VM of the firmware as a program for the PC. The template tests (tests/) run every
# template in it, with the host API of berry/prelude.be.
#   berry/build_hostvm.sh            -> build/hostvm/knxgw-berry
#   OUT=/some/dir CC=clang berry/build_hostvm.sh
# The Berry sources and berry_conf.h are the firmware's (firmware/components/berry); only the port functions differ:
# knxgw_berry.c carries them, be_port_idf.c is left out.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BERRY="$ROOT/firmware/components/berry"
OUT=${OUT:-$ROOT/build/hostvm}
CC=${CC:-cc}

mkdir -p "$OUT"
REV=$(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo unknown)

SRCS=()
for f in "$BERRY"/src/*.c; do
    [ "$(basename "$f")" = be_port_idf.c ] || SRCS+=("$f")
done

"$CC" -std=gnu99 -O2 ${CFLAGS:-} -DKNXGW_REV="\"$REV\"" -I"$BERRY/src" -I"$BERRY/include" \
    -Wall -Wno-unused-parameter -Wno-implicit-fallthrough -Wno-discarded-qualifiers -Wno-array-bounds \
    -Wno-unused-but-set-variable -Wno-unused-variable -Wno-format -Wno-char-subscripts \
    "$HERE/knxgw_berry.c" "${SRCS[@]}" -lm -o "$OUT/knxgw-berry.tmp"
mv "$OUT/knxgw-berry.tmp" "$OUT/knxgw-berry"
echo "knxgw-berry $REV -> $OUT/knxgw-berry"
