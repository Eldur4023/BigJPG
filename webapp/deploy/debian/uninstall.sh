#!/usr/bin/env bash
#
# Desinstala el servicio systemd de BigJPG local instalado por install.sh.
#
set -euo pipefail

print_help() {
  cat <<'HELPEOF'
Uso: sudo ./uninstall.sh [opciones]

Opciones:
  --install-dir DIR   Debe coincidir con el usado en install.sh
                       (por defecto: /opt/bigjpg)
  --data-dir DIR      Debe coincidir con el usado en install.sh
                       (por defecto: /var/lib/bigjpg)
  --user USUARIO      Debe coincidir con el usado en install.sh
                       (por defecto: bigjpg)
  --purge             Tambien borra los datos: subidas, resultados y los
                       modelos ya descargados (sin esto se conservan, por
                       si vuelves a instalar mas adelante)
  -h, --help          Muestra esta ayuda
HELPEOF
}

INSTALL_DIR=/opt/bigjpg
DATA_DIR=/var/lib/bigjpg
SERVICE_USER=bigjpg
PURGE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --install-dir) INSTALL_DIR="$2"; shift 2 ;;
    --data-dir) DATA_DIR="$2"; shift 2 ;;
    --user) SERVICE_USER="$2"; shift 2 ;;
    --purge) PURGE=1; shift ;;
    -h|--help) print_help; exit 0 ;;
    *) echo "Opcion desconocida: $1" >&2; print_help; exit 1 ;;
  esac
done

if [ "$(id -u)" -ne 0 ]; then
  echo "Este script necesita permisos de root (usa: sudo $0)" >&2
  exit 1
fi

echo "==> Deteniendo y deshabilitando el servicio..."
systemctl stop bigjpg 2>/dev/null || true
systemctl disable bigjpg 2>/dev/null || true

echo "==> Eliminando la unidad systemd y su configuracion..."
rm -f /etc/systemd/system/bigjpg.service
rm -rf /etc/bigjpg
systemctl daemon-reload

echo "==> Eliminando $INSTALL_DIR (codigo, Python propio y entorno virtual)..."
rm -rf "$INSTALL_DIR"

if [ "$PURGE" -eq 1 ]; then
  echo "==> --purge: eliminando tambien $DATA_DIR (subidas, resultados y modelos descargados)..."
  rm -rf "$DATA_DIR"
else
  echo "==> Los datos en $DATA_DIR se han conservado (usa --purge la proxima vez para borrarlos tambien)"
fi

if id -u "$SERVICE_USER" >/dev/null 2>&1; then
  echo "==> Eliminando el usuario de sistema '$SERVICE_USER'..."
  userdel "$SERVICE_USER" 2>/dev/null || true
fi

echo "==> Desinstalacion completada."
