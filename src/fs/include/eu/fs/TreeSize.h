// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/tasks/Cancel.h"

#include <QString>

#include <functional>

namespace eu::fs {

/// Lo que ocupa una carpeta con todo su contenido.
struct TreeSize
{
    qint64 bytes = 0;     ///< Suma de los tamaños de los archivos.
    qint64 files = 0;     ///< Archivos y enlaces.
    qint64 folders = 0;   ///< Subcarpetas (sin contar la propia carpeta).
    /// true si alguna subcarpeta no se pudo leer (permisos, desconexión): el
    /// total es entonces un mínimo, no el tamaño real.
    bool incomplete = false;
    bool cancelled = false;
};

/// Recibe el total acumulado mientras se calcula. Se llama desde el hilo que mide.
using TreeSizeCallback = std::function<void(const TreeSize &)>;

/// Suma el tamaño de todo lo que hay bajo `path`. Los enlaces simbólicos no se
/// siguen (cuentan como un archivo de tamaño cero), así que no hay ciclos ni
/// se sale de la carpeta. Es bloqueante: debe llamarse desde una tarea.
/// `onProgress` recibe el acumulado cada vez que se termina de leer una subcarpeta.
TreeSize measureTree(const QString &path, const tasks::CancelToken &cancel, const TreeSizeCallback &onProgress = {});

} // namespace eu::fs
