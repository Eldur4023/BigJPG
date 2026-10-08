#!/usr/bin/env bash
#
# Copia la interfaz (ui/) a las dos apps que la sirven: la del servidor y la de la ventana de escritorio.
# Es la MISMA interfaz: detecta el modo con /api/info. Tras tocar ui/, ejecútalo; tests/ui-sync.sh comprueba
# que las copias no se han quedado atrás.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for dest in server/app/public desktop/app/public; do
    mkdir -p "$ROOT/$dest"
    rsync -a --delete "$ROOT/ui/" "$ROOT/$dest/"
done
echo "ui/ copiada a server/app/public y desktop/app/public"
