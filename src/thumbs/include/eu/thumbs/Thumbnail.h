// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/tasks/Cancel.h"

#include <QImage>
#include <QString>

namespace eu::thumbs {

/// Qué miniatura se quiere. El tamaño y la fecha forman parte de la petición:
/// si el archivo cambia, la miniatura guardada deja de valer (SPEC §5.5).
struct ThumbnailRequest
{
    QString path;
    qint64 size = 0;
    qint64 modifiedMs = 0;
    /// Lado mayor de la miniatura, en píxeles físicos.
    int maxSide = 256;
};

/// Tipos de archivo de los que esta versión sabe hacer miniatura.
enum class ThumbnailSource {
    None,    ///< Se muestra el icono del tipo.
    Image,   ///< JPEG, PNG, GIF, BMP, WebP, TIFF, SVG, ICO…
};

/// De qué clase sería la miniatura de este nombre de archivo. Solo mira la
/// extensión: no abre el archivo.
ThumbnailSource thumbnailSourceFor(const QString &fileName);

/// Genera la miniatura: la imagen entera, con su proporción, sin agrandar
/// lo que ya es pequeño y girada según la orientación EXIF.
///
/// Es bloqueante y lee el archivo: se llama desde una tarea. Un archivo
/// dañado devuelve un error, nunca cuelga ni cierra la aplicación (las
/// imágenes de tamaño absurdo se rechazan antes de decodificarlas).
core::Result<QImage> renderThumbnail(const ThumbnailRequest &request, const tasks::CancelToken &cancel);

} // namespace eu::thumbs
