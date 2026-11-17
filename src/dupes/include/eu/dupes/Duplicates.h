// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/tasks/Cancel.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>

namespace eu::search {
class PauseGate;
}

namespace eu::dupes {

/// Un archivo de un grupo de duplicados.
struct File
{
    QString path;
    qint64 modifiedMs = 0;
    std::uint64_t deviceId = 0;
    std::uint64_t fileId = 0;   ///< Inodo. Dos rutas con el mismo son el mismo archivo (enlace físico).
    /// Índice de la copia física a la que pertenece dentro del grupo: los
    /// enlaces físicos de un mismo archivo comparten número.
    int physical = 0;
};

/// Archivos con exactamente el mismo contenido.
struct Group
{
    qint64 size = 0;
    QList<File> files;
    int physicalCopies = 0;   ///< Copias reales (los enlaces físicos no cuentan aparte).
    /// Lo que se libera dejando una sola copia real.
    qint64 recoverable() const { return size * std::max(0, physicalCopies - 1); }
};

struct Options
{
    QStringList roots;
    qint64 minSize = 1;          ///< Los vacíos son todos "iguales": no se buscan.
    bool includeHidden = false;
    bool byteByByte = false;     ///< Además del hash, comparar byte a byte (SPEC §14.1, etapa 4).
    const search::PauseGate *pause = nullptr;
};

/// Cómo va la búsqueda.
struct Progress
{
    enum class Stage { Listing, BySize, PartialHash, FullHash, Bytes, Done };
    Stage stage = Stage::Listing;
    qint64 filesSeen = 0;
    qint64 candidates = 0;      ///< Archivos que siguen en la etapa actual.
    qint64 done = 0;            ///< De ellos, ya comprobados.
    qint64 bytesRead = 0;
};

struct Result
{
    QList<Group> groups;         ///< De mayor a menor espacio recuperable.
    qint64 filesSeen = 0;
    qint64 bytesRead = 0;        ///< Lo leído para comparar.
    int hardlinkOnlySets = 0;    ///< Conjuntos que eran solo enlaces físicos (no ocupan espacio de más).
    qint64 recoverable() const;
};

/// Busca duplicados exactos (SPEC §14.1) por etapas: tamaño, hash parcial
/// (inicio, medio y final), hash completo (SHA-256) y, si se pide, byte a
/// byte. No sigue enlaces simbólicos ni entra en otras unidades montadas.
/// Solo lee: nunca borra nada.
Result findDuplicates(const Options &options, const tasks::CancelToken &cancel,
                      const std::function<void(const Progress &)> &onProgress = {});

/// Reglas para marcar qué se quita de cada grupo (siempre queda una copia real).
enum class Keep {
    Oldest,      ///< Conservar el más antiguo.
    Newest,      ///< Conservar el más reciente.
    Preferred,   ///< Conservar el que esté en `preferredPath` (si ninguno, el más antiguo).
    ShortestPath,
};

/// Las rutas que la regla propone quitar de un grupo. Nunca todas las copias
/// reales; los enlaces físicos de la copia que se conserva tampoco (no
/// liberarían nada).
QStringList proposeRemoval(const Group &group, Keep keep, const QString &preferredPath = {});

} // namespace eu::dupes
