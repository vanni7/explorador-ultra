// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/archive/Archive.h"
#include "eu/journal/Journal.h"
#include "eu/ops/Engine.h"
#include "eu/tasks/Scheduler.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <functional>
#include <atomic>
#include <memory>
#include <vector>

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
    Revert,    ///< Revertir una operación que quedó a medias por un cierre inesperado.
    Discard,   ///< Darla por perdida: limpiar los restos y dejar lo ya hecho.
    RestoreTrash,   ///< Sacar elementos de la papelera a su sitio original.
    PurgeTrash,     ///< Eliminar para siempre elementos de la papelera.
    Extract,        ///< Sacar de un comprimido.
    Compress,       ///< Crear un comprimido.
};

/// Lo que pasó con una acción, ya de vuelta en el hilo de interfaz.
struct ActionResult
{
    quint64 id = 0;
    ActionKind kind = ActionKind::Copy;
    /// Carpeta donde quedó el resultado (para seleccionarlo si está a la vista).
    QString directory;
    ops::Outcome outcome;
    /// En Deshacer, Revertir y Descartar: descripción de la operación afectada.
    QString undoneDescription;
    /// Solo en Deshacer: no había ninguna operación que se pudiera deshacer.
    bool nothingToUndo = false;
    /// Se canceló antes de empezar: no llegó a tocar nada.
    bool neverRan = false;
};

/// Cómo va una copia o un movimiento de la cola de transferencias (SPEC §13).
struct TransferInfo
{
    enum class State { Queued, Running, Paused, Finished, Failed, Cancelled };

    quint64 id = 0;
    ActionKind kind = ActionKind::Copy;
    QString description;        ///< "Copiar 3 elementos a «Fotos»".
    State state = State::Queued;
    ops::Progress progress;
    double bytesPerSecond = 0;  ///< Media de los últimos segundos; 0 si no se mueve.
    qint64 secondsLeft = -1;    ///< -1: no se sabe.
    QString problem;            ///< Resumen de lo que falló.
    bool canRetry = false;
    bool verifies = false;      ///< Comprueba cada copia con su hash.

    bool isActive() const { return state == State::Queued || state == State::Running || state == State::Paused; }
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
    int runningCount() const;
    bool isBusy() const { return runningCount() > 0; }

    /// La cola de transferencias (copiar y mover), en su orden.
    QList<TransferInfo> transfers() const;
    /// Suma de las transferencias activas: avance, velocidad y tiempo restante.
    TransferInfo overallTransfer() const;
    void pauseTransfer(quint64 id);
    void resumeTransfer(quint64 id);
    void cancelTransfer(quint64 id);
    /// Vuelve a poner en la cola lo que quedó sin terminar. Devuelve su id (0 si no hay nada).
    quint64 retryTransfer(quint64 id);
    /// La pasa a la primera de las que esperan.
    void prioritizeTransfer(quint64 id);
    /// Quita de la lista las que ya acabaron.
    void clearFinishedTransfers();
    /// true (de fábrica): una transferencia detrás de otra. false: todas a la
    /// vez (el planificador sigue sin juntar dos en el mismo disco lento).
    void setSequentialTransfers(bool sequential);
    bool sequentialTransfers() const { return m_sequential; }

    quint64 createFolder(const QString &parentDirectory, const QString &name);
    quint64 createFile(const QString &parentDirectory, const QString &name);
    quint64 rename(const QString &path, const QString &newName);
    quint64 copy(const QStringList &sources, const QString &destinationDirectory, const ops::TransferOptions &policy);
    quint64 move(const QStringList &sources, const QString &destinationDirectory, const ops::TransferOptions &policy);
    /// Extrae de `archivePath` lo que hay en `innerPaths` (vacío: todo) en una
    /// carpeta nueva `folderName` dentro de `destinationDirectory`. Se quita
    /// `innerFolder` del principio de las rutas (la carpeta de dentro donde
    /// estaba el usuario). Nunca sobrescribe ni escribe fuera de la carpeta.
    quint64 extract(const QString &archivePath, const QStringList &innerPaths, const QString &innerFolder,
                    const QString &destinationDirectory, const QString &folderName);
    /// Crea `outputPath` con `sources`. No sobrescribe.
    quint64 compress(const QStringList &sources, const QString &outputPath, archive::Format format);
    quint64 trash(const QStringList &paths);
    quint64 deletePermanently(const QStringList &paths);
    /// Deshace la operación más reciente que se pueda deshacer.
    quint64 undoLast();

    /// Tareas de arranque sobre el historial, en segundo plano: borra lo más
    /// antiguo (SPEC §9: se conservan 30 días o 10 000 operaciones) y busca
    /// operaciones que quedaron a medias. Si hay alguna, avisa con
    /// interruptedOperationsFound().
    void checkJournalAtStartup();
    quint64 revertInterrupted(const journal::Operation &operation);
    quint64 discardInterrupted(const journal::Operation &operation);

    /// Lee en segundo plano la papelera de todas las unidades conectadas y
    /// avisa con trashListed().
    void listTrash();
    quint64 restoreFromTrash(const QList<trash::TrashItem> &items);
    quint64 purgeFromTrash(const QList<trash::TrashItem> &items);

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
    /// Cambió algo de la cola de transferencias (estado, avance, orden).
    void transfersChanged();
    /// Hay operaciones que no terminaron porque la aplicación se cerró de golpe.
    void interruptedOperationsFound(const QList<eu::journal::Operation> &operations);
    /// Contenido de la papelera, del más reciente al más antiguo.
    void trashListed(const QList<eu::trash::TrashItem> &items);

private:
    struct Context
    {
        std::shared_ptr<journal::Journal> journal;
        tasks::CancelToken cancel;
        ops::ProgressCallback progress;
    };
    using Work = std::function<void(const Context &, ActionResult &)>;

    /// `devicePaths`: rutas que dicen sobre qué unidades trabaja (para que el
    /// planificador no lance dos transferencias a la vez al mismo disco lento).
    quint64 start(ActionKind kind, const QString &directory, tasks::Category category, Work work,
                  const QStringList &devicePaths = {});
    void launch(quint64 id, ActionKind kind, const QString &directory, tasks::Category category, Work work,
                const QStringList &devicePaths);
    void onFinished(const ActionResult &result);
    void onProgress(quint64 id, const ops::Progress &progress);
    /// Cancela una tarea y, si no llega a empezar (el planificador no ejecuta
    /// las canceladas), da la acción por terminada igualmente.
    void cancelTask(quint64 id);

    using TransferRun = std::function<ops::Outcome(const ops::TransferOptions &, const Context &)>;
    struct Transfer;
    quint64 queueTransfer(ActionKind kind, const QString &description, const QString &directory,
                          const QStringList &devicePaths, ops::TransferOptions options, TransferRun run);
    void startQueuedTransfers();
    Transfer *transferById(quint64 id);

    tasks::Scheduler &m_scheduler;
    std::shared_ptr<journal::Journal> m_journal;
    QHash<quint64, tasks::TaskHandle> m_running;
    QHash<quint64, ActionResult> m_initial;   ///< Para dar por terminada una que no llegó a empezar.
    QHash<quint64, std::shared_ptr<std::atomic<bool>>> m_ran;
    std::vector<std::unique_ptr<Transfer>> m_transfers;   ///< En el orden de la cola.
    bool m_sequential = true;
    quint64 m_lastId = 0;
    /// Operaciones que ya se intentó deshacer sin conseguirlo del todo: no se
    /// vuelven a intentar, para que Deshacer no se quede atascado en ellas.
    QSet<journal::OperationId> m_undoFailed;
};

} // namespace eu::ui
