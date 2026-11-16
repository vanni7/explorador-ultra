// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/search/Searcher.h"
#include "eu/tasks/Cancel.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace eu::index {

/// Quién saca el texto de un documento (la interfaz pone el del proceso auxiliar).
using TextProvider = std::function<core::Result<QString>(const QString &, const tasks::CancelToken &)>;

/// Lo que se sabe de un índice de contenido sin recorrerlo.
struct ContentIndexInfo
{
    QString root;
    qint64 documents = 0;   ///< Documentos anotados, con texto o sin él.
    qint64 withText = 0;    ///< De ellos, con texto.
    qint64 textChars = 0;   ///< Caracteres de texto guardados.
    qint64 updatedMs = 0;   ///< Cuándo terminó el último recorrido completo. 0 = nunca.
    bool complete = false;  ///< Se terminó de recorrer alguna vez.
};

/// Cómo va una puesta al día.
struct ContentSyncProgress
{
    qint64 found = 0;       ///< Documentos vistos en el disco.
    qint64 toRead = 0;      ///< Nuevos o cambiados (se sabe al terminar de recorrer).
    qint64 read = 0;        ///< De esos, ya leídos.
    qint64 failed = 0;      ///< Sin texto: dañados, protegidos, escaneados, demasiado lentos…
    qint64 removed = 0;     ///< Quitados porque ya no están.
    QString current;        ///< El que se está leyendo.
};

struct ContentSyncOptions
{
    TextProvider textProvider;    ///< Vacío: content::extractText en este proceso.
    QStringList excludedNames;    ///< Carpetas que no se recorren (la papelera).
    qint64 maxPlainBytes = 20 * 1000 * 1000;   ///< Igual que la búsqueda directa.
    std::function<void(const ContentSyncProgress &)> onProgress;
    const search::PauseGate *pause = nullptr;
};

/// Índice de texto completo de los documentos de una carpeta (SPEC §6.5):
/// el texto que se sacó de cada uno, en SQLite FTS5 con trigramas.
///
/// Solo acelera: la búsqueda consulta el texto guardado de los archivos cuyo
/// tamaño y fecha no han cambiado, y lee del disco el resto. Los resultados
/// son los mismos que sin índice, aunque esté a medio hacer o atrasado.
///
/// Como Index, solo guarda dónde está la base; cada llamada abre su conexión.
class ContentIndex
{
public:
    static core::Result<ContentIndex> open(const QString &databasePath, const QString &root);
    static void remove(const QString &databasePath);

    const QString &root() const { return m_root; }
    const QString &databasePath() const { return m_databasePath; }
    bool covers(const QString &path) const;

    ContentIndexInfo info() const;

    /// Recorre la carpeta, lee lo nuevo o cambiado y quita lo que ya no está.
    /// Guarda cada pocos documentos: si se corta, se sigue donde iba.
    core::Result<ContentSyncProgress> sync(const ContentSyncOptions &options, const tasks::CancelToken &cancel) const;

    /// La consulta para una búsqueda sobre estos índices; cada archivo se
    /// mira en el que lo cubre. Se llama desde el hilo de la búsqueda.
    static std::unique_ptr<search::ContentCache> cacheFor(const QList<ContentIndex> &indexes);

private:
    ContentIndex(QString databasePath, QString root)
        : m_databasePath(std::move(databasePath)), m_root(std::move(root))
    {
    }

    QString relativePath(const QString &path) const;

    QString m_databasePath;
    QString m_root;
};

} // namespace eu::index
