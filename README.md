# Explorador Ultra

Administrador de archivos nativo, rápido y de código abierto para Linux, Windows y macOS.
Licencia: **GPL-3.0-or-later**. Estado: **Fase 0** (arquitectura y entorno); todavía no es un explorador utilizable.

## Compilar en Ubuntu

```bash
sudo apt install build-essential cmake ninja-build pipx libgl-dev libxkbcommon-dev libxcb-cursor0
pipx install aqtinstall
aqt install-qt linux desktop 6.12.0 linux_gcc_64 -O ~/Qt -m qtimageformats qtmultimedia qtpdf qtshadertools
export EU_QT_DIR=$HOME/Qt/6.12.0/gcc_64
cmake --preset release && cmake --build --preset release && ctest --preset release
./build/release/bin/explorador-ultra
```

La documentación del proyecto está en [`docs/`](docs/): especificación, arquitectura, decisiones, estado y mediciones.
