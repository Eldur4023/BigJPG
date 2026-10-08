#!/usr/bin/env bash
#
# Quita lo que dejó la versión antigua de BigJPG (Python + PyTorch): el entorno virtual, el intérprete, los pesos de
# los modelos y las subidas viejas. Se ejecuta a mano, cuando la versión nueva ya te sirve.
#
#   sudo bash deploy/limpiar-python.sh
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "Ejecútalo como root (sudo)." >&2; exit 1; }
curl -fsS --max-time 5 "http://$(grep -E '^BIGJPG_HOST=' /etc/bigjpg/bigjpg.env | cut -d= -f2):5002/healthz" >/dev/null \
    || { echo "BigJPG nuevo no responde: no se quita nada." >&2; exit 1; }
du -sh /opt/bigjpg/venv /opt/bigjpg/python /var/lib/bigjpg/weights 2>/dev/null || true
rm -rf /opt/bigjpg/venv /opt/bigjpg/python /var/lib/bigjpg/weights /var/lib/bigjpg/data/uploads /var/lib/bigjpg/data/results
rm -f /etc/bigjpg/bigjpg.env.python
echo "Hecho. (La copia del código antiguo sigue en /root/bigjpg-python-*.tar.gz; el repositorio tiene la etiqueta python-final.)"
