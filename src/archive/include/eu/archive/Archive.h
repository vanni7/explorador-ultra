// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/tasks/Cancel.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace eu::archive {

/// Formatos en los que se puede crear un comprimido (SPEC §15). RAR e ISO
/// solo se leen.
enum class Format {
    Zip,
    SevenZip,
    Tar,
    TarGz,
};

/// true si esta compilación sabe leer y crear comprimidos (libarchive).
bool isAvailable();

/// true si el nombre tiene pinta de comprimido que se puede abrir (ZIP, 7z,
/// RAR, TAR y sus variantes comprimidas, GZ, ISO…). Solo mira la extensión.
bool isArchiveName(const QString &fileName);

/// Extensión con la que se guarda cada formato (".zip", ".tar.gz"…).
QString extensionOf(Format format);

/// Un elemento dentro del comprimido.
struct ArchiveEntry
{
    QString path;             ///< Ruta dentro del comprimido, con "/" y sin "/" final.
    qint64 size = 0;          ///< Tamaño sin comprimir (0 si es carpeta o no se sabe).
    qint64 modifiedMs = 0;    ///< 0 = desconocida.
    bool isDirectory = false;
    bool isSymlink = false;
    QString linkTarget;       ///< Destino del enlace, tal como está en el comprimido.
    bool encrypted = false;   ///< Cifrado: no se puede leer sin contraseña.
};

/// Topes contra comprimidos hechos para agotar el disco o la memoria
/// ("bombas", SPEC §15). Al pasarse, se detiene y se informa.
struct Limits
{
    qint64 maxEntries = 1000000;
    /// Lo que se puede llegar a escribir al extraer, en bytes.
    qint64 maxTotalBytes = 100LL * 1000 * 1000 * 1000;
    /// Cuántas veces puede ser lo extraído mayor que el comprimido. Solo se
    /// aplica pasado `ratioCheckFromBytes`: un archivo pequeño de texto se
    /// comprime muchísimo sin ser una bomba.
    qint64 maxRatio = 1000;
    qint64 ratioCheckFromBytes = 512LL * 1000 * 1000;
};

/// Lee la lista de lo que contiene. Añade las carpetas que el comprimido no
/// lista por separado (un ZIP puede tener "a/b/c.txt" sin "a/" ni "a/b/").
/// Las entradas con rutas peligrosas se listan tal cual: es al extraer
/// cuando se rechazan.
core::Result<QList<ArchiveEntry>> listArchive(const QString &archivePath, const tasks::CancelToken &cancel,
                                              const Limits &limits = {});

struct ExtractOptions
{
    /// Solo estas rutas internas (una carpeta incluye lo que contiene).
    /// Vacía: todo.
    QStringList only;
    Limits limits;
};

struct ExtractResult
{
    int files = 0;
    int folders = 0;
    qint64 bytes = 0;
    /// Entradas omitidas por seguridad o porque ya existía algo con ese nombre,
    /// con el motivo ("../x: sale de la carpeta de destino").
    QStringList skipped;
};

/// Recibe los bytes escritos hasta ahora.
using ProgressCallback = std::function<void(qint64 bytesWritten)>;

/// Extrae en `destination`, que debe existir. Nunca escribe fuera de ella:
/// rechaza rutas absolutas, con "..", y enlaces que apuntan fuera. Nunca
/// sobrescribe: lo que ya exista se omite y se informa. Si se pasa de los
/// topes o se cancela, borra el archivo que estaba a medias y devuelve el
/// error (lo ya extraído se queda).
core::Result<ExtractResult> extractArchive(const QString &archivePath, const QString &destination,
                                           const ExtractOptions &options, const tasks::CancelToken &cancel,
                                           const ProgressCallback &progress = {});

/// Crea `outputPath` con `sources` (archivos o carpetas, que se guardan con
/// su nombre y todo su contenido). Se escribe en un temporal junto al
/// destino y se le da el nombre definitivo al final: un comprimido a medias
/// nunca queda con el nombre bueno. No sobrescribe: si `outputPath` existe,
/// error. Los enlaces se guardan como enlaces, sin seguirlos.
core::Status createArchive(const QString &outputPath, Format format, const QStringList &sources,
                           const tasks::CancelToken &cancel, const ProgressCallback &progress = {});

/// La ruta interna, limpia y segura para extraer: sin "." ni "..", sin "/"
/// inicial ni letra de unidad, con "/" como separador. Vacía si no es segura.
QString safeRelativePath(const QString &insidePath);

} // namespace eu::archive
