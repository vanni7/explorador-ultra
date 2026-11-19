#!/usr/bin/env python3
"""Descarga Qt para Windows (MSVC 2022, 64 bits) desde el repositorio oficial.

Existe porque aqtinstall 3.3.0 no encuentra Qt 6.12 para Windows: desde esa
versión la carpeta del repositorio incluye el compilador en el nombre
(qt6_6120_msvc2022_64) y aqtinstall todavía busca la antigua (qt6_6120).

Uso:  fetch_qt_windows.py <carpeta_destino> [versión, por defecto 6.12.0]
Deja Qt en <carpeta_destino>/<versión>/msvc2022_64. Requiere la orden 7z.
Qt PDF no está en el repositorio "desktop" sino en "extensions"; se baja de allí.
"""
import hashlib
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

MODULES = ["qtimageformats", "qtmultimedia", "qtshadertools"]
EXTENSIONS = ["qtpdf"]
ARCH = "win64_msvc2022_64"
REPOSITORY = "https://download.qt.io/online/qtsdkrepository/windows_x86"


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=120) as response:
        return response.read()


def main() -> int:
    destination = Path(sys.argv[1]).resolve()
    version = sys.argv[2] if len(sys.argv) > 2 else "6.12.0"
    compact = version.replace(".", "")                      # 6.12.0 -> 6120

    # (carpeta del repositorio, paquetes que se quieren de ella)
    sources = [(f"{REPOSITORY}/desktop/qt6_{compact}/qt6_{compact}_msvc2022_64",
                {f"qt.qt6.{compact}.{ARCH}"}
                | {f"qt.qt6.{compact}.addons.{module}.{ARCH}" for module in MODULES})]
    sources += [(f"{REPOSITORY}/extensions/{module}/{compact}/msvc2022_64", {f"extensions.{module}.{compact}.{ARCH}"})
                for module in EXTENSIONS]

    downloads = destination / "descargas"
    downloads.mkdir(parents=True, exist_ok=True)
    target = destination / version / "msvc2022_64"
    target.mkdir(parents=True, exist_ok=True)

    wanted = set()
    found = set()
    for base, names in sources:
        wanted |= names
        status = install(base, names, downloads, target, found)
        if status != 0:
            return status

    missing = wanted - found
    if missing:
        print("ERROR: paquetes no encontrados:", ", ".join(sorted(missing)))
        return 1
    print(f"Qt {version} para Windows listo en {target}")
    return 0


def install(base: str, wanted: set, downloads: Path, target: Path, found: set) -> int:
    updates = ET.fromstring(fetch(f"{base}/Updates.xml"))
    for package in updates.iter("PackageUpdate"):
        name = package.findtext("Name")
        if name not in wanted:
            continue
        found.add(name)
        package_version = package.findtext("Version")
        archives = [a.strip() for a in (package.findtext("DownloadableArchives") or "").split(",") if a.strip()]
        for archive in archives:
            url = f"{base}/{name}/{package_version}{archive}"
            local = downloads / archive
            if not local.exists():
                print(f"Descargando {archive}", flush=True)
                data = fetch(url)
                # Cada archivo se comprueba contra la suma SHA-1 que publica Qt.
                expected = fetch(url + ".sha1").decode().split()[0]
                if hashlib.sha1(data).hexdigest() != expected:
                    print(f"ERROR: la suma de {archive} no coincide")
                    return 1
                local.write_bytes(data)
            subprocess.run(["7z", "x", "-y", "-bso0", "-bsp0", f"-o{target}", str(local)], check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
