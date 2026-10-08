#!/bin/bash
#
# Lanzador de la sonda de QuemaOS (bigjpg-quemaos.service). Lee /etc/bigjpg/bigjpg.env:
#
#   BIGJPG_DB               la base de BigJPG (sólo se lee)
#   BIGJPG_QUEMAOS_PORT     puerto de la sonda: 9700-9799, el rango de la suite (9702)
#   BIGJPG_QUEMAOS_HOST     127.0.0.1 (QuemaOS sólo consulta el bucle local)
#   BIGJPG_HOST/BIGJPG_PORT dónde escucha BigJPG, para comprobar que responde y dar su URL
set -euo pipefail

_script="${BASH_SOURCE[0]}"
_dir="${_script%/*}"
[[ "$_dir" == "$_script" ]] && _dir="."
APP_DIR="$(cd "$_dir" && pwd)"

PORT="${BIGJPG_QUEMAOS_PORT:-9702}"
if ! [[ "$PORT" =~ ^97[0-9][0-9]$ ]]; then
    echo "BIGJPG_QUEMAOS_PORT debe estar entre 9700 y 9799 (es '$PORT')" >&2
    exit 1
fi
export BIGJPG_DB="${BIGJPG_DB:-/var/lib/bigjpg/bigjpg.db}"
export BIGJPG_ENGINE_DIR="${BIGJPG_ENGINE_DIR:-$APP_DIR/engine}"
export BIGJPG_DATA_DIR="${BIGJPG_DATA_DIR:-/var/lib/bigjpg/data}"
# BigJPG escucha en la IP de Tailscale, no en 127.0.0.1: la sonda lo comprueba ahí y publica esa URL (el botón
# «Abrir» de QuemaOS).
export BIGJPG_URL="${BIGJPG_URL:-http://${BIGJPG_HOST:-127.0.0.1}:${BIGJPG_PORT:-5002}}"
export BIGJPG_QUEMAOS_PORT="$PORT"

echo "Sonda de QuemaOS en http://${BIGJPG_QUEMAOS_HOST:-127.0.0.1}:$PORT/quemaos/status"
exec "$APP_DIR/lux" "$APP_DIR/quemaos" --no-watch --port "$PORT"
