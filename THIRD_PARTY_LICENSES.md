# Licencias de terceros

Explorador Ultra se distribuye bajo **GPL-3.0-or-later** (ver `LICENSE`). Los paquetes incluyen, o usan del sistema, las bibliotecas de abajo. Todas son compatibles con GPL-3.0. El texto completo de cada licencia está en el sitio de cada proyecto y, para Qt, en la carpeta de la instalación de Qt (`Licenses/`).

| Componente | Licencia | Dónde va | Proyecto |
|---|---|---|---|
| Qt 6.12 (Core, Gui, Widgets, Svg, Sql, PrintSupport, Multimedia, Pdf, Network, DBus, WaylandClient, Image Formats y sus complementos) | LGPL-3.0 (GPL-3.0 en lo que no es LGPL) | Dentro de todos los paquetes, como bibliotecas dinámicas sin modificar | https://www.qt.io/ · código: https://download.qt.io/official_releases/qt/ |
| FFmpeg que trae Qt Multimedia | LGPL-2.1-or-later | Dentro de todos los paquetes (lo instala Qt) | https://ffmpeg.org/ |
| PDFium (dentro de Qt PDF) | BSD-3-Clause / Apache-2.0 | Dentro de Qt PDF | https://pdfium.googlesource.com/pdfium/ |
| ICU (usado por Qt en Linux) | Unicode-3.0 | Dentro del .deb y del AppImage | https://icu.unicode.org/ |
| SQLite (controlador QSQLITE de Qt) | Dominio público | Dentro de Qt | https://sqlite.org/ |
| libarchive | BSD-2-Clause | .deb: del sistema · AppImage y Windows: dentro | https://libarchive.org/ |
| FFmpeg del sistema (avformat, avcodec, avutil, swscale) | LGPL-2.1-or-later (GPL según cómo lo compile la distribución) | .deb: del sistema · AppImage: dentro · Windows y macOS: no se usa | https://ffmpeg.org/ |
| LibRaw | LGPL-2.1 / CDDL-1.0 | .deb: del sistema · AppImage: dentro | https://www.libraw.org/ |
| Exiv2 | GPL-2.0-or-later | .deb: del sistema · AppImage: dentro | https://exiv2.org/ |
| OpenSSL 3 (`libcrypto`) | Apache-2.0 | .deb: del sistema · AppImage: dentro | https://www.openssl.org/ |
| zlib, bzip2, xz/liblzma, zstd, lz4 (los usa libarchive) | zlib / bzip2 / 0BSD-dominio público / BSD-3-Clause / BSD-2-Clause | Según libarchive | — |
| Entorno de ejecución de Microsoft Visual C++ | Licencia de redistribución de Microsoft | Dentro del instalador de Windows | https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files |

Notas:
- Qt se enlaza de forma dinámica y sin cambios: se puede sustituir por otra versión compatible reemplazando sus bibliotecas en la carpeta de la aplicación (requisito de la LGPL).
- Exiv2 es GPL; como la aplicación ya es GPL-3.0-or-later, no hay conflicto.
- El icono de la aplicación es original del proyecto (misma licencia que el código).
