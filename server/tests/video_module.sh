#!/usr/bin/env bash
#
# Prueba del módulo nativo `inpaint.video` de Lux con ffmpeg de verdad (se salta sin ffmpeg ni ImageMagick):
# un vídeo con un logo que se mueve, la marca señalada en el primer fotograma, modo rápido (sin LaMa). Comprueba
# que sale un vídeo del mismo tamaño y con audio, y que el logo ha desaparecido de TODOS los fotogramas.
#   server/tests/video_module.sh
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$(dirname "$HERE")")"
LUX="${LUX:-$ROOT/build/vendor/lux/lux}"
for t in ffmpeg ffprobe convert python3; do command -v "$t" >/dev/null || { echo "  skip  sin $t"; exit 0; }; done
FONT=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf
[ -f "$FONT" ] || { echo "  skip  sin la fuente DejaVu"; exit 0; }
python3 -c "import numpy" 2>/dev/null || { echo "  skip  sin numpy"; exit 0; }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/app"
printf 'import inpaint\nimport os\napp:\n    name "t"\n    port "5996"\n' > "$TMP/app/app.lux"
POS="x='60+260*abs(sin(t*0.9))':y='40+200*abs(cos(t*0.7))'"
BG="mandelbrot=size=640x360:rate=25:end_pts=100:start_scale=1:end_scale=0.02"
ffmpeg -y -v error -f lavfi -i "$BG" -f lavfi -i sine=frequency=440:duration=3 -t 3 -vf "drawtext=fontfile=$FONT:text='WATERMARK':fontsize=40:fontcolor=white@0.8:borderw=2:bordercolor=black@0.8:$POS" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$TMP/wm.mp4"
ffmpeg -y -v error -f lavfi -i "$BG" -t 3 -c:v libx264 -pix_fmt yuv420p "$TMP/clean.mp4"
# Dónde está el logo en cada fotograma (solo el logo, sobre negro), y su recuadro en el primero como máscara.
ffmpeg -y -v error -f lavfi -i color=black:size=640x360:rate=25:duration=3 -vf "drawtext=fontfile=$FONT:text='WATERMARK':fontsize=40:fontcolor=white:borderw=2:bordercolor=white:$POS" -c:v libx264 -pix_fmt yuv444p -crf 0 "$TMP/logo.mp4"
ffmpeg -y -v error -i "$TMP/logo.mp4" -frames:v 1 "$TMP/logo0.png"
BB="$(convert "$TMP/logo0.png" -threshold 20% -format '%@' info:)"
python3 - "$BB" "$TMP" <<'PY'
import re, subprocess, sys
w, h, x, y = map(int, re.match(r"(\d+)x(\d+)\+(\d+)\+(\d+)", sys.argv[1]).groups())
subprocess.run(["convert", "-size", "640x360", "xc:black", "-fill", "white", "-draw", f"rectangle {x-6},{y-6} {x+w+6},{y+h+6}", "-depth", "8", sys.argv[2] + "/mask.pgm"], check=True)
PY
T_DIR="$TMP" "$LUX" test "$TMP/app" "$HERE/video_module_test.lux" || exit 1
python3 - "$TMP" <<'PY'
import subprocess, sys, numpy as np
d = sys.argv[1]
def frames(f):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", f"{d}/{f}", "-f", "rawvideo", "-pix_fmt", "gray", "-"], capture_output=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, 360, 640).astype(float)
logo, clean, wm, out = frames("logo.mp4") > 60, frames("clean.mp4"), frames("wm.mp4"), frames("out.mp4")
assert len(out) == len(wm) == 75, (len(out), len(wm))
err = lambda v: np.mean([np.abs(v[i] - clean[i])[logo[i]].mean() for i in range(75)])
worst = max(np.abs(out[i] - clean[i])[logo[i]].mean() for i in range(75))
print(f"  error en la zona del logo: con marca {err(wm):.1f}, limpio {err(out):.1f} (peor fotograma {worst:.1f})")
assert err(out) < err(wm) / 3 and worst < 45, "el logo no se ha quitado en todos los fotogramas"
audio = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a", "-show_entries", "stream=codec_name", "-of", "csv=p=0", f"{d}/out.mp4"], capture_output=True, text=True).stdout.strip()
assert audio == "aac", "se ha perdido el audio"
print("  ok    el logo que se mueve desaparece en los 75 fotogramas y el audio sigue")
PY
