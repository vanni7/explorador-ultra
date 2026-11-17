// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/tags/TagStore.h"

#include "eu/fs/DirectoryLister.h"
#include "eu/search/Query.h"
#include "eu/search/Searcher.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <atomic>

namespace eu::tags {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::tags", text);
}

class Connection
{
public:
    explicit Connection(const QString &path)
    {
        static std::atomic<quint64> counter{0};
        m_name = QStringLiteral("eu-tags-%1").arg(++counter);
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        database.setDatabaseName(path);
        m_open = database.open();
        if (m_open) {
            QSqlQuery setup(database);
            setup.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
            setup.exec(QStringLiteral("PRAGMA busy_timeout=10000"));
            setup.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
        } else {
            m_error = database.lastError().text();
        }
    }
    ~Connection()
    {
        {
            QSqlDatabase database = QSqlDatabase::database(m_name, false);
            database.close();
        }
        QSqlDatabase::removeDatabase(m_name);
    }
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    bool isOpen() const { return m_open; }
    QString error() const { return m_error; }
    QSqlDatabase database() const { return QSqlDatabase::database(m_name, false); }

private:
    QString m_name;
    bool m_open = false;
    QString m_error;
};

core::Error databaseError(const QSqlQuery &query, const QString &path)
{
    return core::Error{core::ErrorCode::IoError, tr("Error en las etiquetas: %1").arg(query.lastError().text()), path, 0};
}

Tag readTag(const QSqlQuery &query, int first)
{
    return Tag{query.value(first).toLongLong(), query.value(first + 1).toString(), query.value(first + 2).toString()};
}

/// ¿`path` es `prefix` o está dentro?
bool isInside(const QString &path, const QString &prefix)
{
    return path == prefix || path.startsWith(prefix.endsWith(QLatin1Char('/')) ? prefix : prefix + QLatin1Char('/'));
}

/// Cambia la ruta de las marcas de `from` (y lo de dentro, si es carpeta) a `to`.
void moveMarks(QSqlDatabase &database, const QString &from, const QString &to)
{
    if (from.isEmpty() || to.isEmpty() || from == to) {
        return;
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral("UPDATE OR REPLACE marks SET path = ? || substr(path, ?) "
                                 "WHERE path = ? OR substr(path, 1, ?) = ?"));
    query.addBindValue(to);
    query.addBindValue(int(from.size()) + 1);
    query.addBindValue(from);
    query.addBindValue(int(from.size()) + 1);
    query.addBindValue(from + QLatin1Char('/'));
    query.exec();
    // Si ahora el archivo está donde dice la marca, deja de ser huérfana.
    QSqlQuery found(database);
    found.prepare(QStringLiteral("UPDATE marks SET orphan = 0 WHERE path = ?"));
    found.addBindValue(to);
    found.exec();
}

} // namespace

QStringList defaultColors()
{
    return {QStringLiteral("#e74c3c"), QStringLiteral("#e67e22"), QStringLiteral("#f1c40f"), QStringLiteral("#2ecc71"),
            QStringLiteral("#3498db"), QStringLiteral("#9b59b6"), QStringLiteral("#7f8c8d")};
}

core::Result<std::shared_ptr<TagStore>> TagStore::open(const QString &databasePath)
{
    QDir().mkpath(QFileInfo(databasePath).absolutePath());
    Connection connection(databasePath);
    if (!connection.isOpen()) {
        return core::Error{core::ErrorCode::IoError, tr("No se pudo abrir la base de etiquetas: %1").arg(connection.error()),
                           databasePath, 0};
    }
    QSqlQuery query(connection.database());
    const QStringList schema{
        QStringLiteral("CREATE TABLE IF NOT EXISTS tags(id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE COLLATE NOCASE, "
                       "color TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS marks(id INTEGER PRIMARY KEY, "
                       "tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE, path TEXT NOT NULL, "
                       "device INTEGER NOT NULL DEFAULT 0, inode INTEGER NOT NULL DEFAULT 0, size INTEGER NOT NULL DEFAULT 0, "
                       "mtime INTEGER NOT NULL DEFAULT 0, orphan INTEGER NOT NULL DEFAULT 0, UNIQUE(tag_id, path))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS marks_by_path ON marks(path)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS marks_by_identity ON marks(device, inode)"),
    };
    for (const QString &statement : schema) {
        if (!query.exec(statement)) {
            return databaseError(query, databasePath);
        }
    }
    return std::shared_ptr<TagStore>(new TagStore(databasePath));
}

QList<Tag> TagStore::tags() const
{
    QList<Tag> all;
    Connection connection(m_databasePath);
    QSqlQuery query(connection.database());
    if (query.exec(QStringLiteral("SELECT id, name, color FROM tags ORDER BY name COLLATE NOCASE"))) {
        while (query.next()) {
            all.append(readTag(query, 0));
        }
    }
    return all;
}

core::Result<Tag> TagStore::createTag(const QString &name, const QString &color)
{
    const QString clean = name.simplified();
    if (clean.isEmpty()) {
        return core::Error{core::ErrorCode::InvalidName, tr("La etiqueta necesita un nombre."), name, 0};
    }
    Connection connection(m_databasePath);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("INSERT INTO tags(name, color) VALUES(?, ?)"));
    query.addBindValue(clean);
    query.addBindValue(color);
    if (!query.exec()) {
        return core::Error{core::ErrorCode::AlreadyExists, tr("Ya hay una etiqueta «%1».").arg(clean), clean, 0};
    }
    return Tag{query.lastInsertId().toLongLong(), clean, color};
}

core::Status TagStore::renameTag(qint64 id, const QString &name)
{
    const QString clean = name.simplified();
    if (clean.isEmpty()) {
        return core::Error{core::ErrorCode::InvalidName, tr("La etiqueta necesita un nombre."), name, 0};
    }
    Connection connection(m_databasePath);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("UPDATE tags SET name = ? WHERE id = ?"));
    query.addBindValue(clean);
    query.addBindValue(id);
    if (!query.exec()) {
        return core::Error{core::ErrorCode::AlreadyExists, tr("Ya hay una etiqueta «%1».").arg(clean), clean, 0};
    }
    return {};
}

core::Status TagStore::setColor(qint64 id, const QString &color)
{
    Connection connection(m_databasePath);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("UPDATE tags SET color = ? WHERE id = ?"));
    query.addBindValue(color);
    query.addBindValue(id);
    return query.exec() ? core::Status{} : core::Status{databaseError(query, m_databasePath)};
}

core::Status TagStore::deleteTag(qint64 id)
{
    Connection connection(m_databasePath);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("DELETE FROM tags WHERE id = ?"));
    query.addBindValue(id);
    return query.exec() ? core::Status{} : core::Status{databaseError(query, m_databasePath)};
}

core::Status TagStore::assign(const QStringList &paths, qint64 tagId)
{
    Connection connection(m_databasePath);
    QSqlDatabase database = connection.database();
    database.transaction();
    QSqlQuery query(database);
    query.prepare(QStringLiteral("INSERT INTO marks(tag_id, path, device, inode, size, mtime, orphan) "
                                 "VALUES(?, ?, ?, ?, ?, ?, 0) ON CONFLICT(tag_id, path) DO UPDATE SET "
                                 "device = excluded.device, inode = excluded.inode, size = excluded.size, "
                                 "mtime = excluded.mtime, orphan = 0"));
    for (const QString &path : paths) {
        const core::Result<fs::Entry> entry = fs::entryFor(path);
        if (!entry.ok()) {
            database.rollback();
            return entry.error();
        }
        query.addBindValue(tagId);
        query.addBindValue(QDir::cleanPath(path));
        query.addBindValue(qint64(entry.value().deviceId));
        query.addBindValue(qint64(entry.value().fileId));
        query.addBindValue(entry.value().size);
        query.addBindValue(entry.value().modifiedMs);
        if (!query.exec()) {
            const core::Error error = databaseError(query, m_databasePath);
            database.rollback();
            return error;
        }
    }
    database.commit();
    return {};
}

core::Status TagStore::unassign(const QStringList &paths, qint64 tagId)
{
    Connection connection(m_databasePath);
    QSqlDatabase database = connection.database();
    database.transaction();
    QSqlQuery query(database);
    query.prepare(QStringLiteral("DELETE FROM marks WHERE tag_id = ? AND path = ?"));
    for (const QString &path : paths) {
        query.addBindValue(tagId);
        query.addBindValue(QDir::cleanPath(path));
        query.exec();
    }
    database.commit();
    return {};
}

QHash<QString, QList<Tag>> TagStore::tagsFor(const QStringList &paths) const
{
    QHash<QString, QList<Tag>> result;
    if (paths.isEmpty()) {
        return result;
    }
    Connection connection(m_databasePath);
    QSqlDatabase database = connection.database();
    QSqlQuery byPath(database);
    byPath.prepare(QStringLiteral("SELECT t.id, t.name, t.color FROM marks m JOIN tags t ON t.id = m.tag_id "
                                  "WHERE m.path = ? AND m.orphan = 0 ORDER BY t.name COLLATE NOCASE"));
    QSqlQuery byIdentity(database);
    byIdentity.prepare(QStringLiteral("SELECT m.id, m.path FROM marks m WHERE m.device = ? AND m.inode = ? AND m.inode <> 0"));
    for (const QString &raw : paths) {
        const QString path = QDir::cleanPath(raw);
        byPath.addBindValue(path);
        if (byPath.exec()) {
            while (byPath.next()) {
                result[path].append(readTag(byPath, 0));
            }
        }
        if (result.contains(path)) {
            continue;
        }
        // ¿Una marca de este mismo archivo con otra ruta que ya no existe? (Movido por fuera.)
        const core::Result<fs::Entry> entry = fs::entryFor(path);
        if (!entry.ok() || entry.value().fileId == 0) {
            continue;
        }
        byIdentity.addBindValue(qint64(entry.value().deviceId));
        byIdentity.addBindValue(qint64(entry.value().fileId));
        QList<QString> oldPaths;
        if (byIdentity.exec()) {
            while (byIdentity.next()) {
                const QString old = byIdentity.value(1).toString();
                if (old != path && !QFileInfo::exists(old) && !oldPaths.contains(old)) {
                    oldPaths.append(old);
                }
            }
        }
        if (oldPaths.size() == 1) {
            moveMarks(database, oldPaths.first(), path);
            byPath.addBindValue(path);
            if (byPath.exec()) {
                while (byPath.next()) {
                    result[path].append(readTag(byPath, 0));
                }
            }
        }
    }
    return result;
}

QStringList TagStore::pathsWithTag(const QString &name) const
{
    QStringList paths;
    Connection connection(m_databasePath);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("SELECT m.path FROM marks m JOIN tags t ON t.id = m.tag_id "
                                 "WHERE t.name = ? COLLATE NOCASE AND m.orphan = 0"));
    query.addBindValue(name.simplified());
    if (query.exec()) {
        while (query.next()) {
            paths.append(query.value(0).toString());
        }
    }
    return paths;
}

void TagStore::followSteps(const QList<journal::Step> &steps)
{
    Connection connection(m_databasePath);
    QSqlDatabase database = connection.database();
    database.transaction();
    QHash<QString, QString> copiedTo;   // origen -> destino, de los CopyFile
    for (const journal::Step &step : steps) {
        if (step.state != journal::StepState::Done) {
            continue;
        }
        switch (step.kind) {
        case journal::StepKind::Rename:
            moveMarks(database, QDir::cleanPath(step.source), QDir::cleanPath(step.target));
            break;
        case journal::StepKind::CopyFile:
        case journal::StepKind::CopyLink:
            copiedTo.insert(QDir::cleanPath(step.source), QDir::cleanPath(step.target));
            break;
        case journal::StepKind::RemoveFile: {
            // Mover entre unidades: se copió y se borró el original.
            const QString source = QDir::cleanPath(step.source);
            if (copiedTo.contains(source)) {
                moveMarks(database, source, copiedTo.value(source));
            }
            break;
        }
        default:
            break;
        }
    }
    database.commit();
    // Las marcas movidas entre unidades tienen otra identidad: se actualiza.
    QSqlQuery stale(database);
    if (stale.exec(QStringLiteral("SELECT DISTINCT path FROM marks WHERE orphan = 0"))) {
        QStringList paths;
        while (stale.next()) {
            const QString path = stale.value(0).toString();
            for (const QString &target : std::as_const(copiedTo)) {
                if (isInside(path, target)) {
                    paths.append(path);
                    break;
                }
            }
        }
        QSqlQuery update(database);
        update.prepare(QStringLiteral("UPDATE marks SET device = ?, inode = ?, size = ?, mtime = ? WHERE path = ?"));
        for (const QString &path : std::as_const(paths)) {
            const core::Result<fs::Entry> entry = fs::entryFor(path);
            if (entry.ok()) {
                update.addBindValue(qint64(entry.value().deviceId));
                update.addBindValue(qint64(entry.value().fileId));
                update.addBindValue(entry.value().size);
                update.addBindValue(entry.value().modifiedMs);
                update.addBindValue(path);
                update.exec();
            }
        }
    }
}

QList<Orphan> TagStore::review() const
{
    Connection connection(m_databasePath);
    QSqlDatabase database = connection.database();
    QSqlQuery all(database);
    QList<QPair<qint64, QString>> lost;
    if (all.exec(QStringLiteral("SELECT id, path, device, inode FROM marks WHERE orphan = 0"))) {
        while (all.next()) {
            const QString path = all.value(1).toString();
            const core::Result<fs::Entry> entry = fs::entryFor(path);
            const qint64 inode = all.value(3).toLongLong();
            // No está, o en su ruta hay otro archivo (con identidad conocida y distinta).
            const bool gone = !entry.ok()
                || (inode != 0 && entry.value().fileId != 0
                    && (qint64(entry.value().fileId) != inode || qint64(entry.value().deviceId) != all.value(2).toLongLong()));
            if (gone) {
                lost.append({all.value(0).toLongLong(), path});
            }
        }
    }
    QSqlQuery mark(database);
    mark.prepare(QStringLiteral("UPDATE marks SET orphan = 1 WHERE id = ?"));
    for (const auto &[id, path] : std::as_const(lost)) {
        mark.addBindValue(id);
        mark.exec();
    }
    return orphans();
}

QList<Orphan> TagStore::orphans() const
{
    QList<Orphan> list;
    Connection connection(m_databasePath);
    QSqlQuery query(connection.database());
    if (query.exec(QStringLiteral("SELECT m.id, t.id, t.name, t.color, m.path, m.size, m.mtime FROM marks m "
                                  "JOIN tags t ON t.id = m.tag_id WHERE m.orphan = 1 ORDER BY m.path"))) {
        while (query.next()) {
            list.append(Orphan{query.value(0).toLongLong(), readTag(query, 1), query.value(4).toString(),
                               query.value(5).toLongLong(), query.value(6).toLongLong()});
        }
    }
    return list;
}

core::Result<bool> TagStore::relocate(qint64 markId, const QStringList &roots, const tasks::CancelToken &cancel) const
{
    Connection connection(m_databasePath);
    QSqlDatabase database = connection.database();
    QSqlQuery read(database);
    read.prepare(QStringLiteral("SELECT path, device, inode, size, mtime FROM marks WHERE id = ?"));
    read.addBindValue(markId);
    if (!read.exec() || !read.next()) {
        return core::Error{core::ErrorCode::NotFound, tr("Esa marca ya no existe."), {}, 0};
    }
    const QString oldPath = read.value(0).toString();
    const qint64 device = read.value(1).toLongLong();
    const qint64 inode = read.value(2).toLongLong();
    const qint64 size = read.value(3).toLongLong();
    const qint64 mtime = read.value(4).toLongLong();
    read.finish();
    const QString name = QFileInfo(oldPath).fileName();

    // Candidatos: el mismo archivo (identidad) o, si no hay, mismo nombre, tamaño y fecha.
    QStringList sameIdentity;
    QStringList sameData;
    search::SearchOptions walk;
    walk.includeHidden = true;
    walk.maxHits = std::numeric_limits<qint64>::max();
    search::searchTree(roots, search::Query::parse(QStringLiteral("*")).value(), walk, cancel, nullptr,
                       [&](std::vector<search::Hit> &&hits, const search::SearchProgress &) {
        for (const search::Hit &hit : hits) {
            if (hit.entry.isDirectory) {
                continue;
            }
            if (inode != 0 && qint64(hit.entry.fileId) == inode && qint64(hit.entry.deviceId) == device) {
                sameIdentity.append(hit.path());
            } else if (hit.entry.name == name && hit.entry.size == size && hit.entry.modifiedMs == mtime) {
                sameData.append(hit.path());
            }
        }
    });
    if (cancel.isCancelled()) {
        return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), {}, 0};
    }
    const QStringList &candidates = !sameIdentity.isEmpty() ? sameIdentity : sameData;
    if (candidates.size() != 1) {
        return false;   // ninguno o varios: sin adivinar
    }
    const QString found = QDir::cleanPath(candidates.first());
    QSqlQuery update(database);
    update.prepare(QStringLiteral("UPDATE OR REPLACE marks SET path = ?, orphan = 0 WHERE id = ?"));
    update.addBindValue(found);
    update.addBindValue(markId);
    if (!update.exec()) {
        return databaseError(update, m_databasePath);
    }
    // La identidad, al día (puede haber cambiado de unidad).
    const core::Result<fs::Entry> entry = fs::entryFor(found);
    if (entry.ok()) {
        QSqlQuery identity(database);
        identity.prepare(QStringLiteral("UPDATE marks SET device = ?, inode = ? WHERE id = ?"));
        identity.addBindValue(qint64(entry.value().deviceId));
        identity.addBindValue(qint64(entry.value().fileId));
        identity.addBindValue(markId);
        identity.exec();
    }
    return true;
}

core::Status TagStore::removeMark(qint64 markId)
{
    Connection connection(m_databasePath);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("DELETE FROM marks WHERE id = ?"));
    query.addBindValue(markId);
    return query.exec() ? core::Status{} : core::Status{databaseError(query, m_databasePath)};
}

} // namespace eu::tags
