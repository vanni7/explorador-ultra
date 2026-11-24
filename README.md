# Explorador Ultra

Administrador de archivos nativo, rápido y de código abierto para Linux, Windows y macOS.
Licencia: **GPL-3.0-or-later** (bibliotecas de terceros en [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md)). Estado: **Fase 6** (optimización y distribución); el detalle está en [`docs/STATUS.md`](docs/STATUS.md).

## Instalar
Los paquetes para Ubuntu (.deb y AppImage), Windows (instalador) y macOS (.dmg) están en [Releases](https://github.com/vanni7/explorador-ultra/releases). Cómo instalarlos y qué hacer con los avisos de SmartScreen y Gatekeeper: [`docs/DISTRIBUCION.md`](docs/DISTRIBUCION.md).

## Compilar en Ubuntu

```bash
sudo apt install build-essential cmake ninja-build pipx libgl-dev libxkbcommon-dev libxcb-cursor0
pipx install aqtinstall
aqt install-qt linux desktop 6.12.0 linux_gcc_64 -O ~/Qt -m qtimageformats qtmultimedia qtpdf qtshadertools
export EU_QT_DIR=$HOME/Qt/6.12.0/gcc_64
cmake --preset release && cmake --build --preset release && ctest --preset release
./build/release/bin/explorador-ultra
```

Dependencias opcionales (si faltan, compila igual y esa función queda desactivada): `libarchive-dev libavformat-dev libavcodec-dev libswscale-dev libavutil-dev libraw-dev libexiv2-dev libssl-dev`. Windows y la VM de pruebas: [`docs/ENVIRONMENT.md`](docs/ENVIRONMENT.md). Hacer los paquetes: [`docs/DISTRIBUCION.md`](docs/DISTRIBUCION.md).

La documentación del proyecto está en [`docs/`](docs/): especificación, arquitectura, decisiones, estado y mediciones.
