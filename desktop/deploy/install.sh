#!/usr/bin/env bash
# Instala BigJPG de escritorio: la ventana, el icono, el lanzador y el motor (Real-ESRGAN) para ampliar en ESTE equipo.
#
#   sudo ./desktop/deploy/install.sh [--user NOMBRE]     instalar o actualizar
#   sudo ./desktop/deploy/install.sh --uninstall         quitarlo (no borra tus trabajos: ~/.local/share/lux-desktop)
#
# Antes: cmake -S desktop -B build && cmake --build build --target bigjpg-desktop
set -euo pipefail

[ "$(id -u)" -eq 0 ] || { echo "ejecútalo con sudo" >&2; exit 1; }
HERE=$(cd "$(dirname "$0")/.." && pwd)        # .../desktop
REPO=$(cd "$HERE/.." && pwd)
BUILD="$REPO/build"
APP_USER=${SUDO_USER:-}
UNINSTALL=0
while [ $# -gt 0 ]; do
  case $1 in
    --user) APP_USER=$2; shift 2 ;;
    --uninstall) UNINSTALL=1; shift ;;
    *) echo "opción desconocida: $1" >&2; exit 2 ;;
  esac
done

if [ $UNINSTALL -eq 1 ]; then
  rm -f /usr/local/bin/bigjpg-desktop /usr/share/applications/bigjpg-desktop.desktop /usr/share/icons/hicolor/256x256/apps/bigjpg-desktop.png
  rm -rf /opt/bigjpg-desktop
  echo "Desinstalado. Tus trabajos y ajustes se han conservado."
  exit 0
fi

[ -n "$APP_USER" ] && [ "$APP_USER" != root ] || { echo "indica tu usuario: --user NOMBRE" >&2; exit 2; }
id "$APP_USER" >/dev/null || { echo "no existe el usuario $APP_USER" >&2; exit 2; }
BIN=$BUILD/bigjpg-desktop
[ -x "$BIN" ] || { echo "falta $BIN: compila primero (cmake -S desktop -B build && cmake --build build --target bigjpg-desktop)" >&2; exit 1; }

echo "==> Instalando en /usr/local/bin/bigjpg-desktop"
# `install` borra el destino antes de copiar: sirve aunque la app esté abierta.
install -m 0755 "$BIN" /usr/local/bin/bigjpg-desktop
install -Dm 0644 "$HERE/app/icon.png" /usr/share/icons/hicolor/256x256/apps/bigjpg-desktop.png
install -Dm 0644 "$HERE/deploy/bigjpg-desktop.desktop" /usr/share/applications/bigjpg-desktop.desktop
gtk-update-icon-cache -qf /usr/share/icons/hicolor 2>/dev/null || true

# Para que la ventana busque actualizaciones: de qué repositorio y commit se instaló.
install -d /opt/bigjpg-desktop
echo "$REPO" > /opt/bigjpg-desktop/source
runuser -u "$APP_USER" -- git -C "$REPO" rev-parse HEAD > /opt/bigjpg-desktop/version 2>/dev/null || rm -f /opt/bigjpg-desktop/version

# El motor: Real-ESRGAN (ncnn-Vulkan), para ampliar en este equipo. Usa la GPU por Vulkan.
echo "==> Real-ESRGAN (el motor)"
bash "$REPO/engine/install-engine.sh" /opt/bigjpg-desktop/engine
if ! ls /usr/share/vulkan/icd.d/*.json >/dev/null 2>&1; then
  echo "aviso: no hay controladores Vulkan (sudo apt install mesa-vulkan-drivers libvulkan1); sin ellos no se puede ampliar en este equipo, aunque sí en el servidor." >&2
fi

echo "Listo. Ábrelo desde el menú de aplicaciones o con: bigjpg-desktop"
echo "En ⚙ pon la dirección de tu servidor de BigJPG para poder ampliar allí."
