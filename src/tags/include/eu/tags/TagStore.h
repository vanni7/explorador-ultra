// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/journal/Journal.h"
#include "eu/tasks/Cancel.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

namespace eu::tags {

struct Tag
{
    qint64 id = 0;
    QString name;
    QString color;   ///< "#rrggbb".
};

/// Una etiqueta que perdió su archivo y no se pudo volver a encontrar sin duda.
struct Orphan
{
    qint64 markId = 0;
    Tag tag;
    QString path;      ///< Donde estaba.
    qint64 size = 0;
    qint64 modifiedMs = 0;
};

/// Etiquetas y colores (SPEC §17): en una base local, sin tocar los archivos.
///
/// Cada marca guarda la ruta y la identidad del archivo (inodo o índice de
/// archivo + volumen, tamaño y fecha). Lo que la aplicación mueve o renombra
/// se sigue con `followSteps` (del historial). Lo que se mueve por fuera se
/// reconoce al verlo por su identidad; si no se puede saber sin dudas, la
/// marca queda huérfana para revisarla: nunca se reasigna por adivinanza.
///
/// Cada llamada abre su conexión: se puede usar desde cualquier hilo.
class TagStore
{
public:
    static core::Result<std::shared_ptr<TagStore>> open(const QString &databasePath);

    QList<Tag> tags() const;
    core::Result<Tag> createTag(const QString &name, const QString &color);
    core::Status renameTag(qint64 id, const QString &name);
    core::Status setColor(qint64 id, const QString &color);
    /// Borra la etiqueta y todas sus marcas.
    core::Status deleteTag(qint64 id);

    core::Status assign(const QStringList &paths, qint64 tagId);
    core::Status unassign(const QStringList &paths, qint64 tagId);

    /// Las etiquetas de cada ruta (una sola consulta). Las que no están por su
    /// ruta se buscan por su identidad: si la marca era de un archivo que ya
    /// no está en su ruta vieja, se le pasa a la nueva (movido por fuera en el
    /// mismo volumen).
    QHash<QString, QList<Tag>> tagsFor(const QStringList &paths) const;
    /// Rutas con esa etiqueta (por nombre, sin distinguir mayúsculas).
    QStringList pathsWithTag(const QString &name) const;

    /// Sigue lo que hizo una operación del historial: renombrar y mover
    /// (también entre unidades: copiar y luego borrar el original) y la
    /// papelera. Las carpetas arrastran lo de dentro.
    void followSteps(const QList<journal::Step> &steps);

    /// Repasa todas las marcas: las de archivos que ya no están (o que ahora
    /// son otro archivo) pasan a huérfanas. Devuelve las huérfanas.
    QList<Orphan> review() const;
    QList<Orphan> orphans() const;
    /// Busca otra vez un huérfano en `roots`: un único archivo con su misma
    /// identidad o, si no, con el mismo nombre, tamaño y fecha. Ninguno o
    /// varios: sigue huérfano (devuelve false).
    core::Result<bool> relocate(qint64 markId, const QStringList &roots, const tasks::CancelToken &cancel) const;
    core::Status removeMark(qint64 markId);

    const QString &databasePath() const { return m_databasePath; }

private:
    explicit TagStore(QString databasePath) : m_databasePath(std::move(databasePath)) {}
    QString m_databasePath;
};

/// Colores propuestos para las etiquetas nuevas.
QStringList defaultColors();

} // namespace eu::tags
