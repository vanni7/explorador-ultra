// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/journal/Journal.h"
#include "eu/tasks/Cancel.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace eu::ops {

/// Qué hacer cuando en el destino ya hay algo con el mismo nombre.
/// (Sobrescribir y las demás políticas de SPEC §13 llegan con la cola de
/// transferencias de la Fase 4; aquí nunca se sobrescribe nada.)
enum class CollisionPolicy {
    Skip,       ///< Dejar el destino como está y no copiar ese elemento.
    AutoRename, ///< Copiar con otro nombre: "foto (2).jpg".
};

/// Avance de una operación larga.
struct Progress
{
    qint64 bytesDone = 0;
    qint64 bytesTotal = 0;
    int itemsDone = 0;
    int itemsTotal = 0;
    QString currentPath;
};
using ProgressCallback = std::function<void(const Progress &)>;

/// Resultado de una operación.
struct Outcome
{
    journal::OperationId operationId = 0;   ///< 0 si no llegó a anotarse en el historial.
    QStringList resultPaths;   ///< Lo creado o movido: rutas finales (para seleccionarlas).
    QStringList skipped;       ///< Elementos omitidos por colisión de nombre.
    QList<core::Error> errors; ///< Lo que no se pudo hacer, con su causa.
    bool cancelled = false;

    bool ok() const { return errors.isEmpty() && !cancelled; }
};

/// Motor de operaciones de archivos. Cada método hace una operación completa:
/// la anota en el historial paso a paso **antes** de tocar el disco (SPEC §9)
/// y devuelve qué pasó. Son bloqueantes: se llaman desde una tarea.
///
/// Garantías: nunca se sobrescribe un elemento existente, nunca se siguen
/// enlaces simbólicos, y al mover entre unidades el origen solo se borra
/// después de copiar y sincronizar a disco (SPEC §8.3).
class Engine
{
public:
    explicit Engine(std::shared_ptr<journal::Journal> journal);

    Outcome createFolder(const QString &parentDirectory, const QString &name);
    Outcome createFile(const QString &parentDirectory, const QString &name);
    Outcome rename(const QString &path, const QString &newName);

    Outcome copy(const QStringList &sources, const QString &destinationDirectory, CollisionPolicy policy,
                 const tasks::CancelToken &cancel, const ProgressCallback &progress = {});
    Outcome move(const QStringList &sources, const QString &destinationDirectory, CollisionPolicy policy,
                 const tasks::CancelToken &cancel, const ProgressCallback &progress = {});

    /// Envía a la papelera propia. Si un volumen no admite papelera, ese
    /// elemento queda en `errors` con código NotSupported y no se toca.
    Outcome trash(const QStringList &paths, const tasks::CancelToken &cancel);
    /// Eliminación definitiva. No se puede deshacer.
    Outcome deletePermanently(const QStringList &paths, const tasks::CancelToken &cancel);

    /// true si la operación se puede deshacer (terminó, no era una eliminación
    /// permanente y no se deshizo ya).
    static bool canUndo(const journal::Operation &operation);

    /// Deshace una operación. Antes de cada paso comprueba que el disco está
    /// como lo dejó la operación; lo que cambió desde fuera no se toca y se
    /// informa en `errors` (SPEC §9).
    Outcome undo(journal::OperationId operationId, const tasks::CancelToken &cancel);

    /// Recuperación tras un cierre inesperado (SPEC §9): revierte lo que una
    /// operación interrumpida alcanzó a hacer y limpia sus restos.
    Outcome revertInterrupted(journal::OperationId operationId);
    /// Da por perdida una operación interrumpida: limpia los archivos a medio
    /// escribir y deja hecho lo que ya estaba hecho.
    void discardInterrupted(journal::OperationId operationId);

private:
    class Run;
    std::shared_ptr<journal::Journal> m_journal;
};

} // namespace eu::ops
