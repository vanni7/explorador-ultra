#!/usr/bin/env python3
"""Descarga Qt para Windows (MSVC 2022, 64 bits) desde el repositorio oficial.

Existe porque aqtinstall 3.3.0 no encuentra Qt 6.12 para Windows: desde esa
versión la carpeta del repositorio incluye el compilador en el nombre
(qt6_6120_msvc2022_64) y aqtinstall todavía busca la antigua (qt6_6120).

Uso:  fetch_qt_windows.py <carpeta_destino> [versión, por defecto 6.12.0]
Deja Qt en <carpeta_destino>/<versión>/msvc2022_64. Requiere la orden 7z.
"""
import hashlib
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

# qtpdf vive en otro repositorio ("extensions"); se añadirá en la Fase 3.
MODULES = ["qtimageformats", "qtmultimedia", "qtshadertools"]
ARCH = "win64_msvc2022_64"
MIRROR = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop"


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=120) as response:
        return response.read()


def main() -> int:
    destination = Path(sys.argv[1]).resolve()
    version = sys.argv[2] if len(sys.argv) > 2 else "6.12.0"
    compact = version.replace(".", "")                      # 6.12.0 -> 6120
    base = f"{MIRROR}/qt6_{compact}/qt6_{compact}_msvc2022_64"

    wanted = {f"qt.qt6.{compact}.{ARCH}"}
    wanted |= {f"qt.qt6.{compact}.addons.{module}.{ARCH}" for module in MODULES}

    updates = ET.fromstring(fetch(f"{base}/Updates.xml"))
    downloads = destination / "descargas"
    downloads.mkdir(parents=True, exist_ok=True)
    target = destination / version / "msvc2022_64"
    target.mkdir(parents=True, exist_ok=True)

    found = set()
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

    missing = wanted - found
    if missing:
        print("ERROR: paquetes no encontrados:", ", ".join(sorted(missing)))
        return 1
    print(f"Qt {version} para Windows listo en {target}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
