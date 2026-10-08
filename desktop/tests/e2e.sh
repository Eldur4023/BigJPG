#!/usr/bin/env bash
#
# Extremo a extremo de la app de escritorio: arranca un servidor de BigJPG de verdad (con el motor de pega de
# server/tests/fake-engine) y lanza desktop_test.lux contra la app de escritorio.
#
#   desktop/tests/e2e.sh [filtro]
#
# Variables: LUX (binario de Lux; por defecto el de ../../build), SERVER_PORT (5992).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$(dirname "$HERE")")"
LUX="${LUX:-$ROOT/build/vendor/lux/lux}"
PORT="${SERVER_PORT:-5992}"
TMP="$(mktemp -d)"
PID=""
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; wait 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

mkdir -p "$TMP/files" "$TMP/srv-data" "$TMP/dsk-data"
python3 "$ROOT/server/tests/gen_png.py" 8 6 "$TMP/files/small.png"
python3 "$ROOT/server/tests/gen_png.py" 40 40 "$TMP/files/big.png"
echo "esto no es una imagen" > "$TMP/files/notimage.txt"

# El servidor.
BIGJPG_DB="$TMP/srv.db" BIGJPG_DATA_DIR="$TMP/srv-data" BIGJPG_ENGINE_DIR="$ROOT/server/tests/fake-engine" \
BIGJPG_PORT="$PORT" BIGJPG_HOST=127.0.0.1 BIGJPG_MAX_PIXELS=1000 \
    "$LUX" --no-watch "$ROOT/server/app" >"$TMP/server.log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -fs "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1 && break; sleep 0.2; done
curl -fs "http://127.0.0.1:$PORT/healthz" >/dev/null || { echo "el servidor no arranca"; cat "$TMP/server.log"; exit 1; }

# La app de escritorio bajo prueba.
export BIGJPG_DB="$TMP/dsk.db" BIGJPG_DATA_DIR="$TMP/dsk-data" BIGJPG_ENGINE_DIR="$ROOT/server/tests/fake-engine" \
       BIGJPG_MAX_PIXELS=1000 BIGJPG_TEST_FILES="$TMP/files" BIGJPG_TEST_SERVER="http://127.0.0.1:$PORT" BIGJPG_TEST_SERVER_DATA="$TMP/srv-data"
"$LUX" test "$ROOT/desktop/app" "$HERE/desktop_test.lux" ${1:+-- "$1"}
status=$?
[ $status -eq 0 ] || { echo "--- registro del servidor ---"; tail -n 12 "$TMP/server.log"; }
exit $status
