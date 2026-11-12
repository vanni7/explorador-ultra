// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/fs/Entry.h"
#include "eu/tasks/Cancel.h"

#include <functional>
#include <vector>

namespace eu::fs {

struct ListOptions
{
    /// El primer lote es pequeño para pintar algo cuanto antes (SPEC §4:
    /// "primeros elementos visibles en < 300 ms").
    int firstBatchSize = 200;
    /// Los siguientes son grandes para no inundar de avisos al hilo de interfaz.
    int batchSize = 5000;
    /// Aunque el lote no esté lleno, se entrega pasado este tiempo. Así las
    /// carpetas lentas (red, discos dormidos) muestran avance.
    int maxBatchDelayMs = 100;
};

/// Recibe cada lote de elementos. Se llama desde el hilo que lista.
using BatchCallback = std::function<void(std::vector<Entry> &&)>;

/// Lista el contenido de una carpeta con la API nativa del sistema (ADR-0005)
/// y lo entrega por lotes, sin ordenar. No incluye "." ni "..".
///
/// Es bloqueante: debe llamarse desde una tarea del planificador, nunca desde
/// el hilo de interfaz. Comprueba `cancel` con frecuencia y, si se cancela,
/// devuelve un error ErrorCode::Cancelled.
core::Status listDirectory(const QString &path, const ListOptions &options,
                           const tasks::CancelToken &cancel, const BatchCallback &onBatch);

/// Versión cómoda para pruebas y carpetas pequeñas: lo devuelve todo junto.
core::Result<std::vector<Entry>> listDirectoryAll(const QString &path);

} // namespace eu::fs
