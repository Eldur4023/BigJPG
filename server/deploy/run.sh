#!/bin/bash
#
# Lanzador del servicio. Lo ejecuta systemd; no se llama a mano.
#
# El shebang es la ruta absoluta a propósito, no "/usr/bin/env bash": la unidad puede restringir el PATH.
#
# Lee /etc/bigjpg/bigjpg.env (que systemd ya ha cargado en el entorno):
#
#   BIGJPG_HOST        IP de Tailscale del servidor: BigJPG escucha SÓLO ahí (no en la LAN ni en Internet).
#   BIGJPG_PORT        puerto (5002)
#   BIGJPG_DB          base de datos SQLite
#   BIGJPG_DATA_DIR    subidas y resultados
#   BIGJPG_ENGINE_DIR  dónde está Real-ESRGAN (lo instala install.sh)
#   BIGJPG_GPU         «auto» o el número de dispositivo Vulkan
#   BIGJPG_MAX_PIXELS / BIGJPG_KEEP_HOURS / BIGJPG_MAX_BODY   límites (ver app/app.lux)
#   LUX_BIN            binario de Lux (por defecto, /opt/bigjpg/lux)
#
# Lux resuelve BIGJPG_DB, BIGJPG_HOST, BIGJPG_PORT y BIGJPG_MAX_BODY al compilar (env() en app/app.lux), así que
# tienen que estar en el entorno antes de lanzarlo.

set -euo pipefail

_script="${BASH_SOURCE[0]}"
_dir="${_script%/*}"
[[ "$_dir" == "$_script" ]] && _dir="."
APP_DIR="$(cd "$_dir" && pwd)"

export BIGJPG_DB="${BIGJPG_DB:-/var/lib/bigjpg/bigjpg.db}"
export BIGJPG_DATA_DIR="${BIGJPG_DATA_DIR:-/var/lib/bigjpg/data}"
export BIGJPG_ENGINE_DIR="${BIGJPG_ENGINE_DIR:-$APP_DIR/engine}"
BIGJPG_PORT="${BIGJPG_PORT:-5002}"
export BIGJPG_PORT
# Lux propio de BigJPG (con sus módulos): no el del sistema, que puede ser el de otra app.
LUX_BIN="${LUX_BIN:-$APP_DIR/lux}"

if [[ ! -x "$LUX_BIN" ]]; then
    echo "No se encuentra el binario de Lux ($LUX_BIN). Reinstala con install.sh." >&2
    exit 1
fi
if [[ ! -x "$BIGJPG_ENGINE_DIR/realesrgan-ncnn-vulkan" ]]; then
    echo "Aviso: Real-ESRGAN no está en $BIGJPG_ENGINE_DIR; la web arrancará pero no podrá ampliar (reinstala con install.sh)." >&2
fi

cd "$APP_DIR"
HOST="${BIGJPG_HOST:-}"
if [[ -z "$HOST" || "$HOST" == 0.0.0.0 ]]; then
    echo "BIGJPG_HOST debe ser la IP de Tailscale (es '${HOST}'): BigJPG no se expone a otras redes." >&2
    exit 1
fi
# Tras un reinicio, tailscaled puede tardar en poner la IP en la interfaz: se espera en vez de fallar.
# Se mira /proc/net/fib_trie y no `ip addr`: `ip` habla por netlink, que la unidad de systemd no permite
# (RestrictAddressFamilies), y dentro del servicio fallaba en silencio.
tiene_ip() { grep -qF -- "|-- $HOST" /proc/net/fib_trie 2>/dev/null; }
if [[ "$HOST" != 127.0.0.1 ]]; then
    for _ in $(seq 1 90); do tiene_ip && break; sleep 1; done
    tiene_ip || { echo "La IP $HOST no está en ninguna interfaz (¿tailscaled?)." >&2; exit 1; }
fi
echo "Escuchando en http://$HOST:$BIGJPG_PORT (red de Tailscale; el tráfico ya va cifrado por WireGuard)"
# --no-watch: en producción no se vigilan los ficheros para recargar.
exec "$LUX_BIN" "$APP_DIR/app" --no-watch --port "$BIGJPG_PORT"
