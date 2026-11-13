// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/Index.h"

#include "eu/fs/DirectoryLister.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <atomic>
#include <chrono>
#include <deque>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace eu::index {

namespace {

constexpr int schemaVersion = 1;

/// Bits de la columna `flags`.
enum Flag : int {
    DirectoryFlag = 1,
    SymlinkFlag = 2,
    HiddenFlag = 4,
    BrokenSymlinkFlag = 8,
};

qint64 steadyNowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::uint64_t deviceOf(const QString &path)
{
#ifdef Q_OS_UNIX
    struct stat info;
    if (::stat(QFile::encodeName(path).constData(), &info) == 0) {
        return std::uint64_t(info.st_dev);
    }
#else
    Q_UNUSED(path);
#endif
    return 0;
}

core::Error databaseError(const QString &what, const QSqlError &error, const QString &path)
{
    return core::Error{core::ErrorCode::IoError, what + QStringLiteral(": ") + error.text(), path, 0};
}

/// Una conexión propia de quien la crea, que se cierra al salir del bloque.
/// Las consultas (QSqlQuery) deben declararse después, para destruirse antes.
class Connection
{
public:
    explicit Connection(const QString &path)
    {
        static std::atomic<quint64> counter{0};
        m_name = QStringLiteral("eu-index-%1").arg(++counter);
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        database.setDatabaseName(path);
        m_open = database.open();
        if (m_open) {
            QSqlQuery setup(database);
            setup.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
            // El índice se puede reconstruir: no hace falta forzar el disco en
            // cada confirmación como en el historial de operaciones.
            setup.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
            setup.exec(QStringLiteral("PRAGMA busy_timeout=10000"));
            setup.exec(QStringLiteral("PRAGMA temp_store=MEMORY"));
        } else {
            m_error = database.lastError();
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
    QSqlError error() const { return m_error; }
    QSqlDatabase database() const { return QSqlDatabase::database(m_name, false); }

private:
    QString m_name;
    bool m_open = false;
    QSqlError m_error;
};

QString metaValue(const QSqlDatabase &database, const QString &key)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
    query.addBindValue(key);
    return (query.exec() && query.next()) ? query.value(0).toString() : QString();
}

bool setMetaValue(const QSqlDatabase &database, const QString &key, const QString &value)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)"));
    query.addBindValue(key);
    query.addBindValue(value);
    return query.exec();
}

int flagsOf(const fs::Entry &entry)
{
    return (entry.isDirectory ? DirectoryFlag : 0) | (entry.isSymlink ? SymlinkFlag : 0)
        | (entry.isHidden ? HiddenFlag : 0) | (entry.isBrokenSymlink ? BrokenSymlinkFlag : 0);
}

bool isRealFolder(int flags)
{
    return (flags & DirectoryFlag) != 0 && (flags & SymlinkFlag) == 0;
}

QString joinRelative(const QString &directory, const QString &name)
{
    return directory.isEmpty() ? name : directory + QLatin1Char('/') + name;
}

/// Texto para LIKE que encaja con cualquier nombre que contenga `text`.
QString likeContaining(const QString &text)
{
    QString escaped;
    for (const QChar character : text) {
        if (character == QLatin1Char('%') || character == QLatin1Char('_') || character == QLatin1Char('\\')) {
            escaped.append(QLatin1Char('\\'));
        }
        escaped.append(character);
    }
    return QLatin1Char('%') + escaped + QLatin1Char('%');
}

/// Una ruta relativa lista para la base de datos. La raíz es la cadena vacía,
/// y una QString nula se guardaría como NULL: hay que distinguirlas.
QVariant pathValue(const QString &relative)
{
    return relative.isEmpty() ? QVariant(QStringLiteral("")) : QVariant(relative);
}

struct StoredEntry
{
    qint64 size = 0;
    qint64 modifiedMs = 0;
    int flags = 0;
};

struct PendingFolder
{
    QString relative;
    std::uint64_t device = 0;
    bool hidden = false;
};

} // namespace

core::Result<Index> Index::open(const QString &databasePath, const QString &root)
{
    const QString cleanRoot = QDir::cleanPath(root);
    QDir().mkpath(QFileInfo(databasePath).absolutePath());
    {
        Connection connection(databasePath);
        if (!connection.isOpen()) {
            return databaseError(QStringLiteral("No se pudo abrir el índice"), connection.error(), databasePath);
        }
        const QSqlDatabase database = connection.database();
        QSqlQuery query(database);
        int version = 0;
        if (query.exec(QStringLiteral("PRAGMA user_version")) && query.next()) {
            version = query.value(0).toInt();
        }
        query.finish();
        const bool usable = version == schemaVersion && metaValue(database, QStringLiteral("root")) == cleanRoot;
        if (!usable) {
            // Era de otra carpeta, de otra versión o está recién creado.
            const QStringList statements{
                QStringLiteral("DROP TABLE IF EXISTS entries"),
                QStringLiteral("DROP TABLE IF EXISTS dirs"),
                QStringLiteral("DROP TABLE IF EXISTS meta"),
                QStringLiteral("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT) WITHOUT ROWID"),
                // `path` es relativa a la raíz ("" es la raíz). `hidden` vale 1
                // si la carpeta o alguna de las que la contienen está oculta.
                QStringLiteral("CREATE TABLE dirs(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, "
                               "hidden INTEGER NOT NULL)"),
                QStringLiteral("CREATE TABLE entries(dir_id INTEGER NOT NULL, name TEXT NOT NULL, "
                               "size INTEGER NOT NULL, mtime INTEGER NOT NULL, ctime INTEGER NOT NULL, "
                               "flags INTEGER NOT NULL, PRIMARY KEY(dir_id, name)) WITHOUT ROWID"),
                QStringLiteral("PRAGMA user_version=%1").arg(schemaVersion),
            };
            for (const QString &statement : statements) {
                if (!query.exec(statement)) {
                    return databaseError(QStringLiteral("No se pudo preparar el índice"), query.lastError(),
                                         databasePath);
                }
            }
            setMetaValue(database, QStringLiteral("root"), cleanRoot);
        }
    }
    return Index(databasePath, cleanRoot);
}

void Index::remove(const QString &databasePath)
{
    QFile::remove(databasePath);
    QFile::remove(databasePath + QStringLiteral("-wal"));
    QFile::remove(databasePath + QStringLiteral("-shm"));
}

QString Index::relativePath(const QString &path) const
{
    const QString clean = QDir::cleanPath(path);
    if (clean == m_root) {
        return {};
    }
    const QString prefix = m_root.endsWith(QLatin1Char('/')) ? m_root : m_root + QLatin1Char('/');
    return clean.mid(prefix.size());
}

QString Index::absolutePath(const QString &relative) const
{
    if (relative.isEmpty()) {
        return m_root;
    }
    return (m_root.endsWith(QLatin1Char('/')) ? m_root : m_root + QLatin1Char('/')) + relative;
}

bool Index::covers(const QString &path) const
{
    const QString clean = QDir::cleanPath(path);
    const QString prefix = m_root.endsWith(QLatin1Char('/')) ? m_root : m_root + QLatin1Char('/');
    return clean == m_root || clean.startsWith(prefix);
}

IndexInfo Index::info() const
{
    IndexInfo info;
    info.root = m_root;
    Connection connection(m_databasePath);
    if (!connection.isOpen()) {
        return info;
    }
    const QSqlDatabase database = connection.database();
    info.entries = metaValue(database, QStringLiteral("entries")).toLongLong();
    info.folders = metaValue(database, QStringLiteral("folders")).toLongLong();
    info.updatedMs = metaValue(database, QStringLiteral("updated")).toLongLong();
    info.complete = metaValue(database, QStringLiteral("complete")) == QLatin1String("1");
    return info;
}

core::Result<SyncProgress> Index::sync(const QString &folder, const SyncOptions &options,
                                       const tasks::CancelToken &cancel) const
{
    const QString startAbsolute = QDir::cleanPath(folder);
    if (!covers(startAbsolute)) {
        return core::Error{core::ErrorCode::NotSupported, QStringLiteral("La carpeta no pertenece a este índice"),
                           startAbsolute, 0};
    }
    Connection connection(m_databasePath);
    if (!connection.isOpen()) {
        return databaseError(QStringLiteral("No se pudo abrir el índice"), connection.error(), m_databasePath);
    }
    QSqlDatabase database = connection.database();
    SyncProgress progress;

    QSqlQuery findFolder(database);
    findFolder.prepare(QStringLiteral("SELECT id, hidden FROM dirs WHERE path = ?"));
    QSqlQuery insertFolder(database);
    insertFolder.prepare(QStringLiteral("INSERT INTO dirs(path, hidden) VALUES(?, ?)"));
    QSqlQuery loadEntries(database);
    loadEntries.setForwardOnly(true);
    loadEntries.prepare(QStringLiteral("SELECT name, size, mtime, flags FROM entries WHERE dir_id = ?"));
    QSqlQuery writeEntry(database);
    writeEntry.prepare(QStringLiteral("INSERT OR REPLACE INTO entries(dir_id, name, size, mtime, ctime, flags) "
                                      "VALUES(?, ?, ?, ?, ?, ?)"));
    QSqlQuery deleteEntry(database);
    deleteEntry.prepare(QStringLiteral("DELETE FROM entries WHERE dir_id = ? AND name = ?"));
    // Todo lo que cuelga de una carpeta: su ruta, o las que empiezan por
    // "ruta/" (el carácter siguiente a "/" es "0").
    QSqlQuery purgeEntries(database);
    purgeEntries.prepare(QStringLiteral("DELETE FROM entries WHERE dir_id IN "
                                        "(SELECT id FROM dirs WHERE path = ? OR (path >= ? AND path < ?))"));
    QSqlQuery purgeFolders(database);
    purgeFolders.prepare(QStringLiteral("DELETE FROM dirs WHERE path = ? OR (path >= ? AND path < ?)"));

    const auto purgeSubtree = [&](const QString &relative) {
        for (QSqlQuery *query : {&purgeEntries, &purgeFolders}) {
            query->bindValue(0, pathValue(relative));
            query->bindValue(1, relative + QLatin1Char('/'));
            query->bindValue(2, relative + QLatin1Char('0'));
            query->exec();
        }
    };
    // Devuelve el id de la carpeta, o -1 si no está anotada.
    const auto folderId = [&](const QString &relative, bool *hidden) -> qint64 {
        findFolder.bindValue(0, pathValue(relative));
        qint64 id = -1;
        if (findFolder.exec() && findFolder.next()) {
            id = findFolder.value(0).toLongLong();
            if (hidden != nullptr) {
                *hidden = findFolder.value(1).toBool();
            }
        }
        findFolder.finish();
        return id;
    };

    // La fecha de una carpeta está anotada en la fila que tiene en su carpeta
    // padre. Al comparar solo esa carpeta, su propia fila se pone al día aquí.
    QSqlQuery touchEntry(database);
    touchEntry.prepare(QStringLiteral("UPDATE entries SET mtime = ? WHERE dir_id = ? AND name = ?"));
    const auto refreshOwnRow = [&](const QString &relative) {
        if (relative.isEmpty()) {
            return;
        }
        const QFileInfo info(absolutePath(relative));
        const qsizetype slash = relative.lastIndexOf(QLatin1Char('/'));
        const qint64 parentId = folderId(slash < 0 ? QString() : relative.left(slash), nullptr);
        if (!info.exists() || parentId < 0) {
            return;
        }
        touchEntry.bindValue(0, info.lastModified().toMSecsSinceEpoch());
        touchEntry.bindValue(1, parentId);
        touchEntry.bindValue(2, relative.mid(slash + 1));
        touchEntry.exec();
    };

    if (!database.transaction()) {
        return databaseError(QStringLiteral("No se pudo escribir en el índice"), database.lastError(), m_databasePath);
    }

    // La raíz siempre está anotada.
    if (folderId(QString(), nullptr) < 0) {
        insertFolder.bindValue(0, pathValue(QString()));
        insertFolder.bindValue(1, 0);
        if (!insertFolder.exec()) {
            const QSqlError error = insertFolder.lastError();
            database.rollback();
            return databaseError(QStringLiteral("No se pudo escribir en el índice"), error, m_databasePath);
        }
    }

    // Punto de partida. Si la carpeta aún no está anotada (acaba de crearse),
    // se parte de la más cercana por encima que sí lo esté: al compararla
    // aparecerá la nueva y se bajará a ella.
    QString startRelative = relativePath(startAbsolute);
    bool wholeTree = options.wholeTree;
    bool startHidden = false;
    while (!startRelative.isEmpty() && folderId(startRelative, &startHidden) < 0) {
        const qsizetype slash = startRelative.lastIndexOf(QLatin1Char('/'));
        startRelative = slash < 0 ? QString() : startRelative.left(slash);
    }
    const bool fullRun = wholeTree && startRelative.isEmpty();
    if (!wholeTree) {
        refreshOwnRow(startRelative);
    }

    std::deque<PendingFolder> pending;
    pending.push_back({startRelative, deviceOf(absolutePath(startRelative)), startHidden});

    fs::ListOptions listOptions;
    listOptions.firstBatchSize = 5000;
    listOptions.batchSize = 5000;

    qint64 lastCommitMs = steadyNowMs();
    qint64 foldersSinceCommit = 0;
    bool failed = false;

    while (!pending.empty() && !failed) {
        if (options.pause != nullptr && options.pause->isPaused()) {
            // En pausa no se deja una transacción abierta.
            database.commit();
            options.pause->waitWhilePaused(cancel);
            database.transaction();
        }
        if (cancel.isCancelled()) {
            break;
        }
        const PendingFolder folder = pending.front();
        pending.pop_front();
        const QString absolute = absolutePath(folder.relative);
        progress.currentFolder = absolute;

        std::vector<fs::Entry> onDisk;
        const core::Status listed = fs::listDirectory(absolute, listOptions, cancel,
                                                      [&](std::vector<fs::Entry> &&batch) {
            for (fs::Entry &entry : batch) {
                onDisk.push_back(std::move(entry));
            }
        });
        if (cancel.isCancelled()) {
            break;
        }
        ++progress.foldersScanned;
        if (!listed.ok()) {
            if (listed.error().code == core::ErrorCode::NotFound && !folder.relative.isEmpty()) {
                // La carpeta ya no existe: fuera ella y lo que colgaba.
                purgeSubtree(folder.relative);
                const qsizetype slash = folder.relative.lastIndexOf(QLatin1Char('/'));
                const qint64 parentId = folderId(slash < 0 ? QString() : folder.relative.left(slash), nullptr);
                if (parentId >= 0) {
                    deleteEntry.bindValue(0, parentId);
                    deleteEntry.bindValue(1, folder.relative.mid(slash + 1));
                    deleteEntry.exec();
                    ++progress.removed;
                }
                refreshOwnRow(slash < 0 ? QString() : folder.relative.left(slash));
            } else {
                // Sin permiso o error pasajero: lo anotado se deja como está.
                ++progress.unreadableFolders;
            }
            continue;
        }

        qint64 id = folderId(folder.relative, nullptr);
        if (id < 0) {
            insertFolder.bindValue(0, pathValue(folder.relative));
            insertFolder.bindValue(1, folder.hidden ? 1 : 0);
            if (!insertFolder.exec()) {
                failed = true;
                break;
            }
            id = insertFolder.lastInsertId().toLongLong();
        }

        QHash<QString, StoredEntry> stored;
        loadEntries.bindValue(0, id);
        if (loadEntries.exec()) {
            while (loadEntries.next()) {
                stored.insert(loadEntries.value(0).toString(),
                              {loadEntries.value(1).toLongLong(), loadEntries.value(2).toLongLong(),
                               loadEntries.value(3).toInt()});
            }
        }
        loadEntries.finish();

        for (const fs::Entry &entry : onDisk) {
            if (options.excludedNames.contains(entry.name)) {
                continue;   // si estaba anotado, se quitará con los sobrantes
            }
            ++progress.entriesSeen;
            const int flags = flagsOf(entry);
            const auto found = stored.constFind(entry.name);
            const bool existed = found != stored.constEnd();
            const bool wasRealFolder = existed && isRealFolder(found->flags);
            if (!existed || found->size != entry.size || found->modifiedMs != entry.modifiedMs
                || found->flags != flags) {
                writeEntry.bindValue(0, id);
                writeEntry.bindValue(1, entry.name);
                writeEntry.bindValue(2, entry.size);
                writeEntry.bindValue(3, entry.modifiedMs);
                writeEntry.bindValue(4, entry.createdMs);
                writeEntry.bindValue(5, flags);
                if (!writeEntry.exec()) {
                    failed = true;
                    break;
                }
                ++(existed ? progress.updated : progress.added);
            }
            // Igual que la búsqueda directa: se baja a las carpetas reales de
            // la misma unidad, nunca a través de un enlace.
            const bool otherDevice = folder.device != 0 && entry.deviceId != 0 && entry.deviceId != folder.device;
            const bool descend = isRealFolder(flags) && !otherDevice;
            const QString childRelative = joinRelative(folder.relative, entry.name);
            if (descend && (wholeTree || !wasRealFolder)) {
                pending.push_back({childRelative, entry.deviceId != 0 ? entry.deviceId : folder.device,
                                   folder.hidden || entry.isHidden});
            } else if (wasRealFolder && !descend) {
                purgeSubtree(childRelative);
            }
            if (existed) {
                stored.erase(found);
            }
        }
        // Lo que estaba anotado y ya no está en el disco.
        for (auto gone = stored.constBegin(); gone != stored.constEnd() && !failed; ++gone) {
            deleteEntry.bindValue(0, id);
            deleteEntry.bindValue(1, gone.key());
            failed = !deleteEntry.exec();
            ++progress.removed;
            if (isRealFolder(gone->flags)) {
                purgeSubtree(joinRelative(folder.relative, gone.key()));
            }
        }
        if (options.onFolder) {
            options.onFolder(absolute);
        }

        ++foldersSinceCommit;
        const qint64 now = steadyNowMs();
        if (foldersSinceCommit >= 2000 || now - lastCommitMs >= 1000) {
            // Confirmar a trozos: lo hecho no se pierde si se corta, y quien
            // busca mientras tanto va viendo lo nuevo.
            database.commit();
            database.transaction();
            foldersSinceCommit = 0;
            lastCommitMs = now;
            if (options.onProgress) {
                options.onProgress(progress);
            }
        }
    }

    const bool cancelled = cancel.isCancelled();
    if (!failed && !cancelled) {
        QSqlQuery count(database);
        if (count.exec(QStringLiteral("SELECT COUNT(*) FROM entries")) && count.next()) {
            setMetaValue(database, QStringLiteral("entries"), count.value(0).toString());
        }
        if (count.exec(QStringLiteral("SELECT COUNT(*) FROM dirs")) && count.next()) {
            setMetaValue(database, QStringLiteral("folders"), count.value(0).toString());
        }
        if (fullRun) {
            setMetaValue(database, QStringLiteral("complete"), QStringLiteral("1"));
            setMetaValue(database, QStringLiteral("updated"),
                         QString::number(QDateTime::currentMSecsSinceEpoch()));
        }
    }
    if (!database.commit()) {
        failed = true;
    }
    progress.currentFolder.clear();
    if (options.onProgress) {
        options.onProgress(progress);
    }
    if (failed) {
        return databaseError(QStringLiteral("No se pudo escribir en el índice"), database.lastError(), m_databasePath);
    }
    if (cancelled) {
        return core::Error{core::ErrorCode::Cancelled, QStringLiteral("Indexación cancelada"), startAbsolute, 0};
    }
    return progress;
}

bool Index::canAnswer(const QStringList &roots, const search::Query &query,
                      const search::SearchOptions &options) const
{
    // Lo que el índice no sabe: el contenido de los archivos. Y lo que no
    // recorre igual que la búsqueda directa: otras unidades.
    if (roots.isEmpty() || query.usesContent() || !options.recursive || options.crossDevices) {
        return false;
    }
    for (const QString &root : roots) {
        if (!covers(root)) {
            return false;
        }
    }
    Connection connection(m_databasePath);
    if (!connection.isOpen()) {
        return false;
    }
    const QSqlDatabase database = connection.database();
    if (metaValue(database, QStringLiteral("complete")) != QLatin1String("1")) {
        return false;
    }
    QSqlQuery findFolder(database);
    findFolder.prepare(QStringLiteral("SELECT hidden FROM dirs WHERE path = ?"));
    for (const QString &root : roots) {
        findFolder.bindValue(0, pathValue(relativePath(root)));
        if (!findFolder.exec() || !findFolder.next()) {
            return false;   // carpeta que el índice no conoce (nueva, o de otra unidad)
        }
        // Buscar dentro de una carpeta oculta sin "mostrar ocultos" sí
        // enseña su contenido; el índice lo tiene marcado como oculto.
        if (!options.includeHidden && findFolder.value(0).toBool()) {
            return false;
        }
        findFolder.finish();
    }
    return true;
}

search::SearchSummary Index::search(const QStringList &roots, const search::Query &query,
                                    const search::SearchOptions &options, const tasks::CancelToken &cancel,
                                    const search::HitsCallback &onHits) const
{
    search::SearchSummary summary;
    search::SearchProgress &progress = summary.progress;
    std::vector<search::Hit> batch;
    qint64 lastDeliveryMs = steadyNowMs();
    const auto deliver = [&](bool force) {
        const qint64 now = steadyNowMs();
        if (!force && qint64(batch.size()) < options.batchSize && now - lastDeliveryMs < options.maxBatchDelayMs) {
            return;
        }
        lastDeliveryMs = now;
        std::vector<search::Hit> ready;
        ready.swap(batch);
        onHits(std::move(ready), progress);
    };

    {
        Connection connection(m_databasePath);
        const QString hint = query.nameHint();
        for (const QString &root : roots) {
            if (!connection.isOpen() || summary.truncated || cancel.isCancelled()) {
                break;
            }
            const QString relative = relativePath(root);
            QString sql = QStringLiteral("SELECT d.path, e.name, e.size, e.mtime, e.ctime, e.flags "
                                         "FROM entries e JOIN dirs d ON d.id = e.dir_id WHERE 1");
            if (!relative.isEmpty()) {
                sql += QStringLiteral(" AND (d.path = ? OR (d.path >= ? AND d.path < ?))");
            }
            if (!options.includeHidden) {
                sql += QStringLiteral(" AND d.hidden = 0 AND (e.flags & %1) = 0").arg(int(HiddenFlag));
            }
            if (!hint.isEmpty()) {
                sql += QStringLiteral(" AND e.name LIKE ? ESCAPE '\\'");
            }
            QSqlQuery rows(connection.database());
            rows.setForwardOnly(true);
            rows.prepare(sql);
            if (!relative.isEmpty()) {
                rows.addBindValue(relative);
                rows.addBindValue(relative + QLatin1Char('/'));
                rows.addBindValue(relative + QLatin1Char('0'));
            }
            if (!hint.isEmpty()) {
                rows.addBindValue(likeContaining(hint));
            }
            if (!rows.exec()) {
                break;
            }
            qint64 sinceCheck = 0;
            while (rows.next()) {
                if (++sinceCheck >= 4096) {
                    sinceCheck = 0;
                    if (cancel.isCancelled()) {
                        break;
                    }
                    deliver(false);
                }
                ++progress.itemsExamined;
                fs::Entry entry;
                entry.name = rows.value(1).toString();
                entry.size = rows.value(2).toLongLong();
                entry.modifiedMs = rows.value(3).toLongLong();
                entry.createdMs = rows.value(4).toLongLong();
                const int flags = rows.value(5).toInt();
                entry.isDirectory = (flags & DirectoryFlag) != 0;
                entry.isSymlink = (flags & SymlinkFlag) != 0;
                entry.isHidden = (flags & HiddenFlag) != 0;
                entry.isBrokenSymlink = (flags & BrokenSymlinkFlag) != 0;
                if (!query.matches(entry)) {
                    continue;
                }
                ++progress.hits;
                batch.push_back({absolutePath(rows.value(0).toString()), std::move(entry)});
                if (progress.hits >= options.maxHits) {
                    summary.truncated = true;
                    break;
                }
                deliver(false);
            }
        }
    }
    summary.cancelled = cancel.isCancelled();
    deliver(true);
    return summary;
}

} // namespace eu::index
