// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/journal/Journal.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUuid>
#include <QVariant>

namespace eu::journal {

namespace {

/// Versión actual del esquema. Cada cambio de tablas añade una migración a
/// `migrations` y sube este número; nunca se edita una migración ya publicada.
constexpr int currentSchemaVersion = 1;

const char *const migrationToV1[] = {
    "CREATE TABLE operations ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  kind INTEGER NOT NULL,"
    "  state INTEGER NOT NULL,"
    "  description TEXT NOT NULL,"
    "  started_ms INTEGER NOT NULL,"
    "  finished_ms INTEGER NOT NULL DEFAULT 0)",
    "CREATE TABLE steps ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  operation_id INTEGER NOT NULL REFERENCES operations(id) ON DELETE CASCADE,"
    "  sequence INTEGER NOT NULL,"
    "  kind INTEGER NOT NULL,"
    "  state INTEGER NOT NULL,"
    "  source TEXT NOT NULL,"
    "  target TEXT NOT NULL,"
    "  result_size INTEGER NOT NULL DEFAULT -1,"
    "  result_modified_ms INTEGER NOT NULL DEFAULT 0)",
    "CREATE INDEX steps_by_operation ON steps(operation_id, sequence)",
    "CREATE INDEX operations_by_state ON operations(state)",
};

core::Error databaseError(const QSqlError &error, const QString &path)
{
    return core::Error{core::ErrorCode::Corrupted,
                       QCoreApplication::translate("eu::journal", "Error en el historial de operaciones: %1")
                           .arg(error.text()),
                       path, 0};
}

Operation readOperation(const QSqlQuery &query)
{
    Operation operation;
    operation.id = query.value(0).toLongLong();
    operation.kind = OperationKind(query.value(1).toInt());
    operation.state = OperationState(query.value(2).toInt());
    operation.description = query.value(3).toString();
    operation.startedMs = query.value(4).toLongLong();
    operation.finishedMs = query.value(5).toLongLong();
    return operation;
}

const QString operationColumns = QStringLiteral("id, kind, state, description, started_ms, finished_ms");

} // namespace

/// Conexión a la base para el hilo actual. Qt exige que cada conexión se use
/// solo desde el hilo que la creó, así que hay una por hilo, identificada por
/// nombre; SQLite en modo WAL admite varias a la vez.
class Journal::Connection
{
public:
    Connection(const Journal &journal)
    {
        const QString name = QStringLiteral("eu-journal-%1-%2")
            .arg(journal.m_instanceId)
            .arg(quintptr(QThread::currentThreadId()));
        if (QSqlDatabase::contains(name)) {
            m_database = QSqlDatabase::database(name, false);
        } else {
            m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            m_database.setDatabaseName(journal.m_filePath);
        }
        if (!m_database.isOpen() && m_database.open()) {
            QSqlQuery setup(m_database);
            // WAL: lectores y un escritor a la vez, y resistente a cortes.
            setup.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
            // FULL: cada confirmación llega al disco antes de seguir. Es más
            // lento, pero el historial es justo lo que debe sobrevivir a un corte.
            setup.exec(QStringLiteral("PRAGMA synchronous=FULL"));
            setup.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
            setup.exec(QStringLiteral("PRAGMA busy_timeout=10000"));
        }
    }

    bool isOpen() const { return m_database.isOpen(); }
    QSqlDatabase &database() { return m_database; }
    QSqlError lastError() const { return m_database.lastError(); }

private:
    QSqlDatabase m_database;
};

Journal::Journal(QString filePath)
    : m_filePath(std::move(filePath)), m_instanceId(QUuid::createUuid().toString(QUuid::Id128))
{
}

Journal::~Journal()
{
    // Se cierran las conexiones que esta instancia abrió en cualquier hilo.
    const QString prefix = QStringLiteral("eu-journal-%1-").arg(m_instanceId);
    const QStringList names = QSqlDatabase::connectionNames();
    for (const QString &name : names) {
        if (name.startsWith(prefix)) {
            QSqlDatabase::removeDatabase(name);
        }
    }
}

core::Result<std::shared_ptr<Journal>> Journal::open(const QString &filePath)
{
    if (!QDir().mkpath(QFileInfo(filePath).absolutePath())) {
        return core::Error{core::ErrorCode::PermissionDenied,
                           QCoreApplication::translate("eu::journal", "No se puede crear la carpeta del historial."),
                           filePath, 0};
    }
    std::shared_ptr<Journal> journal(new Journal(filePath));
    const core::Status status = journal->prepareDatabase();
    if (!status.ok()) {
        return status.error();
    }
    return journal;
}

core::Status Journal::prepareDatabase()
{
    Connection connection(*this);
    if (!connection.isOpen()) {
        return databaseError(connection.lastError(), m_filePath);
    }
    QSqlDatabase &database = connection.database();
    QSqlQuery query(database);

    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
        return databaseError(query.lastError(), m_filePath);
    }
    const int version = query.value(0).toInt();
    query.finish();

    if (version > currentSchemaVersion) {
        // La base la creó una versión más nueva de la aplicación: no se toca.
        return core::Error{core::ErrorCode::NotSupported,
                           QCoreApplication::translate("eu::journal",
                               "El historial fue creado por una versión más reciente de Explorador Ultra."),
                           m_filePath, 0};
    }

    // Migraciones: se aplican en orden, cada una dentro de una transacción.
    if (version < 1) {
        if (!database.transaction()) {
            return databaseError(database.lastError(), m_filePath);
        }
        for (const char *statement : migrationToV1) {
            if (!query.exec(QString::fromLatin1(statement))) {
                const QSqlError error = query.lastError();
                database.rollback();
                return databaseError(error, m_filePath);
            }
        }
        if (!query.exec(QStringLiteral("PRAGMA user_version=1")) || !database.commit()) {
            const QSqlError error = database.lastError();
            database.rollback();
            return databaseError(error, m_filePath);
        }
    }
    return {};
}

int Journal::schemaVersion() const
{
    Connection connection(*this);
    QSqlQuery query(connection.database());
    if (query.exec(QStringLiteral("PRAGMA user_version")) && query.next()) {
        return query.value(0).toInt();
    }
    return -1;
}

core::Result<OperationId> Journal::beginOperation(OperationKind kind, const QString &description)
{
    Connection connection(*this);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("INSERT INTO operations(kind, state, description, started_ms) VALUES(?, ?, ?, ?)"));
    query.addBindValue(int(kind));
    query.addBindValue(int(OperationState::Running));
    query.addBindValue(description);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) {
        return databaseError(query.lastError(), m_filePath);
    }
    return query.lastInsertId().toLongLong();
}

core::Status Journal::finishOperation(OperationId id, OperationState state)
{
    Connection connection(*this);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("UPDATE operations SET state=?, finished_ms=? WHERE id=?"));
    query.addBindValue(int(state));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(id);
    if (!query.exec()) {
        return databaseError(query.lastError(), m_filePath);
    }
    return {};
}

core::Result<StepId> Journal::addStep(OperationId operation, StepKind kind, const QString &source,
                                      const QString &target)
{
    Connection connection(*this);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral(
        "INSERT INTO steps(operation_id, sequence, kind, state, source, target) "
        "VALUES(?, (SELECT COALESCE(MAX(sequence), 0) + 1 FROM steps WHERE operation_id=?), ?, ?, ?, ?)"));
    query.addBindValue(operation);
    query.addBindValue(operation);
    query.addBindValue(int(kind));
    query.addBindValue(int(StepState::Pending));
    query.addBindValue(source);
    query.addBindValue(target);
    if (!query.exec()) {
        return databaseError(query.lastError(), m_filePath);
    }
    return query.lastInsertId().toLongLong();
}

core::Status Journal::completeStep(StepId id, qint64 resultSize, qint64 resultModifiedMs)
{
    Connection connection(*this);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("UPDATE steps SET state=?, result_size=?, result_modified_ms=? WHERE id=?"));
    query.addBindValue(int(StepState::Done));
    query.addBindValue(resultSize);
    query.addBindValue(resultModifiedMs);
    query.addBindValue(id);
    if (!query.exec()) {
        return databaseError(query.lastError(), m_filePath);
    }
    return {};
}

core::Status Journal::setStepState(StepId id, StepState state)
{
    Connection connection(*this);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("UPDATE steps SET state=? WHERE id=?"));
    query.addBindValue(int(state));
    query.addBindValue(id);
    if (!query.exec()) {
        return databaseError(query.lastError(), m_filePath);
    }
    return {};
}

core::Result<Operation> Journal::operation(OperationId id) const
{
    Connection connection(*this);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("SELECT %1 FROM operations WHERE id=?").arg(operationColumns));
    query.addBindValue(id);
    if (!query.exec()) {
        return databaseError(query.lastError(), m_filePath);
    }
    if (!query.next()) {
        return core::Error{core::ErrorCode::NotFound,
                           QCoreApplication::translate("eu::journal", "Esa operación ya no está en el historial."),
                           {}, 0};
    }
    return readOperation(query);
}

core::Result<QList<Step>> Journal::steps(OperationId id) const
{
    Connection connection(*this);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral(
        "SELECT id, operation_id, sequence, kind, state, source, target, result_size, result_modified_ms "
        "FROM steps WHERE operation_id=? ORDER BY sequence"));
    query.addBindValue(id);
    if (!query.exec()) {
        return databaseError(query.lastError(), m_filePath);
    }
    QList<Step> result;
    while (query.next()) {
        Step step;
        step.id = query.value(0).toLongLong();
        step.operationId = query.value(1).toLongLong();
        step.sequence = query.value(2).toInt();
        step.kind = StepKind(query.value(3).toInt());
        step.state = StepState(query.value(4).toInt());
        step.source = query.value(5).toString();
        step.target = query.value(6).toString();
        step.resultSize = query.value(7).toLongLong();
        step.resultModifiedMs = query.value(8).toLongLong();
        result.append(step);
    }
    return result;
}

core::Result<QList<Operation>> Journal::recentOperations(int limit) const
{
    Connection connection(*this);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("SELECT %1 FROM operations ORDER BY id DESC LIMIT ?").arg(operationColumns));
    query.addBindValue(limit);
    if (!query.exec()) {
        return databaseError(query.lastError(), m_filePath);
    }
    QList<Operation> result;
    while (query.next()) {
        result.append(readOperation(query));
    }
    return result;
}

core::Result<QList<Operation>> Journal::incompleteOperations() const
{
    Connection connection(*this);
    QSqlQuery query(connection.database());
    query.prepare(QStringLiteral("SELECT %1 FROM operations WHERE state=? ORDER BY id").arg(operationColumns));
    query.addBindValue(int(OperationState::Running));
    if (!query.exec()) {
        return databaseError(query.lastError(), m_filePath);
    }
    QList<Operation> result;
    while (query.next()) {
        result.append(readOperation(query));
    }
    return result;
}

core::Result<int> Journal::prune(int maxOperations, int maxAgeDays)
{
    Connection connection(*this);
    QSqlQuery query(connection.database());
    const qint64 oldestAllowedMs = QDateTime::currentMSecsSinceEpoch() - qint64(maxAgeDays) * 24 * 3600 * 1000;

    // Las operaciones en curso (o interrumpidas) nunca se purgan: hacen falta
    // para la recuperación.
    query.prepare(QStringLiteral(
        "DELETE FROM operations WHERE state<>? AND ("
        "  started_ms<? OR id NOT IN (SELECT id FROM operations ORDER BY id DESC LIMIT ?))"));
    query.addBindValue(int(OperationState::Running));
    query.addBindValue(oldestAllowedMs);
    query.addBindValue(maxOperations);
    if (!query.exec()) {
        return databaseError(query.lastError(), m_filePath);
    }
    return query.numRowsAffected();
}

QString operationKindName(OperationKind kind)
{
    switch (kind) {
    case OperationKind::Create:  return QStringLiteral("Create");
    case OperationKind::Rename:  return QStringLiteral("Rename");
    case OperationKind::Copy:    return QStringLiteral("Copy");
    case OperationKind::Move:    return QStringLiteral("Move");
    case OperationKind::Trash:   return QStringLiteral("Trash");
    case OperationKind::Restore: return QStringLiteral("Restore");
    case OperationKind::Delete:  return QStringLiteral("Delete");
    }
    return QStringLiteral("?");
}

QString stepKindName(StepKind kind)
{
    switch (kind) {
    case StepKind::MakeDirectory:   return QStringLiteral("MakeDirectory");
    case StepKind::CreateFile:      return QStringLiteral("CreateFile");
    case StepKind::Rename:          return QStringLiteral("Rename");
    case StepKind::CopyFile:        return QStringLiteral("CopyFile");
    case StepKind::CopyLink:        return QStringLiteral("CopyLink");
    case StepKind::RemoveFile:      return QStringLiteral("RemoveFile");
    case StepKind::RemoveDirectory: return QStringLiteral("RemoveDirectory");
    }
    return QStringLiteral("?");
}

} // namespace eu::journal
