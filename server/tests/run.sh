#!/usr/bin/env bash
#
# Pruebas del servidor de BigJPG, con un motor de pega (fake-engine/) en vez de Real-ESRGAN.
#
#   server/tests/run.sh [filtro]
#
# Variables: LUX (binario de Lux; por defecto el que compila CMake en ../../build).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
LUX="${LUX:-$(dirname "$ROOT")/build/vendor/lux/lux}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/files" "$TMP/data"
python3 "$HERE/gen_png.py" 8 6 "$TMP/files/small.png"
python3 "$HERE/gen_png.py" 40 40 "$TMP/files/big.png"
echo "esto no es una imagen" > "$TMP/files/notimage.txt"

export BIGJPG_DB="$TMP/bigjpg.db" BIGJPG_DATA_DIR="$TMP/data" BIGJPG_ENGINE_DIR="$HERE/fake-engine" \
       BIGJPG_MAX_PIXELS=1000 BIGJPG_TEST_FILES="$TMP/files" BIGJPG_KEEP_HOURS=6
"$LUX" test "$ROOT/app" "$HERE/api_test.lux" ${1:+-- "$1"}
