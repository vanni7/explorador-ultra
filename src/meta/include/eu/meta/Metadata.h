// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/tasks/Cancel.h"

#include <QList>
#include <QString>

namespace eu::meta {

struct Field
{
    QString label;
    QString value;
};

/// Un grupo de datos con su título ("Imagen", "Cámara", "Audio"…).
struct Section
{
    QString title;
    QList<Field> fields;
};

/// Los metadatos propios del tipo de archivo (SPEC §7.2): dimensiones y EXIF
/// de las fotos; duración, códecs y etiquetas del audio y el video; páginas
/// de un PDF. Solo lo que aplica y lo que el archivo trae: no hay campos
/// vacíos. Lo común a todos (nombre, tamaño, fechas, tipo) lo pone quien
/// muestra el panel, que sabe cómo formatearlo.
///
/// Bloqueante y lee el archivo: se llama desde una tarea. Un archivo dañado
/// da una lista más corta, nunca un fallo de la aplicación.
QList<Section> readMetadata(const QString &path, const tasks::CancelToken &cancel);

/// true si esta compilación lee el EXIF de las fotos (Exiv2).
bool hasExifSupport();
/// true si esta compilación lee los datos de audio y video (FFmpeg).
bool hasMediaSupport();

} // namespace eu::meta
