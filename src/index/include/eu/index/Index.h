// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/search/Query.h"
#include "eu/search/Searcher.h"
#include "eu/tasks/Cancel.h"

#include <QString>
#include <QStringList>

#include <functional>

namespace eu::index {

/// Lo que se sabe de un índice sin recorrerlo.
struct IndexInfo
{
    QString root;           ///< Carpeta (normalmente una unidad) que cubre.
    qint64 entries = 0;     ///< Archivos y carpetas anotados.
    qint64 folders = 0;     ///< Carpetas recorridas.
    qint64 updatedMs = 0;   ///< Cuándo terminó el último recorrido completo. 0 = nunca.
    /// true si alguna vez se recorrió entero. Hasta entonces no sirve para buscar.
    bool complete = false;
};

/// Cómo va una sincronización del índice con el disco.
struct SyncProgress
{
    qint64 foldersScanned = 0;
    qint64 entriesSeen = 0;
    qint64 added = 0;
    qint64 removed = 0;     ///< Filas quitadas directamente (sin contar lo que colgaba de una carpeta borrada).
    qint64 updated = 0;
    qint64 unreadableFolders = 0;
    QString currentFolder;
};

struct SyncOptions
{
    /// true: recorre todo lo que cuelga de la carpeta (construir el índice o
    /// ponerlo al día). false: compara solo esa carpeta y baja únicamente a
    /// las subcarpetas que acaban de aparecer (aviso de un cambio).
    bool wholeTree = true;
    /// Nombres que no se anotan ni se recorren (la carpeta de la papelera).
    QStringList excludedNames;
    /// Se llama con cada carpeta recorrida: es donde se pide vigilarla.
    std::function<void(const QString &)> onFolder;
    /// Avance, unas pocas veces por segundo.
    std::function<void(const SyncProgress &)> onProgress;
    const search::PauseGate *pause = nullptr;
};

/// Índice de nombres de una carpeta y todo lo que cuelga de ella, guardado en
/// una base SQLite (SPEC §6.4). Anota nombre, tamaño, fechas y tipo; no el
/// contenido de los archivos.
///
/// Igual que la búsqueda directa, no sigue enlaces a carpetas ni entra en
/// otras unidades montadas dentro.
///
/// El objeto solo guarda dónde está la base: cada llamada abre su propia
/// conexión (ADR-0011), así que se puede copiar y usar desde cualquier hilo.
/// Dos sincronizaciones a la vez sobre el mismo índice se estorban (una espera
/// a la otra): quien lo use debe lanzarlas de una en una. Buscar mientras se
/// sincroniza sí es correcto y no bloquea.
class Index
{
public:
    /// Abre el índice de `root` guardado en `databasePath`, creándolo vacío si
    /// no existe. Si el archivo era de otra carpeta o de otra versión, se vacía.
    static core::Result<Index> open(const QString &databasePath, const QString &root);
    /// Borra los archivos de un índice.
    static void remove(const QString &databasePath);

    const QString &root() const { return m_root; }
    const QString &databasePath() const { return m_databasePath; }

    IndexInfo info() const;

    /// Pone el índice de acuerdo con el disco a partir de `folder` (que debe
    /// ser la raíz o estar dentro de ella). Bloqueante: se llama desde una tarea.
    core::Result<SyncProgress> sync(const QString &folder, const SyncOptions &options,
                                    const tasks::CancelToken &cancel) const;

    /// true si `path` es la raíz o está dentro de ella.
    bool covers(const QString &path) const;

    /// true si esta búsqueda se puede contestar con el índice y daría lo mismo
    /// que recorrer el disco (si el índice está al día).
    bool canAnswer(const QStringList &roots, const search::Query &query, const search::SearchOptions &options) const;

    /// Busca en el índice lo que cuelga de `roots`. Misma forma de entregar
    /// que search::searchTree; `foldersScanned` queda en 0 porque no se lee
    /// ninguna carpeta del disco.
    search::SearchSummary search(const QStringList &roots, const search::Query &query,
                                 const search::SearchOptions &options, const tasks::CancelToken &cancel,
                                 const search::HitsCallback &onHits) const;

private:
    Index(QString databasePath, QString root) : m_databasePath(std::move(databasePath)), m_root(std::move(root)) {}

    /// Ruta relativa a la raíz ("" para la propia raíz).
    QString relativePath(const QString &path) const;
    QString absolutePath(const QString &relative) const;

    QString m_databasePath;
    QString m_root;
};

} // namespace eu::index
