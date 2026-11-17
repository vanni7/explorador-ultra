// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/ContentIndex.h"

#include "eu/content/TextExtractor.h"
#include "eu/search/Query.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <atomic>
#include <vector>

namespace eu::index {

namespace {

/// Si cambia la forma de la base, se sube y la base vieja se vacía.
constexpr int kSchemaVersion = 2;   // 2: páginas separadas por \f
/// Cada cuántos documentos (o segundos) se guarda lo hecho.
constexpr int kCommitEvery = 20;
constexpr qint64 kCommitEveryMs = 3000;

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::index::ContentIndex", text);
}

/// Una conexión propia para cada llamada (ADR-0011).
class Connection
{
public:
    explicit Connection(const QString &path)
    {
        static std::atomic<quint64> counter{0};
        m_name = QStringLiteral("eu-content-%1").arg(++counter);
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        database.setDatabaseName(path);
        m_open = database.open();
        if (m_open) {
            QSqlQuery setup(database);
            setup.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
            setup.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
            setup.exec(QStringLiteral("PRAGMA busy_timeout=10000"));
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

QString metaValue(const QSqlDatabase &database, const QString &key)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
    query.addBindValue(key);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}

void setMeta(const QSqlDatabase &database, const QString &key, const QString &value)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("INSERT INTO meta(key, value) VALUES(?, ?) "
                                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    query.addBindValue(key);
    query.addBindValue(value);
    query.exec();
}

/// Lo anotado de un documento.
struct Row
{
    qint64 id = 0;
    qint64 size = 0;
    qint64 modifiedMs = 0;
    bool hasText = false;
};

QHash<QString, Row> loadRows(const QSqlDatabase &database)
{
    QHash<QString, Row> rows;
    QSqlQuery query(database);
    query.setForwardOnly(true);
    if (query.exec(QStringLiteral("SELECT path, id, size, mtime, status FROM docs"))) {
        while (query.next()) {
            rows.insert(query.value(0).toString(), Row{query.value(1).toLongLong(), query.value(2).toLongLong(),
                                                       query.value(3).toLongLong(), query.value(4).toInt() == 0});
        }
    }
    return rows;
}

/// Un texto como frase de FTS5 ("…", con las comillas de dentro dobladas).
QString ftsPhrase(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

/// La consulta de una búsqueda sobre un índice.
class SingleCache
{
public:
    explicit SingleCache(ContentIndex index) : m_index(std::move(index)) {}

    const ContentIndex &index() const { return m_index; }

    void prepare(const QStringList &required)
    {
        m_connection = std::make_unique<Connection>(m_index.databasePath());
        if (!m_connection->isOpen()) {
            m_connection.reset();
            return;
        }
        const QSqlDatabase database = m_connection->database();
        m_rows = loadRows(database);
        // Con trigramas, FTS5 encuentra cualquier trozo de 3 caracteres o más,
        // sin distinguir mayúsculas: da un conjunto que incluye seguro a los
        // que contienen el texto. Lo exacto se comprueba después sobre el texto.
        for (const QString &term : required) {
            if (term.toUcs4().size() < 3) {
                continue;
            }
            QSqlQuery query(database);
            query.setForwardOnly(true);
            query.prepare(QStringLiteral("SELECT rowid FROM body WHERE body MATCH ?"));
            query.addBindValue(ftsPhrase(term));
            if (!query.exec()) {
                continue;   // sin filtro previo: se mira el texto de cada uno
            }
            QSet<qint64> ids;
            while (query.next()) {
                ids.insert(query.value(0).toLongLong());
            }
            if (m_filtered) {
                m_candidates.intersect(ids);
            } else {
                m_candidates = std::move(ids);
                m_filtered = true;
            }
        }
        m_readText = std::make_unique<QSqlQuery>(database);
        m_readText->prepare(QStringLiteral("SELECT text FROM body WHERE rowid = ?"));
    }

    search::ContentCache::Lookup lookup(const QString &relative, qint64 size, qint64 modifiedMs, QString *text)
    {
        using Lookup = search::ContentCache::Lookup;
        if (!m_connection) {
            return Lookup::Unknown;
        }
        const auto found = m_rows.constFind(relative);
        if (found == m_rows.constEnd() || found->size != size || found->modifiedMs != modifiedMs) {
            return Lookup::Unknown;   // nuevo o cambiado desde que se anotó
        }
        if (!found->hasText || (m_filtered && !m_candidates.contains(found->id))) {
            return Lookup::Absent;
        }
        m_readText->bindValue(0, found->id);
        if (!m_readText->exec() || !m_readText->next()) {
            return Lookup::Unknown;
        }
        *text = m_readText->value(0).toString();
        m_readText->finish();
        return Lookup::Text;
    }

    ~SingleCache()
    {
        m_readText.reset();   // antes que la conexión
    }

private:
    ContentIndex m_index;
    std::unique_ptr<Connection> m_connection;
    std::unique_ptr<QSqlQuery> m_readText;
    QHash<QString, Row> m_rows;
    QSet<qint64> m_candidates;
    bool m_filtered = false;
};

class Cache : public search::ContentCache
{
public:
    explicit Cache(const QList<ContentIndex> &indexes)
    {
        for (const ContentIndex &index : indexes) {
            m_caches.push_back(std::make_unique<SingleCache>(index));
        }
    }

    void prepare(const QStringList &required, Qt::CaseSensitivity) override
    {
        for (const auto &cache : m_caches) {
            cache->prepare(required);
        }
    }

    Lookup lookup(const QString &path, qint64 size, qint64 modifiedMs, QString *text) override
    {
        // El índice más profundo que lo cubre.
        SingleCache *best = nullptr;
        for (const auto &cache : m_caches) {
            if (cache->index().covers(path)
                && (best == nullptr || cache->index().root().size() > best->index().root().size())) {
                best = cache.get();
            }
        }
        if (best == nullptr) {
            return Lookup::Unknown;
        }
        const QString clean = QDir::cleanPath(path);
        const QString &root = best->index().root();
        const QString relative = clean.mid(root.endsWith(QLatin1Char('/')) ? root.size() : root.size() + 1);
        return best->lookup(relative, size, modifiedMs, text);
    }

private:
    std::vector<std::unique_ptr<SingleCache>> m_caches;
};

} // namespace

core::Result<ContentIndex> ContentIndex::open(const QString &databasePath, const QString &root)
{
    const QString cleanRoot = QDir::cleanPath(root);
    QDir().mkpath(QFileInfo(databasePath).absolutePath());
    Connection connection(databasePath);
    if (!connection.isOpen()) {
        return core::Error{core::ErrorCode::IoError, tr("No se pudo abrir el índice de contenido: %1")
                                                         .arg(connection.error()), databasePath, 0};
    }
    QSqlDatabase database = connection.database();
    QSqlQuery query(database);
    query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT)"));
    const bool sameRoot = metaValue(database, QStringLiteral("root")) == cleanRoot;
    const bool sameVersion = metaValue(database, QStringLiteral("version")) == QString::number(kSchemaVersion);
    if (!sameRoot || !sameVersion) {
        // De otra carpeta o de otra versión: se empieza de cero.
        query.exec(QStringLiteral("DROP TABLE IF EXISTS docs"));
        query.exec(QStringLiteral("DROP TABLE IF EXISTS body"));
        query.exec(QStringLiteral("DELETE FROM meta"));
    }
    query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS docs(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, "
                              "size INTEGER NOT NULL, mtime INTEGER NOT NULL, status INTEGER NOT NULL)"));
    // Trigramas: encuentra cualquier trozo, como «contiene» en la búsqueda directa.
    if (!query.exec(QStringLiteral("CREATE VIRTUAL TABLE IF NOT EXISTS body USING fts5(text, tokenize = 'trigram')"))) {
        return core::Error{core::ErrorCode::NotSupported,
                           tr("El SQLite de esta instalación no tiene búsqueda de texto completo (FTS5 con "
                              "trigramas): %1").arg(query.lastError().text()),
                           databasePath, 0};
    }
    setMeta(database, QStringLiteral("root"), cleanRoot);
    setMeta(database, QStringLiteral("version"), QString::number(kSchemaVersion));
    return ContentIndex(databasePath, cleanRoot);
}

void ContentIndex::remove(const QString &databasePath)
{
    QFile::remove(databasePath);
    QFile::remove(databasePath + QStringLiteral("-wal"));
    QFile::remove(databasePath + QStringLiteral("-shm"));
}

bool ContentIndex::covers(const QString &path) const
{
    const QString clean = QDir::cleanPath(path);
    const QString prefix = m_root.endsWith(QLatin1Char('/')) ? m_root : m_root + QLatin1Char('/');
    return clean == m_root || clean.startsWith(prefix);
}

QString ContentIndex::relativePath(const QString &path) const
{
    const QString clean = QDir::cleanPath(path);
    if (clean == m_root) {
        return {};
    }
    return clean.mid(m_root.endsWith(QLatin1Char('/')) ? m_root.size() : m_root.size() + 1);
}

ContentIndexInfo ContentIndex::info() const
{
    ContentIndexInfo info;
    info.root = m_root;
    Connection connection(m_databasePath);
    if (!connection.isOpen()) {
        return info;
    }
    const QSqlDatabase database = connection.database();
    QSqlQuery query(database);
    if (query.exec(QStringLiteral("SELECT count(*), coalesce(sum(status = 0), 0) FROM docs")) && query.next()) {
        info.documents = query.value(0).toLongLong();
        info.withText = query.value(1).toLongLong();
    }
    info.textChars = metaValue(database, QStringLiteral("chars")).toLongLong();
    info.updatedMs = metaValue(database, QStringLiteral("updated")).toLongLong();
    info.complete = metaValue(database, QStringLiteral("complete")) == QLatin1String("1");
    return info;
}

core::Result<ContentSyncProgress> ContentIndex::sync(const ContentSyncOptions &options,
                                                     const tasks::CancelToken &cancel) const
{
    const auto cancelled = [&] {
        return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), m_root, 0};
    };
    if (!QFileInfo(m_root).isDir()) {
        return core::Error{core::ErrorCode::Disconnected, tr("La carpeta no está disponible"), m_root, 0};
    }
    Connection connection(m_databasePath);
    if (!connection.isOpen()) {
        return core::Error{core::ErrorCode::IoError, connection.error(), m_databasePath, 0};
    }
    QSqlDatabase database = connection.database();
    QHash<QString, Row> rows = loadRows(database);

    ContentSyncProgress progress;
    QElapsedTimer sinceReport;
    sinceReport.start();
    const auto report = [&](bool force) {
        if (options.onProgress && (force || sinceReport.elapsed() > 250)) {
            options.onProgress(progress);
            sinceReport.restart();
        }
    };

    // 1. Qué documentos hay ahora (con la misma forma de recorrer que la búsqueda).
    struct Found
    {
        QString relative;
        qint64 size;
        qint64 modifiedMs;
    };
    std::vector<Found> toRead;
    QSet<QString> seen;
    search::SearchOptions walk;
    walk.includeHidden = false;
    walk.recursive = true;
    walk.maxHits = std::numeric_limits<qint64>::max();
    const search::SearchSummary walked = search::searchTree(
        {m_root}, search::Query::parse(QStringLiteral("*")).value(), walk, cancel, options.pause,
        [&](std::vector<search::Hit> &&hits, const search::SearchProgress &) {
            for (const search::Hit &hit : hits) {
                if (hit.entry.isDirectory || hit.entry.isSymlink) {
                    continue;
                }
                const QString path = hit.path();
                const content::TextSource source = content::textSourceFor(hit.entry.name);
                if (source == content::TextSource::None
                    || (source == content::TextSource::Plain && hit.entry.size > options.maxPlainBytes)) {
                    continue;
                }
                const QString relative = relativePath(path);
                bool excluded = false;
                for (const QString &name : options.excludedNames) {
                    if (relative.startsWith(name + QLatin1Char('/')) || relative.contains(QLatin1Char('/') + name + QLatin1Char('/'))) {
                        excluded = true;
                        break;
                    }
                }
                if (excluded) {
                    continue;
                }
                // La fecha y el tamaño, como los verá la búsqueda.
                const QFileInfo info(path);
                const qint64 size = info.size();
                const qint64 modifiedMs = info.lastModified().toMSecsSinceEpoch();
                ++progress.found;
                seen.insert(relative);
                const auto known = rows.constFind(relative);
                if (known == rows.constEnd() || known->size != size || known->modifiedMs != modifiedMs) {
                    toRead.push_back({relative, size, modifiedMs});
                }
            }
            report(false);
        });
    if (cancel.isCancelled()) {
        return cancelled();
    }
    if (progress.found == 0 && !rows.isEmpty()) {
        // Vacía pero con documentos anotados: lo más probable es que la unidad
        // no esté montada. No se borra nada por eso.
        return core::Error{core::ErrorCode::Disconnected, tr("La carpeta está vacía o no está conectada"), m_root, 0};
    }
    progress.toRead = qint64(toRead.size());
    report(true);

    // 2. Leer lo nuevo o cambiado, guardando cada poco.
    QSqlQuery insertDoc(database);
    insertDoc.prepare(QStringLiteral("INSERT INTO docs(path, size, mtime, status) VALUES(?, ?, ?, ?)"));
    QSqlQuery updateDoc(database);
    updateDoc.prepare(QStringLiteral("UPDATE docs SET size = ?, mtime = ?, status = ? WHERE id = ?"));
    QSqlQuery deleteBody(database);
    deleteBody.prepare(QStringLiteral("DELETE FROM body WHERE rowid = ?"));
    QSqlQuery insertBody(database);
    insertBody.prepare(QStringLiteral("INSERT INTO body(rowid, text) VALUES(?, ?)"));
    qint64 chars = metaValue(database, QStringLiteral("chars")).toLongLong();
    QElapsedTimer sinceCommit;
    sinceCommit.start();
    int pendingWrites = 0;
    database.transaction();
    const auto commit = [&] {
        setMeta(database, QStringLiteral("chars"), QString::number(chars));
        database.commit();
        database.transaction();
        pendingWrites = 0;
        sinceCommit.restart();
    };
    for (const Found &document : toRead) {
        if (options.pause != nullptr && options.pause->isPaused()) {
            commit();   // lo hecho, guardado antes de esperar
            options.pause->waitWhilePaused(cancel);
        }
        if (cancel.isCancelled()) {
            commit();
            return cancelled();
        }
        const QString path = (m_root.endsWith(QLatin1Char('/')) ? m_root : m_root + QLatin1Char('/'))
            + document.relative;
        progress.current = path;
        report(false);
        const core::Result<QString> text = options.textProvider ? options.textProvider(path, cancel)
                                                                : content::extractText(path, cancel);
        if (!text.ok() && text.error().code == core::ErrorCode::Cancelled) {
            commit();
            return cancelled();
        }
        const bool hasText = text.ok() && !text.value().isEmpty();
        const auto known = rows.constFind(document.relative);
        qint64 id = 0;
        if (known != rows.constEnd()) {
            id = known->id;
            updateDoc.bindValue(0, document.size);
            updateDoc.bindValue(1, document.modifiedMs);
            updateDoc.bindValue(2, hasText ? 0 : 1);
            updateDoc.bindValue(3, id);
            updateDoc.exec();
            if (known->hasText) {
                deleteBody.bindValue(0, id);
                deleteBody.exec();
            }
        } else {
            insertDoc.bindValue(0, document.relative);
            insertDoc.bindValue(1, document.size);
            insertDoc.bindValue(2, document.modifiedMs);
            insertDoc.bindValue(3, hasText ? 0 : 1);
            if (!insertDoc.exec()) {
                database.rollback();
                return core::Error{core::ErrorCode::IoError, insertDoc.lastError().text(), m_databasePath, 0};
            }
            id = insertDoc.lastInsertId().toLongLong();
        }
        rows.insert(document.relative, Row{id, document.size, document.modifiedMs, hasText});
        if (hasText) {
            insertBody.bindValue(0, id);
            insertBody.bindValue(1, text.value());
            if (!insertBody.exec()) {
                database.rollback();
                return core::Error{core::ErrorCode::IoError, insertBody.lastError().text(), m_databasePath, 0};
            }
            chars += text.value().size();
        } else {
            ++progress.failed;
        }
        ++progress.read;
        if (++pendingWrites >= kCommitEvery || sinceCommit.elapsed() > kCommitEveryMs) {
            commit();
        }
    }
    progress.current.clear();

    // 3. Quitar lo que ya no está (solo tras un recorrido completo y sin cancelar).
    if (!walked.truncated) {
        QSqlQuery deleteDoc(database);
        deleteDoc.prepare(QStringLiteral("DELETE FROM docs WHERE id = ?"));
        for (auto row = rows.constBegin(); row != rows.constEnd(); ++row) {
            if (seen.contains(row.key())) {
                continue;
            }
            if (row->hasText) {
                deleteBody.bindValue(0, row->id);
                deleteBody.exec();
            }
            deleteDoc.bindValue(0, row->id);
            deleteDoc.exec();
            ++progress.removed;
        }
        if (progress.removed > 0) {
            // Los caracteres guardados, contados de nuevo.
            QSqlQuery count(database);
            if (count.exec(QStringLiteral("SELECT coalesce(sum(length(text)), 0) FROM body")) && count.next()) {
                chars = count.value(0).toLongLong();
            }
        }
        setMeta(database, QStringLiteral("complete"), QStringLiteral("1"));
        setMeta(database, QStringLiteral("updated"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    }
    setMeta(database, QStringLiteral("chars"), QString::number(chars));
    database.commit();
    report(true);
    return progress;
}

std::unique_ptr<search::ContentCache> ContentIndex::cacheFor(const QList<ContentIndex> &indexes)
{
    if (indexes.isEmpty()) {
        return nullptr;
    }
    return std::make_unique<Cache>(indexes);
}

} // namespace eu::index
