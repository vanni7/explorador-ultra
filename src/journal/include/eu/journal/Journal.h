// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"

#include <QList>
#include <QString>

#include <memory>

namespace eu::journal {

/// Qué clase de operación pidió el usuario.
enum class OperationKind {
    Create,   ///< Crear archivo o carpeta.
    Rename,
    Copy,
    Move,
    Trash,    ///< Enviar a la papelera propia.
    Restore,  ///< Sacar de la papelera.
    Delete,   ///< Eliminación permanente (no se puede deshacer).
};

enum class OperationState {
    Running,    ///< En curso… o interrumpida, si la aplicación se cerró de golpe.
    Completed,
    Failed,     ///< Terminó con errores; puede haber pasos hechos.
    Cancelled,
    Undone,     ///< Se deshizo después.
};

/// Qué cambio concreto se hace en el disco. Una operación tiene uno o más pasos.
enum class StepKind {
    MakeDirectory, ///< target = carpeta creada.
    CreateFile,    ///< target = archivo creado.
    Rename,        ///< source -> target, mismo volumen (incluye mover y papelera).
    CopyFile,      ///< source se copió a target.
    CopyLink,      ///< Enlace simbólico copiado como enlace.
    RemoveFile,    ///< source eliminado para siempre.
    RemoveDirectory,
};

enum class StepState {
    Pending, ///< Anotado, aún sin ejecutar (o interrumpido a medias).
    Done,
    Failed,
    Undone,
};

using OperationId = qint64;
using StepId = qint64;

struct Step
{
    StepId id = 0;
    OperationId operationId = 0;
    int sequence = 0;
    StepKind kind = StepKind::Rename;
    StepState state = StepState::Pending;
    QString source;
    QString target;
    /// Huella del resultado al terminar el paso (tamaño y fecha de modificación
    /// del destino). Antes de deshacer se compara con el disco: si no coincide,
    /// alguien lo cambió desde fuera y ese paso no se deshace (SPEC §9).
    qint64 resultSize = -1;
    qint64 resultModifiedMs = 0;
};

struct Operation
{
    OperationId id = 0;
    OperationKind kind = OperationKind::Create;
    OperationState state = OperationState::Running;
    QString description;   ///< Texto para mostrar: "Copiar 3 elementos a Fotos".
    qint64 startedMs = 0;
    qint64 finishedMs = 0;
};

/// Historial de operaciones en una base de datos SQLite (modo WAL).
///
/// Regla de oro (write-ahead): cada paso se anota con addStep() **antes** de
/// tocar el disco y se confirma con completeStep() después. Si la aplicación
/// muere entre medias, al reabrir incompleteOperations() dice exactamente qué
/// quedó a medias.
///
/// Se puede usar desde cualquier hilo: cada hilo abre su propia conexión.
class Journal
{
public:
    /// Abre (y crea o actualiza si hace falta) la base de datos en `filePath`.
    static core::Result<std::shared_ptr<Journal>> open(const QString &filePath);
    ~Journal();

    Journal(const Journal &) = delete;
    Journal &operator=(const Journal &) = delete;

    core::Result<OperationId> beginOperation(OperationKind kind, const QString &description);
    core::Status finishOperation(OperationId id, OperationState state);

    core::Result<StepId> addStep(OperationId operation, StepKind kind, const QString &source, const QString &target);
    core::Status completeStep(StepId id, qint64 resultSize, qint64 resultModifiedMs);
    core::Status setStepState(StepId id, StepState state);

    core::Result<Operation> operation(OperationId id) const;
    core::Result<QList<Step>> steps(OperationId id) const;
    /// Las más recientes primero.
    core::Result<QList<Operation>> recentOperations(int limit) const;
    /// Operaciones que quedaron en estado Running: solo puede pasar si la
    /// aplicación se cerró inesperadamente.
    core::Result<QList<Operation>> incompleteOperations() const;

    /// Borra del historial lo más antiguo: se conservan como mucho
    /// `maxOperations` y nada anterior a `maxAgeDays` (SPEC §9: 10 000 y 30).
    /// Nunca borra operaciones en curso. Devuelve cuántas eliminó.
    core::Result<int> prune(int maxOperations, int maxAgeDays);

    /// Versión del esquema de la base (para pruebas de migración).
    int schemaVersion() const;

private:
    explicit Journal(QString filePath);
    core::Status prepareDatabase();

    class Connection;
    QString m_filePath;
    QString m_instanceId;   ///< Distingue las conexiones de varias instancias de Journal.
};

/// Nombres estables para el registro (no se traducen).
QString operationKindName(OperationKind kind);
QString stepKindName(StepKind kind);

} // namespace eu::journal
