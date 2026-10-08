#!/usr/bin/env bash
#
# Trae Real-ESRGAN (el port oficial ncnn-Vulkan de xinntao) y lo deja listo en DESTINO:
#
#   DESTINO/realesrgan-ncnn-vulkan      el binario
#   DESTINO/models/*.param|*.bin         los modelos (realesrgan-x4plus, realesrgan-x4plus-anime, realesr-animevideov3-x2/x3/x4)
#   DESTINO/VERSION                      el hash del paquete instalado
#
#   engine/install-engine.sh DESTINO
#
# Es el paquete portátil de Linux de https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.5.0 (BSD-3).
# Se descarga una sola vez, con la versión y el hash FIJADOS aquí: si el hash no coincide, no se instala.
# Se puede repetir sin coste (si ya está esa versión, no hace nada). Para no descargar, BIGJPG_ENGINE_ZIP=/ruta.zip.
set -euo pipefail

ZIP_URL="https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-ubuntu.zip"
ZIP_SHA256="e5aa6eb131234b87c0c51f82b89390f5e3e642b7b70f2b9bbe95b6a285a40c96"

DEST="${1:?uso: install-engine.sh DESTINO}"
if [[ -x "$DEST/realesrgan-ncnn-vulkan" && "$(cat "$DEST/VERSION" 2>/dev/null)" == "$ZIP_SHA256" ]]; then
    echo "Real-ESRGAN ya está instalado en $DEST"
    exit 0
fi
command -v unzip >/dev/null || { echo "falta unzip (sudo apt install unzip)" >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
ZIP="${BIGJPG_ENGINE_ZIP:-$TMP/engine.zip}"
if [[ ! -f "$ZIP" ]]; then
    echo "==> Descargando Real-ESRGAN (47 MB)"
    curl -fL --retry 3 --progress-bar -o "$ZIP" "$ZIP_URL"
fi
echo "$ZIP_SHA256  $ZIP" | sha256sum -c --quiet - || { echo "el hash del paquete no coincide: no se instala" >&2; exit 1; }

unzip -q -o "$ZIP" realesrgan-ncnn-vulkan 'models/*' -d "$TMP/x"
mkdir -p "$DEST"
rm -rf "$DEST/models"
mv "$TMP/x/models" "$DEST/models"
install -m 755 "$TMP/x/realesrgan-ncnn-vulkan" "$DEST/realesrgan-ncnn-vulkan"
echo "$ZIP_SHA256" > "$DEST/VERSION"
echo "Real-ESRGAN instalado en $DEST ($(ls "$DEST/models" | wc -l) ficheros de modelos)"
