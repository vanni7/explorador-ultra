# Dependencias

Todas son gratuitas y compatibles con GPL-3.0-or-later. "En uso" indica si el código ya la enlaza.
Versión = la instalada en el equipo de desarrollo (Ubuntu 26.04) el 2026-11-11.

| Biblioteca | Versión | Licencia | Uso | Enlace | En uso |
|---|---|---|---|---|---|
| Qt (Core, Gui, Widgets, Svg, Test) | 6.12.0 | LGPL-3.0 / GPL-3.0 | Base de la aplicación, SVG, pruebas | dinámico | **Sí** |
| Qt Multimedia, Qt PDF, Qt Image Formats | 6.12.0 | LGPL-3.0 / GPL-3.0 | Reproducir en la miniatura; PDF (complemento de imagen); TIFF/WebP | dinámico | **Sí**, opcionales (Fase 3). Qt Multimedia trae su propio FFmpeg 9.0, que convive con el del sistema (bibliotecas y símbolos con versiones distintas) |
| SQLite (incluido en Qt SQL, controlador QSQLITE) | 3.53.4 | Dominio público | Historial, índice, etiquetas, FTS5 | dentro del complemento de Qt | **Sí** (ADR-0011) |
| libarchive | 3.8.5 | BSD-2-Clause | ZIP, 7z, TAR, GZ, ISO; lectura de RAR; miniatura incrustada de los documentos de oficina | dinámico / vcpkg | **Sí**, opcional (Fase 3); el resto en la Fase 4 |
| FFmpeg (avformat, avcodec, swscale) | 8.0.1 | LGPL-2.1+ (GPL según opciones) | Fotogramas de video, carátulas, metadatos multimedia | dinámico / vcpkg | **Sí**, opcional, 6.1 o posterior |
| Exiv2 | 0.28.8 | GPL-2.0-or-later | EXIF, XMP, IPTC | dinámico / vcpkg | **Sí**, opcional |
| TagLib | 2.2.1 | LGPL-2.1 / MPL-1.1 | Etiquetas de audio (ID3, etc.) | dinámico / vcpkg | **No**: FFmpeg ya lee las etiquetas; se reconsiderará si hace falta escribirlas (Fase 4, etiquetas) |
| LibRaw (`libraw_r`) | 0.21.5 | LGPL-2.1 / CDDL-1.0 | Imágenes RAW | dinámico / vcpkg | **Sí**, opcional |
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

Las bibliotecas "opcionales" se detectan en `cmake/EuOptionalDeps.cmake`: si falta alguna, la aplicación compila igual y esa función queda desactivada (se ve el icono del tipo de archivo). LibreOffice tampoco se enlaza: se usa como programa externo si está instalado.

## OpenSSL `libcrypto` (opcional, Fase 4)
SHA-256, SHA-1 y MD5 rápidos (instrucciones del procesador) para comprobar las copias. Si falta, se usa `QCryptographicHash`, unas 13 veces más lento en SHA-256. Licencia Apache 2.0 (OpenSSL 3), compatible con GPL-3. Ubuntu: `libssl-dev`.
