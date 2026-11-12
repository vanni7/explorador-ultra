# Dependencias

Todas son gratuitas y compatibles con GPL-3.0-or-later. "En uso" indica si el código ya la enlaza.
Versión = la instalada en el equipo de desarrollo (Ubuntu 26.04) el 2026-11-11.

| Biblioteca | Versión | Licencia | Uso | Enlace | En uso |
|---|---|---|---|---|---|
| Qt (Core, Gui, Widgets, Svg, Test) | 6.12.0 | LGPL-3.0 / GPL-3.0 | Base de la aplicación, SVG, pruebas | dinámico | **Sí** |
| Qt Multimedia, Qt PDF, Qt Image Formats | 6.12.0 | LGPL-3.0 / GPL-3.0 | Audio/video, PDF, TIFF/WebP | dinámico | Fase 3 |
| SQLite | 3.46.1 | Dominio público | Historial, índice, etiquetas, FTS5 | dinámico (Linux) / vcpkg | Fase 1 |
| libarchive | 3.8.5 | BSD-2-Clause | ZIP, 7z, TAR, GZ, ISO; lectura de RAR | dinámico / vcpkg | Fase 4 |
| FFmpeg (avformat, avcodec, swscale) | 8.0.1 | LGPL-2.1+ (GPL según opciones) | Fotogramas de video, metadatos multimedia | dinámico / vcpkg | Fase 3 |
| Exiv2 | 0.28.8 | GPL-2.0-or-later | EXIF, XMP, IPTC | dinámico / vcpkg | Fase 3 |
| TagLib | 2.2.1 | LGPL-2.1 / MPL-1.1 | Etiquetas de audio (ID3, etc.) | dinámico / vcpkg | Fase 3 |
| LibRaw | 0.21.5 | LGPL-2.1 / CDDL-1.0 | Imágenes RAW | dinámico / vcpkg | Fase 3 |
| xxHash | 0.8.3 | BSD-2-Clause | Hash parcial rápido (duplicados) | dinámico / vcpkg | Fase 4 |
| zlib | 1.3.x | zlib | CRC32 | dinámico / vcpkg | Fase 4 |
| OpenSSL | 3.x | Apache-2.0 | Solo si `QCryptographicHash` no rinde lo suficiente | — | Por decidir (Fase 4) |

Notas de licencia:
- Apache-2.0 (OpenSSL) es compatible con GPL-3, no con GPL-2; al ser el proyecto GPL-3.0-or-later no hay conflicto.
- Exiv2 es GPL: obliga a que la aplicación sea GPL, que ya lo es.
- El lector de RAR de libarchive es código propio bajo BSD (no usa unrar, cuya licencia no es libre). Su compatibilidad con RAR5 cifrado o de volúmenes múltiples es limitada: se probará en la Fase 4.
- FFmpeg se usará en su variante LGPL/GPL sin componentes "nonfree".
- LibreOffice (opcional, §1) no se enlaza: se invoca como programa externo si el usuario lo tiene instalado.

Herramientas de desarrollo (no se distribuyen): CMake, Ninja, GCC, Clang, clang-tidy, clang-format, ccache, Valgrind, hyperfine, aqtinstall, rsvg-convert, icoutils, icnsutils. Detalle en `ENVIRONMENT.md`.
