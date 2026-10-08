#!/usr/bin/env bash
#
# Pruebas de la sonda de QuemaOS de BigJPG (server/quemaos): arranca BigJPG de verdad (con el motor de pega), hace un
# trabajo, mide con él activo y después lo para para comprobar el «caído».
#
#   server/tests/quemaos.sh
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
LUX="${LUX:-$(dirname "$ROOT")/build/vendor/lux/lux}"
PORT="${SERVER_PORT:-5996}"
TMP="$(mktemp -d)"
PID=""
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; wait 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

mkdir -p "$TMP/data" "$TMP/engine"
cp -r "$HERE/fake-engine/." "$TMP/engine/"          # copia: una de las pruebas la mueve de sitio
python3 "$HERE/gen_png.py" 8 6 "$TMP/small.png"
export BIGJPG_DB="$TMP/bigjpg.db" BIGJPG_DATA_DIR="$TMP/data" BIGJPG_ENGINE_DIR="$TMP/engine" \
       BIGJPG_PORT="$PORT" BIGJPG_HOST=127.0.0.1 BIGJPG_URL="http://127.0.0.1:$PORT"
"$LUX" --no-watch "$ROOT/app" >"$TMP/server.log" 2>&1 &
PID=$!
for _ in $(seq 1 50); do curl -fs "$BIGJPG_URL/healthz" >/dev/null 2>&1 && break; sleep 0.2; done
curl -fs "$BIGJPG_URL/healthz" >/dev/null || { echo "BigJPG no arranca"; cat "$TMP/server.log"; exit 1; }

# Un trabajo hecho, para que haya algo que medir.
id=$(curl -s -H 'X-Requested-With: bigjpg' -F "file=@$TMP/small.png" "$BIGJPG_URL/api/jobs?model=anime&scale=4" | sed -E 's/.*"id":"([0-9a-f]+)".*/\1/')
for _ in $(seq 1 40); do curl -s "$BIGJPG_URL/api/jobs/$id" | grep -q '"status":"done"' && break; sleep 0.25; done
curl -s "$BIGJPG_URL/api/jobs/$id" | grep -q '"status":"done"' || { echo "el trabajo de prueba no terminó"; exit 1; }

status=0
echo "== con BigJPG activo"
"$LUX" test "$ROOT/quemaos" "$HERE/quemaos_test.lux" -- activo || status=1

echo "== con BigJPG parado"
kill "$PID"; wait "$PID" 2>/dev/null; PID=""
"$LUX" test "$ROOT/quemaos" "$HERE/quemaos_test.lux" -- caido || status=1

echo "== el lanzador impone el rango de la suite y arranca en un puerto bueno"
mkdir -p "$TMP/opt" && cp "$LUX" "$TMP/opt/lux" && cp -r "$ROOT/quemaos" "$TMP/opt/quemaos" && cp "$ROOT/deploy/quemaos-run.sh" "$TMP/opt/"
for bad in 8000 9699 9800 abc; do
  out=$(BIGJPG_QUEMAOS_PORT=$bad "$TMP/opt/quemaos-run.sh" 2>&1); code=$?
  if [ $code -ne 0 ] && echo "$out" | grep -q "entre 9700 y 9799"; then echo "  ok    rechaza $bad"; else echo "  FAIL  acepta $bad ($code): $out"; status=1; fi
done
BIGJPG_QUEMAOS_PORT=9792 "$TMP/opt/quemaos-run.sh" >"$TMP/probe.log" 2>&1 & PID=$!
for _ in $(seq 1 50); do curl -fs http://127.0.0.1:9792/quemaos/status >/dev/null 2>&1 && break; sleep 0.2; done
body=$(curl -s http://127.0.0.1:9792/quemaos/status)
echo "$body" | grep -q '"id":"bigjpg"' && echo "  ok    responde en 9792 (BigJPG parado: $(echo "$body" | sed -E 's/.*"status":"([a-z]+)".*/\1/'))" || { echo "  FAIL  sin respuesta: $body"; status=1; }
[ $status -eq 0 ] && echo "TODO CORRECTO" || echo "HAY FALLOS"
exit $status
