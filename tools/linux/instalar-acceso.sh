#!/bin/sh
# Crea el acceso «Explorador Ultra» en el menú de aplicaciones (solo para este
# usuario) apuntando a la versión compilada con el preset release, con su icono
# en todos los tamaños. Uso: tools/linux/instalar-acceso.sh
set -e
proyecto=$(cd "$(dirname "$0")/../.." && pwd)
iconos="$HOME/.local/share/icons/hicolor"
for lado in 16 22 24 32 48 64 128 256 512; do
    mkdir -p "$iconos/${lado}x${lado}/apps"
    cp "$proyecto/assets/icon/png/explorador-ultra-$lado.png" "$iconos/${lado}x${lado}/apps/explorador-ultra.png"
done
mkdir -p "$iconos/scalable/apps"
cp "$proyecto/assets/icon/explorador-ultra.svg" "$iconos/scalable/apps/explorador-ultra.svg"
[ -f "$iconos/index.theme" ] || cp /usr/share/icons/hicolor/index.theme "$iconos/" 2>/dev/null || true
gtk-update-icon-cache -f -t "$iconos" >/dev/null 2>&1 || true
mkdir -p "$HOME/.local/share/applications"
cat > "$HOME/.local/share/applications/explorador-ultra.desktop" <<FIN
[Desktop Entry]
Type=Application
Name=Explorador Ultra
Comment=Administrador de archivos (versión de desarrollo)
Exec=$proyecto/build/release/bin/explorador-ultra %U
Icon=explorador-ultra
Terminal=false
Categories=System;FileTools;FileManager;
StartupNotify=true
StartupWMClass=explorador-ultra
FIN
update-desktop-database "$HOME/.local/share/applications" >/dev/null 2>&1 || true
echo "Acceso creado: busca «Explorador Ultra» en el menú de aplicaciones."
