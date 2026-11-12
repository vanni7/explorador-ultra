// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/journal/Journal.h"
#include "eu/ops/Engine.h"
#include "eu/tasks/Scheduler.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <functional>
#include <memory>

namespace eu::ui {

/// Qué pidió el usuario.
enum class ActionKind {
    CreateFolder,
    CreateFile,
    Rename,
    Copy,
    Move,
    Trash,
    Delete,   ///< Eliminación permanente.
    Undo,
};

/// Lo que pasó con una acción, ya de vuelta en el hilo de interfaz.
struct ActionResult
{
    quint64 id = 0;
    ActionKind kind = ActionKind::Copy;
    /// Carpeta donde quedó el resultado (para seleccionarlo si está a la vista).
    QString directory;
    ops::Outcome outcome;
    /// Solo en Deshacer: descripción de la operación que se deshizo.
    QString undoneDescription;
    /// Solo en Deshacer: no había ninguna operación que se pudiera deshacer.
    bool nothingToUndo = false;
};

/// Puente entre la interfaz y el motor de operaciones.
///
/// Cada método encola una tarea en el planificador y vuelve enseguida: el hilo
/// de interfaz nunca toca el disco. Cuando la tarea termina, el resultado llega
/// con la señal finished(), ya en el hilo de interfaz.
///
/// Las tareas no usan este objeto desde su hilo: trabajan con copias de los
/// datos y devuelven el resultado a través de la aplicación, donde se comprueba
/// que el objeto sigue vivo. Así cerrar la ventana a mitad de una copia es seguro.
class FileActions : public QObject
{
    Q_OBJECT

public:
    FileActions(tasks::Scheduler &scheduler, std::shared_ptr<journal::Journal> journal, QObject *parent = nullptr);
    ~FileActions() override;

    /// false si el historial no se pudo abrir: sin historial no se modifica
    /// nada (la regla es anotar antes de actuar, SPEC §9).
    bool isAvailable() const { return m_journal != nullptr; }

    /// Número de acciones en curso o en cola.
    int runningCount() const { return int(m_running.size()); }
    bool isBusy() const { return !m_running.isEmpty(); }

    quint64 createFolder(const QString &parentDirectory, const QString &name);
    quint64 createFile(const QString &parentDirectory, const QString &name);
    quint64 rename(const QString &path, const QString &newName);
    quint64 copy(const QStringList &sources, const QString &destinationDirectory, ops::CollisionPolicy policy);
    quint64 move(const QStringList &sources, const QString &destinationDirectory, ops::CollisionPolicy policy);
    quint64 trash(const QStringList &paths);
    quint64 deletePermanently(const QStringList &paths);
    /// Deshace la operación más reciente que se pueda deshacer.
    quint64 undoLast();

public slots:
    /// Pide a todas las acciones en curso que se detengan. Terminan de forma
    /// ordenada y avisan con finished() (con `cancelled` en su resultado).
    void cancelAll();

signals:
    void started(quint64 id, eu::ui::ActionKind kind);
    void progressed(quint64 id, const eu::ops::Progress &progress);
    void finished(const eu::ui::ActionResult &result);
    /// Cambió runningCount().
    void runningCountChanged(int count);

private:
    struct Context
    {
        std::shared_ptr<journal::Journal> journal;
        tasks::CancelToken cancel;
        ops::ProgressCallback progress;
    };
    using Work = std::function<void(const Context &, ActionResult &)>;

    quint64 start(ActionKind kind, const QString &directory, tasks::Category category, Work work);
    void onFinished(const ActionResult &result);

    tasks::Scheduler &m_scheduler;
    std::shared_ptr<journal::Journal> m_journal;
    QHash<quint64, tasks::TaskHandle> m_running;
    quint64 m_lastId = 0;
    /// Operaciones que ya se intentó deshacer sin conseguirlo del todo: no se
    /// vuelven a intentar, para que Deshacer no se quede atascado en ellas.
    QSet<journal::OperationId> m_undoFailed;
};

} // namespace eu::ui
