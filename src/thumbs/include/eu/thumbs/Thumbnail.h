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
    /// Es una carpeta: su miniatura muestra lo que hay dentro.
    bool isDirectory = false;
};

/// Tipos de archivo de los que esta versión sabe hacer miniatura.
enum class ThumbnailSource {
    None,     ///< Se muestra el icono del tipo.
    Image,    ///< JPEG, PNG, GIF, BMP, WebP, TIFF, SVG, ICO…
    Pdf,      ///< La primera página (si Qt tiene su complemento de PDF).
    Text,     ///< Las primeras líneas de un texto, código o Markdown.
    Folder,   ///< Hasta cuatro miniaturas de lo que contiene.
};

/// De qué clase sería la miniatura de este nombre de archivo. Solo mira la
/// extensión: no abre el archivo.
ThumbnailSource thumbnailSourceFor(const QString &fileName);
/// Igual, para una petición (que puede ser de una carpeta).
ThumbnailSource thumbnailSourceFor(const ThumbnailRequest &request);

/// Genera la miniatura. Imágenes: enteras, con su proporción, sin agrandar
/// lo que ya es pequeño y giradas según la orientación EXIF. PDF: la primera
/// página. Texto: una página con sus primeras líneas. Carpetas: si los
/// primeros archivos son todos de la misma clase (fotos, PDF…), hasta cuatro
/// de sus miniaturas en cuadrícula; si están mezclados o no hay archivos, no
/// hay miniatura y se ve el icono de carpeta.
///
/// Es bloqueante y lee el archivo: se llama desde una tarea. Un archivo
/// dañado devuelve un error, nunca cuelga ni cierra la aplicación (las
/// imágenes de tamaño absurdo se rechazan antes de decodificarlas).
core::Result<QImage> renderThumbnail(const ThumbnailRequest &request, const tasks::CancelToken &cancel);

} // namespace eu::thumbs
