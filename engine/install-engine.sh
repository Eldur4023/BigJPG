#!/usr/bin/env bash
#
# Trae Real-ESRGAN (el port oficial ncnn-Vulkan de xinntao) y lo deja listo en DESTINO:
#
#   DESTINO/realesrgan-ncnn-vulkan      el binario
#   DESTINO/models/*.param|*.bin         los modelos (realesrgan-x4plus, realesrgan-x4plus-anime, realesr-animevideov3-x2/x3/x4)
#   DESTINO/VERSION                      el hash del paquete instalado
#   DESTINO/libonnxruntime.so            ONNX Runtime (CPU), que carga el módulo `inpaint` de Lux
#   DESTINO/lama_fp32.onnx               LaMa (Carve/LaMa-ONNX), para quitar marcas de agua y objetos
#
#   engine/install-engine.sh DESTINO
#
# Es el paquete portátil de Linux de https://github.com/xinntao/Real-ESRGAN/releases/tag/v0.2.5.0 (BSD-3).
# Se descarga una sola vez, con la versión y el hash FIJADOS aquí: si el hash no coincide, no se instala.
# Se puede repetir sin coste (si ya está esa versión, no hace nada). Para no descargar, BIGJPG_ENGINE_ZIP=/ruta.zip.
# Para quitar marcas de agua: ONNX Runtime 1.20.1 (MIT, 7 MB) y LaMa (208 MB), también fijados por hash.
# BIGJPG_SKIP_LAMA=1 los omite (la pantalla «Quitar marca» queda sin motor).
set -euo pipefail

ZIP_URL="https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-ubuntu.zip"
ZIP_SHA256="e5aa6eb131234b87c0c51f82b89390f5e3e642b7b70f2b9bbe95b6a285a40c96"

ORT_URL="https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-linux-x64-1.20.1.tgz"
ORT_SHA256="67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105"
LAMA_URL="https://huggingface.co/Carve/LaMa-ONNX/resolve/main/lama_fp32.onnx"
LAMA_SHA256="1faef5301d78db7dda502fe59966957ec4b79dd64e16f03ed96913c7a4eb68d6"

DEST="${1:?uso: install-engine.sh DESTINO}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

install_lama() {
    [[ "${BIGJPG_SKIP_LAMA:-}" == 1 ]] && return 0
    mkdir -p "$DEST"
    if [[ ! -f "$DEST/libonnxruntime.so" || "$(cat "$DEST/ORT_VERSION" 2>/dev/null)" != "$ORT_SHA256" ]]; then
        echo "==> Descargando ONNX Runtime (7 MB)"
        curl -fL --retry 3 --progress-bar -o "$TMP/ort.tgz" "$ORT_URL"
        echo "$ORT_SHA256  $TMP/ort.tgz" | sha256sum -c --quiet - || { echo "el hash de ONNX Runtime no coincide: no se instala" >&2; exit 1; }
        mkdir -p "$TMP/ort"
        tar xzf "$TMP/ort.tgz" -C "$TMP/ort" --strip-components=1
        install -m 755 "$(readlink -f "$TMP/ort/lib/libonnxruntime.so")" "$DEST/libonnxruntime.so"
        echo "$ORT_SHA256" > "$DEST/ORT_VERSION"
    fi
    if [[ ! -f "$DEST/lama_fp32.onnx" || "$(cat "$DEST/LAMA_VERSION" 2>/dev/null)" != "$LAMA_SHA256" ]]; then
        echo "==> Descargando LaMa (208 MB)"
        curl -fL --retry 3 --progress-bar -o "$TMP/lama.onnx" "$LAMA_URL"
        echo "$LAMA_SHA256  $TMP/lama.onnx" | sha256sum -c --quiet - || { echo "el hash de LaMa no coincide: no se instala" >&2; exit 1; }
        install -m 644 "$TMP/lama.onnx" "$DEST/lama_fp32.onnx"
        echo "$LAMA_SHA256" > "$DEST/LAMA_VERSION"
    fi
    echo "LaMa instalado en $DEST"
}

if [[ -x "$DEST/realesrgan-ncnn-vulkan" && "$(cat "$DEST/VERSION" 2>/dev/null)" == "$ZIP_SHA256" ]]; then
    echo "Real-ESRGAN ya está instalado en $DEST"
    install_lama
    exit 0
fi
command -v unzip >/dev/null || { echo "falta unzip (sudo apt install unzip)" >&2; exit 1; }

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
install_lama
