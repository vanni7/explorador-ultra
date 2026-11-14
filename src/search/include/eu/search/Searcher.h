// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/fs/Entry.h"
#include "eu/search/Query.h"
#include "eu/tasks/Cancel.h"

#include <QString>
#include <QStringList>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>

namespace eu::search {

/// Un resultado: el elemento y la carpeta donde está.
struct Hit
{
    QString directory;   ///< Carpeta que lo contiene (ruta completa, con "/").
    fs::Entry entry;

    QString path() const;
};

/// Cómo va la búsqueda. Se entrega junto con cada lote de resultados.
struct SearchProgress
{
    qint64 foldersScanned = 0;      ///< Carpetas ya leídas.
    qint64 itemsExamined = 0;       ///< Elementos comparados con la consulta.
    qint64 hits = 0;                ///< Resultados encontrados hasta ahora.
    qint64 unreadableFolders = 0;   ///< Carpetas que no se pudieron leer (permisos…).
    qint64 filesRead = 0;           ///< Archivos abiertos para mirar su contenido.
    QString currentFolder;          ///< La que se está leyendo.
};

struct SearchOptions
{
    /// Entrar en carpetas ocultas y considerar los elementos ocultos.
    bool includeHidden = false;
    /// Entrar en carpetas que están en otra unidad (puntos de montaje). Por
    /// defecto no: buscar en "/" no debe recorrer todos los discos conectados.
    /// Solo se puede comprobar donde el sistema informa del dispositivo.
    bool crossDevices = false;
    /// Bajar a las subcarpetas. Sin esto solo se mira dentro de cada raíz.
    bool recursive = true;
    /// Al buscar en el contenido, los archivos mayores que esto no se leen.
    qint64 maxContentBytes = 20 * 1000 * 1000;
    /// Tope de resultados. Al alcanzarlo la búsqueda termina y se marca
    /// `truncated`: una consulta que lo cumple todo no debe agotar la memoria.
    qint64 maxHits = 200000;
    /// Los resultados se entregan cuando hay este número o pasa este tiempo.
    int batchSize = 2000;
    int maxBatchDelayMs = 100;
};

/// Permite detener temporalmente una búsqueda desde otro hilo.
class PauseGate
{
public:
    void setPaused(bool paused);
    bool isPaused() const;
    /// Si está en pausa, espera a que se reanude o se cancele.
    void waitWhilePaused(const tasks::CancelToken &cancel) const;

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_changed;
    bool m_paused = false;
};

struct SearchSummary
{
    SearchProgress progress;
    bool truncated = false;   ///< Se alcanzó maxHits: hay más resultados sin mostrar.
    bool cancelled = false;
};

/// Recibe cada lote de resultados (puede venir vacío: solo avance).
using HitsCallback = std::function<void(std::vector<Hit> &&, const SearchProgress &)>;

/// Recorre `roots` con sus subcarpetas y entrega lo que cumple la consulta,
/// según se va encontrando (SPEC §6.2: búsqueda en vivo, sin índice).
///
/// - No sigue enlaces simbólicos a carpetas: así no hay ciclos ni se sale del
///   árbol pedido. El enlace en sí se examina como un elemento más.
/// - Una carpeta que no se puede leer se cuenta y se sigue con las demás.
/// - Es bloqueante: se llama desde una tarea. Comprueba `cancel` y `pause`
///   entre carpeta y carpeta y entre lote y lote.
///
/// - Si la consulta mira el contenido (Query::usesContent), se leen los
///   archivos de texto (UTF-8, UTF-16 o Latin-1) de hasta `maxContentBytes`.
///   Los binarios (PDF, DOCX, imágenes…) no se leen: nunca cumplen.
///
/// `pause` puede ser nullptr.
SearchSummary searchTree(const QStringList &roots, const Query &query, const SearchOptions &options,
                         const tasks::CancelToken &cancel, const PauseGate *pause, const HitsCallback &onHits);

} // namespace eu::search
