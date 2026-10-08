#!/usr/bin/env bash
#
# Todas las pruebas de BigJPG:
#   ./run-tests.sh
#
# Hace falta haber compilado Lux antes: cmake -S desktop -B build && cmake --build build --target lux-bin
# (todas usan ese binario y un motor de pega; no hace falta GPU ni Real-ESRGAN).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
status=0
echo "== Las copias de la interfaz (ui/) están al día"
for d in server/app/public desktop/app/public; do
    if diff -rq "$ROOT/ui" "$ROOT/$d" >/dev/null; then echo "  ok    $d"; else echo "  FAIL  $d no coincide con ui/ (ejecuta tools/sync-ui.sh)"; status=1; fi
done
node --check "$ROOT/ui/app.js" && echo "  ok    ui/app.js compila" || status=1
echo "== Servidor (API, cola, cancelación, limpieza)"
"$ROOT/server/tests/run.sh" || status=1
echo "== Sonda de QuemaOS"
"$ROOT/server/tests/quemaos.sh" || status=1
echo "== Escritorio contra un servidor real (local, servidor, errores, cancelación)"
"$ROOT/desktop/tests/e2e.sh" || status=1
[ $status -eq 0 ] && echo "TODO CORRECTO" || echo "HAY FALLOS"
exit $status
