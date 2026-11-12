#!/usr/bin/env bash
# Genera los iconos de cada plataforma a partir del SVG maestro (SPEC §5.1).
# Requiere: rsvg-convert (librsvg2-bin), icotool (icoutils) y png2icns (icnsutils).
set -euo pipefail
cd "$(dirname "$0")/../assets/icon"

svg=explorador-ultra.svg
mkdir -p png

# PNG en los tamaños freedesktop (Linux).
for size in 16 22 24 32 48 64 128 256 512 1024; do
    rsvg-convert -w "$size" -h "$size" "$svg" -o "png/explorador-ultra-$size.png"
done

# .ico de Windows (varios tamaños dentro de un mismo archivo).
icotool -c -o explorador-ultra.ico png/explorador-ultra-{16,24,32,48,64,128,256}.png

# .icns de macOS.
png2icns explorador-ultra.icns png/explorador-ultra-{16,32,48,128,256,512,1024}.png > /dev/null

echo "Iconos generados en $(pwd)"
