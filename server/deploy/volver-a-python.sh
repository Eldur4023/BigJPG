#!/usr/bin/env bash
#
# Vuelve a la versión antigua de BigJPG (Python), si todavía no has ejecutado limpiar-python.sh: restaura su código,
# su configuración y su unidad desde /root/bigjpg-python-*.tar.gz.
#
#   sudo bash deploy/volver-a-python.sh
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "Ejecútalo como root (sudo)." >&2; exit 1; }
BK="$(ls -1t /root/bigjpg-python-*.tar.gz 2>/dev/null | head -1)"
[[ -n "$BK" ]] || { echo "No hay copia /root/bigjpg-python-*.tar.gz." >&2; exit 1; }
[[ -d /opt/bigjpg/venv ]] || { echo "El entorno virtual ya no está (se ejecutó limpiar-python.sh): no se puede volver." >&2; exit 1; }
systemctl stop bigjpg bigjpg-quemaos 2>/dev/null || true
rm -rf /opt/bigjpg/app
tar xzf "$BK" -C /
systemctl daemon-reload
systemctl start bigjpg
echo "Restaurada la versión Python desde $BK"
