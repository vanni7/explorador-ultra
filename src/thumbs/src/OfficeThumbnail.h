// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/thumbs/Thumbnail.h"

namespace eu::thumbs::detail {

/// true si sabe leer documentos de oficina por dentro (libarchive).
bool hasOfficeSupport();
/// Ruta del programa de LibreOffice, o vacía si no está instalado.
QString libreOfficeProgram();

/// Primera página de un documento de oficina (SPEC §5.5):
///  1. la miniatura que el propio archivo lleva dentro (ODF siempre; Word,
///     Excel y PowerPoint si se guardó con ella);
///  2. si no la lleva, LibreOffice en un proceso aparte, de uno en uno, con
///     un perfil propio y un tiempo máximo. Nunca ejecuta macros.
core::Result<QImage> renderOffice(const ThumbnailRequest &request, const tasks::CancelToken &cancel);

/// Fotos RAW de cámara: la vista previa que guarda la cámara dentro del
/// archivo, girada según la cámara (LibRaw).
bool hasRawSupport();
core::Result<QImage> renderRaw(const ThumbnailRequest &request, const tasks::CancelToken &cancel);

} // namespace eu::thumbs::detail
