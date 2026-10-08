#!/usr/bin/env bash
#
# Instalación de BigJPG (servidor) en Ubuntu Server (22.04 / 24.04).
#
#   sudo bash deploy/install.sh
#
# Deja el servicio escuchando en la IP de TAILSCALE del servidor (puerto 5002) bajo systemd, y SÓLO ahí: BigJPG no
# se expone a la LAN ni a Internet, y no lleva nginx ni TLS (el tráfico por Tailscale ya va cifrado). Trae él
# mismo Real-ESRGAN (el motor), instala los controladores Vulkan y da al servicio acceso a la GPU.
#
# Convive con otras apps de la máquina: usuario `bigjpg`, /opt/bigjpg, /var/lib/bigjpg, puerto 5002 y su PROPIO
# binario de Lux (/opt/bigjpg/lux), sin tocar el del sistema. El binario de Lux sale de, por orden:
#   1. LUX_BIN=/ruta/a/lux     2. un fichero «lux» junto a deploy/ (lo envía deploy.sh).
#
# Si encuentra la versión antigua (Python + PyTorch), la sustituye: guarda antes su código, su unidad y su
# configuración en /root/bigjpg-python-<fecha>.tar.gz, y deja el entorno virtual para quitarlo cuando la nueva
# funcione (`sudo bash deploy/limpiar-python.sh`).

set -euo pipefail

APP_DIR=/opt/bigjpg
DATA_DIR=/var/lib/bigjpg
ENV_DIR=/etc/bigjpg
SERVICE_USER=bigjpg

if [[ $EUID -ne 0 ]]; then
    echo "Ejecútalo como root (sudo)." >&2
    exit 1
fi

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

LUX_SRC_BIN="${LUX_BIN:-}"
[[ -z "$LUX_SRC_BIN" && -f "$SOURCE_DIR/lux" ]] && LUX_SRC_BIN="$SOURCE_DIR/lux"
if [[ -z "$LUX_SRC_BIN" || ! -f "$LUX_SRC_BIN" ]]; then
    echo "No hay binario de Lux. Pásalo con LUX_BIN=/ruta/lux (deploy.sh lo envía solo)." >&2
    exit 1
fi

echo "==> Buscando la IP de Tailscale"
TS_IP=""
command -v tailscale >/dev/null 2>&1 && TS_IP="$(tailscale ip -4 2>/dev/null | head -1)"
if [[ -z "$TS_IP" ]]; then
    echo "No encuentro la IP de Tailscale (¿está tailscaled en marcha?). BigJPG sólo escucha ahí." >&2
    exit 1
fi
echo "    $TS_IP"

echo "==> Instalando dependencias del sistema"
apt-get update -qq
# Bibliotecas que enlaza el binario de Lux; curl y unzip para traer el motor; libvulkan1 y mesa-vulkan-drivers
# (controladores Vulkan de AMD/Intel, y el de CPU por software como último recurso); tzdata.
apt-get install -y --no-install-recommends \
    ca-certificates curl unzip openssl \
    libsqlite3-0 libcurl4 libwebp7 libpng16-16 libjpeg-turbo8 libcairo2 \
    libjemalloc2 libpq5 libmysqlclient21 zlib1g libargon2-1 tzdata \
    libvulkan1 mesa-vulkan-drivers

echo "==> Preparando el usuario de servicio"
if ! id "$SERVICE_USER" &>/dev/null; then
    adduser --system --group --home "$DATA_DIR" --no-create-home "$SERVICE_USER"
fi
# La GPU se abre por /dev/dri/renderD128 (grupo render).
for g in render video; do getent group "$g" >/dev/null && usermod -aG "$g" "$SERVICE_USER" || true; done

# --- migración desde la versión Python -------------------------------------------------------------
OLD_PY=0
if [[ -d "$APP_DIR/venv" || -f "$APP_DIR/app/app.py" ]]; then
    OLD_PY=1
    BK="/root/bigjpg-python-$(date +%F).tar.gz"
    echo "==> Versión antigua (Python) encontrada: guardando su código y configuración en $BK"
    tar czf "$BK" -C / --ignore-failed-read opt/bigjpg/app etc/bigjpg etc/systemd/system/bigjpg.service etc/systemd/system/bigjpg.service.d 2>/dev/null || true
    chmod 600 "$BK"
    systemctl stop bigjpg 2>/dev/null || true
fi

echo "==> Copiando la aplicación a $APP_DIR"
mkdir -p "$APP_DIR"
# Se borra el código anterior en vez de copiar encima: si no, un fichero que se haya eliminado seguiría vivo.
rm -rf "$APP_DIR/app" "$APP_DIR/deploy" "$APP_DIR/quemaos"
cp -r "$SOURCE_DIR/app" "$APP_DIR/app"
cp -r "$SOURCE_DIR/quemaos" "$APP_DIR/quemaos"
install -m 755 "$LUX_SRC_BIN" "$APP_DIR/lux"
cp "$SOURCE_DIR/deploy/run.sh" "$SOURCE_DIR/deploy/quemaos-run.sh" "$APP_DIR/"
chmod 755 "$APP_DIR/run.sh" "$APP_DIR/quemaos-run.sh"
cp -r "$SOURCE_DIR/deploy" "$APP_DIR/deploy"
rm -f "$APP_DIR/deploy/lux"
mkdir -p "$APP_DIR/engine-src"
cp "$SOURCE_DIR/engine/install-engine.sh" "$APP_DIR/engine-src/"

echo "==> Comprobando que la aplicación compila con este Lux"
BIGJPG_DB=/dev/null "$APP_DIR/lux" --check "$APP_DIR/app" >/dev/null
BIGJPG_DB=/dev/null "$APP_DIR/lux" --check "$APP_DIR/quemaos" >/dev/null

echo "==> Instalando Real-ESRGAN (el motor)"
bash "$SOURCE_DIR/engine/install-engine.sh" "$APP_DIR/engine"

echo "==> Preparando los directorios de datos"
mkdir -p "$DATA_DIR/data" "$ENV_DIR"
chown -R "$SERVICE_USER:$SERVICE_USER" "$DATA_DIR"
chmod 750 "$DATA_DIR"

# Configuración: la antigua (de la versión Python) no sirve; se conserva como .python y se escribe la nueva.
if [[ -f "$ENV_DIR/bigjpg.env" ]] && ! grep -q '^BIGJPG_DB=' "$ENV_DIR/bigjpg.env"; then
    mv "$ENV_DIR/bigjpg.env" "$ENV_DIR/bigjpg.env.python"
fi
if [[ ! -f "$ENV_DIR/bigjpg.env" ]]; then
    cat > "$ENV_DIR/bigjpg.env" <<ENV
# Configuración de BigJPG. Lo lee systemd y run.sh.
BIGJPG_DB=$DATA_DIR/bigjpg.db
BIGJPG_DATA_DIR=$DATA_DIR/data
BIGJPG_ENGINE_DIR=$APP_DIR/engine

# Dónde escucha: SÓLO la IP de Tailscale de este servidor. No se expone a la LAN ni a Internet; si la IP de
# Tailscale cambiara, actualiza BIGJPG_HOST y reinicia.
BIGJPG_HOST=$TS_IP
BIGJPG_PORT=5002

# Sonda de estado para QuemaOS: su propio puerto, del rango 9700-9799 de la suite (sólo 127.0.0.1).
BIGJPG_QUEMAOS_PORT=9702

# Límites: píxeles de entrada, tamaño de subida y cuánto se conservan los trabajos terminados.
BIGJPG_MAX_PIXELS=16000000
BIGJPG_MAX_BODY=60MB
BIGJPG_KEEP_HOURS=6

# GPU de Vulkan: «auto» (la primera) o el número de dispositivo (-g de realesrgan).
BIGJPG_GPU=auto
ENV
    chmod 640 "$ENV_DIR/bigjpg.env"
    chown root:"$SERVICE_USER" "$ENV_DIR/bigjpg.env"
    echo "    configuración creada en $ENV_DIR/bigjpg.env"
else
    echo "    $ENV_DIR/bigjpg.env ya existe: se conserva."
fi

chown -R root:"$SERVICE_USER" "$APP_DIR"
chmod -R g+rX "$APP_DIR"

echo "==> Probando el motor con una imagen de verdad (como el usuario del servicio)"
TESTDIR="$(mktemp -d)"; chown "$SERVICE_USER" "$TESTDIR"
# Un PNG de 8×8 (rojo), en base64.
base64 -d > "$TESTDIR/t.png" <<'B64'
iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEUlEQVR4nGO4WR6OFTEMLQkAcq9pwYW/CuAAAAAASUVORK5CYII=
B64
chown "$SERVICE_USER" "$TESTDIR/t.png"
set +e
sudo -u "$SERVICE_USER" env HOME="$DATA_DIR" "$APP_DIR/engine/realesrgan-ncnn-vulkan" -i "$TESTDIR/t.png" -o "$TESTDIR/o.png" \
    -n realesrgan-x4plus-anime -s 4 -m "$APP_DIR/engine/models" 2> "$TESTDIR/log"
RC=$?
set -e
DEV="$(grep -o '^\[0 [^]]*\]' "$TESTDIR/log" | head -1 | sed 's/^\[0 //; s/\]$//')"
if [[ $RC -eq 0 && -s "$TESTDIR/o.png" ]]; then
    if echo "$DEV" | grep -qi llvmpipe; then
        echo "    aviso: sin GPU; irá por CPU con Vulkan por software (funciona, pero es lento). Dispositivo: $DEV"
    else
        echo "    motor OK en: $DEV"
    fi
else
    echo "    aviso: el motor no pudo ampliar la imagen de prueba (código $RC):" >&2
    grep -vE '^\[|^[0-9.]+%$' "$TESTDIR/log" | head -5 >&2 || true
fi
rm -rf "$TESTDIR"

echo "==> Instalando el servicio"
cp "$SOURCE_DIR/deploy/bigjpg.service" /etc/systemd/system/bigjpg.service
cp "$SOURCE_DIR/deploy/bigjpg-quemaos.service" /etc/systemd/system/bigjpg-quemaos.service
# Los drop-ins de la versión Python (p. ej. tailscale.conf) ya no hacen falta: la unidad nueva lo trae.
rm -rf /etc/systemd/system/bigjpg.service.d
systemctl daemon-reload
systemctl enable bigjpg.service bigjpg-quemaos.service >/dev/null 2>&1
systemctl restart bigjpg.service

PUERTO="$(grep -E '^BIGJPG_PORT=' "$ENV_DIR/bigjpg.env" | cut -d= -f2)"
PUERTO="${PUERTO:-5002}"
HOST_ENV="$(grep -E '^BIGJPG_HOST=' "$ENV_DIR/bigjpg.env" | cut -d= -f2)"
HOST_ENV="${HOST_ENV:-$TS_IP}"
for _ in $(seq 1 30); do
    if curl -fsS --max-time 3 "http://$HOST_ENV:$PUERTO/healthz" >/dev/null 2>&1; then break; fi
    sleep 1
done
systemctl restart bigjpg-quemaos.service
QPORT="$(grep -E '^BIGJPG_QUEMAOS_PORT=' "$ENV_DIR/bigjpg.env" | cut -d= -f2)"
QPORT="${QPORT:-9702}"
sleep 2

echo
if curl -fsS --max-time 3 "http://127.0.0.1:$QPORT/quemaos/status" >/dev/null 2>&1; then
    echo "Sonda de QuemaOS en http://127.0.0.1:$QPORT/quemaos/status"
else
    echo "aviso: la sonda de QuemaOS no responde (journalctl -u bigjpg-quemaos)" >&2
fi
if curl -fsS --max-time 3 "http://$HOST_ENV:$PUERTO/healthz" >/dev/null 2>&1; then
    echo "BigJPG responde en http://$HOST_ENV:$PUERTO"
else
    echo "El servicio no responde todavía. Revisa:" >&2
    echo "  systemctl status bigjpg; journalctl -u bigjpg -n 50" >&2
    if [[ $OLD_PY -eq 1 ]]; then
        echo "Para volver a la versión Python: sudo bash $APP_DIR/deploy/volver-a-python.sh" >&2
    fi
    exit 1
fi
echo
echo "BigJPG escucha en http://$HOST_ENV:$PUERTO, sólo por Tailscale (no está expuesto a Internet ni a la LAN)."
echo "En la app de escritorio, la dirección del servidor es esa."
if [[ $OLD_PY -eq 1 ]]; then
    echo
    echo "La versión antigua (Python + PyTorch, ~1,4 GB) sigue en $APP_DIR/venv y $APP_DIR/python, y sus pesos en"
    echo "$DATA_DIR/weights. Cuando compruebes que la nueva te sirve:  sudo bash $APP_DIR/deploy/limpiar-python.sh"
fi
