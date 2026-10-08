#!/usr/bin/env bash
#
# Instalador del servicio systemd de BigJPG local para Debian/Ubuntu.
#
# No toca el Python del sistema: descarga una build portable de CPython
# (python-build-standalone, la misma que usa "uv python install") dentro del
# propio directorio de instalacion y crea ahi un entorno virtual aislado. No
# se usa apt en ningun momento; los unicos requisitos son bash, curl, tar y
# systemd, que ya vienen en cualquier Debian/Ubuntu de serie.
#
# Uso:
#   sudo ./install.sh [opciones]
#
set -euo pipefail

print_help() {
  cat <<'HELPEOF'
Uso: sudo ./install.sh [opciones]

Opciones (todas tienen un valor por defecto razonable):
  --port PUERTO          Puerto de escucha (por defecto: 5002)
  --host HOST             Direccion de escucha (por defecto: 0.0.0.0, acepta
                          conexiones desde la red; usa 127.0.0.1 para dejarlo
                          solo accesible en esta maquina)
  --install-dir DIR      Codigo, Python propio y entorno virtual
                          (por defecto: /opt/bigjpg)
  --data-dir DIR         Subidas, resultados y modelos descargados
                          (por defecto: /var/lib/bigjpg)
  --user USUARIO         Usuario de sistema que ejecuta el servicio
                          (por defecto: bigjpg, se crea si no existe)
  --prefetch-weights     Descarga los modelos mas habituales durante la
                          instalacion (si no, se descargan solos la primera
                          vez que se usan, con la consiguiente espera)
  --no-start             Deja el servicio instalado pero no lo arranca
  -h, --help             Muestra esta ayuda

Por defecto escucha en 0.0.0.0, o sea accesible desde la red. La app no
tiene autenticacion propia: cualquiera que llegue al puerto podria subir
imagenes y consumir CPU. Si esto va en una red no confiable, usa
--host 127.0.0.1 y accede via proxy inverso (nginx/Caddy) con autenticacion
o un tunel SSH.
HELPEOF
}

# ------------------------------------------------------------- valores por defecto
PORT=5002
HOST=0.0.0.0
INSTALL_DIR=/opt/bigjpg
DATA_DIR=/var/lib/bigjpg
SERVICE_USER=bigjpg
PREFETCH=0
START_SERVICE=1

# Version portable de Python que se descarga (python-build-standalone). Se
# fija una version y un hash exactos, igual que el resto del proyecto fija
# las URLs de los pesos .pth: reproducible y verificable, nada de "latest".
PBS_TAG=20260814
PBS_PYVER=3.12.14
PBS_SHA256_X86_64=3297691ae34f75fed81ac424e040145fccb0bafe8e581cd5cadbddfa1c0766c0
PBS_SHA256_AARCH64=4952b18bafda1880d4ab1f86e1c348dbdb31f0e6d049e76dc5f052f2f796f1c5

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_SRC_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"   # .../webapp

# ------------------------------------------------------------------ opciones
while [ $# -gt 0 ]; do
  case "$1" in
    --port) PORT="$2"; shift 2 ;;
    --host) HOST="$2"; shift 2 ;;
    --install-dir) INSTALL_DIR="$2"; shift 2 ;;
    --data-dir) DATA_DIR="$2"; shift 2 ;;
    --user) SERVICE_USER="$2"; shift 2 ;;
    --prefetch-weights) PREFETCH=1; shift ;;
    --no-start) START_SERVICE=0; shift ;;
    -h|--help) print_help; exit 0 ;;
    *) echo "Opcion desconocida: $1" >&2; print_help; exit 1 ;;
  esac
done

# --------------------------------------------------------------- comprobaciones
if [ "$(id -u)" -ne 0 ]; then
  echo "Este instalador necesita permisos de root (usa: sudo $0)" >&2
  exit 1
fi

for cmd in curl tar sha256sum systemctl useradd; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "Falta el comando '$cmd', necesario para instalar. Instalalo e intentalo de nuevo." >&2
    exit 1
  fi
done

if [ "$SERVICE_USER" = "root" ]; then
  echo "Aviso: vas a ejecutar el servicio como root. No es lo recomendado; " >&2
  echo "usa --user con un usuario normal salvo que tengas una razon concreta." >&2
fi

ARCH="$(uname -m)"
case "$ARCH" in
  x86_64) PBS_ARCH=x86_64; PBS_SHA256=$PBS_SHA256_X86_64 ;;
  aarch64|arm64) PBS_ARCH=aarch64; PBS_SHA256=$PBS_SHA256_AARCH64 ;;
  *)
    echo "Arquitectura no soportada: $ARCH (solo x86_64 y aarch64)" >&2
    exit 1
    ;;
esac

echo "==> BigJPG local: instalando servicio systemd"
echo "    Codigo/Python : $INSTALL_DIR"
echo "    Datos/pesos   : $DATA_DIR"
echo "    Escucha en    : $HOST:$PORT (usuario del servicio: $SERVICE_USER)"
echo

# ------------------------------------------------------ python propio (no del sistema)
PYTHON_DIR="$INSTALL_DIR/python"
PYTHON_BIN="$PYTHON_DIR/bin/python3"
mkdir -p "$INSTALL_DIR"

if [ -x "$PYTHON_BIN" ]; then
  echo "==> Ya hay un Python propio en $PYTHON_DIR, no se vuelve a descargar"
else
  echo "==> Descargando CPython $PBS_PYVER portable ($PBS_ARCH)..."
  echo "    (build independiente del sistema operativo; el Python de Debian no se toca)"
  TARBALL_NAME="cpython-${PBS_PYVER}+${PBS_TAG}-${PBS_ARCH}-unknown-linux-gnu-install_only.tar.gz"
  TARBALL_URL="https://github.com/astral-sh/python-build-standalone/releases/download/${PBS_TAG}/${TARBALL_NAME}"
  TMP_TARBALL="$(mktemp)"
  trap 'rm -f "$TMP_TARBALL"' EXIT

  curl -fL --progress-bar -o "$TMP_TARBALL" "$TARBALL_URL"

  echo "==> Verificando la integridad de la descarga (sha256)..."
  echo "${PBS_SHA256}  ${TMP_TARBALL}" | sha256sum -c - >/dev/null
  echo "    OK"

  echo "==> Extrayendo Python en $PYTHON_DIR..."
  rm -rf "$PYTHON_DIR"
  tar -xzf "$TMP_TARBALL" -C "$INSTALL_DIR"
  rm -f "$TMP_TARBALL"
  trap - EXIT

  if [ ! -x "$PYTHON_BIN" ]; then
    echo "No se encontro $PYTHON_BIN tras extraer el tarball. Instalacion abortada." >&2
    exit 1
  fi
fi

echo -n "    "; "$PYTHON_BIN" --version

# --------------------------------------------------------------------- entorno virtual
VENV_DIR="$INSTALL_DIR/venv"
if [ ! -x "$VENV_DIR/bin/python" ]; then
  echo "==> Creando entorno virtual en $VENV_DIR..."
  "$PYTHON_BIN" -m venv "$VENV_DIR"
fi

echo "==> Copiando la aplicacion a $INSTALL_DIR/app..."
mkdir -p "$INSTALL_DIR/app"
tar --exclude='.venv' --exclude='data' --exclude='__pycache__' --exclude='deploy' \
    -C "$APP_SRC_DIR" -cf - . | tar -C "$INSTALL_DIR/app" -xf -

echo "==> Instalando dependencias (torch puede tardar varios minutos)..."
"$VENV_DIR/bin/pip" install --upgrade pip --quiet
# torch se instala aparte, desde el indice CPU de PyTorch: el indice normal
# de PyPI en Linux trae por defecto las librerias CUDA (varios cientos de MB
# extra) aunque la maquina no tenga GPU NVIDIA.
grep -vi '^torch' "$INSTALL_DIR/app/requirements.txt" > "$INSTALL_DIR/app/.requirements-no-torch.txt"
"$VENV_DIR/bin/pip" install -r "$INSTALL_DIR/app/.requirements-no-torch.txt"
"$VENV_DIR/bin/pip" install torch --index-url https://download.pytorch.org/whl/cpu
rm -f "$INSTALL_DIR/app/.requirements-no-torch.txt"

# ------------------------------------------------------------------- usuario
if ! id -u "$SERVICE_USER" >/dev/null 2>&1; then
  echo "==> Creando el usuario de sistema '$SERVICE_USER'..."
  useradd --system --home-dir "$DATA_DIR" --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
fi

# --------------------------------------------------------------------- datos
mkdir -p "$DATA_DIR/data/uploads" "$DATA_DIR/data/results" "$DATA_DIR/weights"
chown -R "$SERVICE_USER:$SERVICE_USER" "$DATA_DIR"
chmod -R go+rX "$INSTALL_DIR"

# ---------------------------------------------------------- entorno del servicio
mkdir -p /etc/bigjpg
cat > /etc/bigjpg/bigjpg.env <<ENV_EOF
# Configuracion del servicio BigJPG local.
# Tras cambiar algo aqui: sudo systemctl restart bigjpg
BIGJPG_HOST=$HOST
BIGJPG_PORT=$PORT
BIGJPG_DATA_DIR=$DATA_DIR/data
BIGJPG_WEIGHTS_DIR=$DATA_DIR/weights
BIGJPG_MAX_UPLOAD_MB=30
BIGJPG_KEEP_SECONDS=21600

# Agente de estado para QuemaOS (ver QuemaOS/docs/AGENT_API.md). Siempre
# en 127.0.0.1, independientemente de BIGJPG_HOST. BIGJPG_QUEMAOS_ENABLED=0
# lo desactiva.
BIGJPG_QUEMAOS_ENABLED=1
BIGJPG_QUEMAOS_PORT=9702
ENV_EOF
chmod 644 /etc/bigjpg/bigjpg.env

# ------------------------------------------------------------------- systemd
cat > /etc/systemd/system/bigjpg.service <<UNIT_EOF
[Unit]
Description=BigJPG local - ampliacion y restauracion de imagenes con IA
After=network.target

[Service]
Type=simple
User=$SERVICE_USER
Group=$SERVICE_USER
EnvironmentFile=/etc/bigjpg/bigjpg.env
WorkingDirectory=$INSTALL_DIR/app
ExecStart=$VENV_DIR/bin/python $INSTALL_DIR/app/app.py
Restart=on-failure
RestartSec=5

# Endurecimiento basico. ReadWritePaths es lo unico que el servicio necesita
# escribir: subidas, resultados y pesos de los modelos.
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=$DATA_DIR
PrivateTmp=true

[Install]
WantedBy=multi-user.target
UNIT_EOF

systemctl daemon-reload

# ----------------------------------------------------------- descarga previa
if [ "$PREFETCH" -eq 1 ]; then
  echo "==> Descargando los modelos mas habituales (puede tardar varios minutos)..."
  (
    cd "$INSTALL_DIR/app"
    sudo -u "$SERVICE_USER" "$VENV_DIR/bin/python" -c "
import sys
sys.path.insert(0, '.')
from esrgan.registry import ensure_weights as esrgan_ensure
from lama.registry import ensure_weights as lama_ensure

weights_dir = '$DATA_DIR/weights'
for model in ('RealESRGAN_x4plus_anime_6B', 'realesr-general-x4v3'):
    print('   modelo:', model)
    esrgan_ensure(model, weights_dir)
print('   modelo: big-lama (restaurar / quitar marcas de agua)')
lama_ensure(weights_dir)
print('   listo')
"
  )
fi

# -------------------------------------------------------------------- arranque
systemctl enable bigjpg >/dev/null

if [ "$START_SERVICE" -eq 1 ]; then
  echo "==> Arrancando el servicio..."
  systemctl restart bigjpg
  sleep 2
  if systemctl is-active --quiet bigjpg; then
    echo "    Servicio activo."
  else
    echo "    El servicio no ha arrancado correctamente. Revisa: journalctl -u bigjpg -n 50" >&2
    exit 1
  fi
else
  echo "==> Servicio instalado y habilitado, pero no arrancado (--no-start)."
fi

echo
echo "==> Instalacion completada."
echo "    URL local     : http://127.0.0.1:$PORT"
if [ "$HOST" = "0.0.0.0" ]; then
  LAN_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
  if [ -n "$LAN_IP" ]; then
    echo "    URL en red    : http://$LAN_IP:$PORT"
  fi
  echo "    (escuchando en 0.0.0.0: accesible desde cualquier maquina de la red; sin"
  echo "     autenticacion propia, ver el aviso de --help si esto te preocupa)"
fi
echo "    Agente QuemaOS: http://127.0.0.1:9702/quemaos/status (solo loopback, siempre)"
echo "    Configuracion : /etc/bigjpg/bigjpg.env (edita y reinicia con: systemctl restart bigjpg)"
echo "    Logs          : journalctl -u bigjpg -f"
echo "    Datos         : $DATA_DIR/data (subidas y resultados, se borran solos pasadas unas horas)"
echo "    Modelos       : $DATA_DIR/weights (se descargan solos la primera vez que se usa cada uno)"
if [ "$PREFETCH" -eq 0 ]; then
  echo
  echo "    Nota: los modelos no se han pre-descargado. La primera vez que ampliés o"
  echo "    restaures una imagen con un modelo nuevo, esa peticion tardara mas mientras"
  echo "    se descarga (unos 65 MB el modelo de anime, ~200 MB el de restauracion)."
fi
