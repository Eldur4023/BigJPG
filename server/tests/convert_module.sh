#!/usr/bin/env bash
#
# Prueba del módulo nativo `convert` de Lux con ffmpeg de verdad (se salta sin ffmpeg).
#   server/tests/convert_module.sh
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$(dirname "$HERE")")"
LUX="${LUX:-$ROOT/build/vendor/lux/lux}"
command -v ffmpeg >/dev/null || { echo "  skip  sin ffmpeg"; exit 0; }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/app"
printf 'import convert\nimport os\napp:\n    name "t"\n    port "5998"\n' > "$TMP/app/app.lux"
ffmpeg -y -v error -f lavfi -i sine=frequency=440:duration=1 "$TMP/in.wav"
python3 "$HERE/gen_png.py" 20 10 "$TMP/in.png"
T_DIR="$TMP" "$LUX" test "$TMP/app" "$HERE/convert_module_test.lux"
