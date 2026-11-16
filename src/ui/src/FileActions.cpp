// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/FileActions.h"

#include "eu/search/Searcher.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>

namespace eu::ui {

namespace {

/// Error que se devuelve cuando no hay historial.
core::Error journalUnavailable()
{
    return {core::ErrorCode::NotSupported,
            QCoreApplication::translate("eu::ui::FileActions",
                                        "El historial de operaciones no está disponible; no se modifica nada."),
            {}, 0};
}

/// Milisegundos de un reloj que nunca retrocede.
qint64 steadyNowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

struct FileActions::Transfer
{
    TransferInfo info;
    QString directory;
    QStringList devicePaths;
    ops::TransferOptions options;
    TransferRun run;
    std::shared_ptr<search::PauseGate> gate = std::make_shared<search::PauseGate>();
    QList<QPair<QString, QString>> unfinished;
    std::deque<std::pair<qint64, qint64>> samples;   ///< (ms, bytes hechos), para la velocidad.
    bool neverStarted = true;
};

FileActions::FileActions(tasks::Scheduler &scheduler, std::shared_ptr<journal::Journal> journal, QObject *parent)
    : QObject(parent), m_scheduler(scheduler), m_journal(std::move(journal))
{
}

FileActions::~FileActions()
{
    // Las tareas no tocan este objeto, así que no hace falta esperarlas: basta
    // pedirles que paren. El planificador las espera al destruirse.
    for (const auto &transfer : m_transfers) {
        transfer->gate->setPaused(false);
    }
    for (tasks::TaskHandle &task : m_running) {
        task.cancel();
    }
}

void FileActions::cancelAll()
{
    for (const auto &transfer : m_transfers) {
        if (transfer->info.state == TransferInfo::State::Queued) {
            transfer->info.state = TransferInfo::State::Cancelled;
            transfer->info.canRetry = true;
        }
        transfer->gate->setPaused(false);
    }
    for (const quint64 id : m_running.keys()) {
        cancelTask(id);
    }
    emit runningCountChanged(runningCount());
    emit transfersChanged();
}

void FileActions::cancelTask(quint64 id)
{
    const auto running = m_running.find(id);
    if (running == m_running.end()) {
        return;
    }
    running->cancel();
    // Si estaba esperando turno, el planificador la descarta sin ejecutarla:
    // se mira cada poco y, cuando conste como terminada sin haber empezado,
    // se da por cancelada aquí. El temporizador se borra solo al acabar.
    auto *timer = new QTimer(this);
    timer->setInterval(50);
    connect(timer, &QTimer::timeout, this, [this, id, timer] {
        const auto ran = m_ran.value(id);
        if (!m_running.contains(id) || !ran || ran->load()) {
            timer->deleteLater();   // ya terminó, o empezó y avisará ella misma
            return;
        }
        if (m_running.value(id).isFinished()) {
            timer->deleteLater();
            ActionResult result = m_initial.value(id);
            result.outcome.cancelled = true;
            result.neverRan = true;
            onFinished(result);
        }
    });
    timer->start();
}

int FileActions::runningCount() const
{
    int queued = 0;
    for (const auto &transfer : m_transfers) {
        if (transfer->info.state == TransferInfo::State::Queued) {
            ++queued;
        }
    }
    return int(m_running.size()) + queued;
}

quint64 FileActions::start(ActionKind kind, const QString &directory, tasks::Category category, Work work,
                           const QStringList &devicePaths)
{
    const quint64 id = ++m_lastId;
    launch(id, kind, directory, category, std::move(work), devicePaths);
    emit started(id, kind);
    emit runningCountChanged(runningCount());
    return id;
}

void FileActions::launch(quint64 id, ActionKind kind, const QString &directory, tasks::Category category, Work work,
                         const QStringList &devicePaths)
{
    ActionResult initial;
    initial.id = id;
    initial.kind = kind;
    initial.directory = directory;

    if (!m_journal) {
        initial.outcome.errors.append(journalUnavailable());
        // Se avisa en la siguiente vuelta del bucle de eventos, igual que una
        // acción real: quien llama siempre recibe finished() después de volver.
        QMetaObject::invokeMethod(this, [this, initial] { onFinished(initial); }, Qt::QueuedConnection);
        return;
    }

    const QPointer<FileActions> self(this);
    const std::shared_ptr<journal::Journal> journal = m_journal;
    const auto ran = std::make_shared<std::atomic<bool>>(false);
    m_ran.insert(id, ran);
    m_initial.insert(id, initial);
    // Copiar y mover esperan su turno detrás de lo que el usuario acaba de pedir.
    const tasks::Priority priority = (kind == ActionKind::Copy || kind == ActionKind::Move)
        ? tasks::Priority::ActiveTransfer
        : tasks::Priority::UserInteraction;

    const tasks::TaskHandle handle = m_scheduler.submitForPaths(
        category, priority, devicePaths, [self, journal, initial, ran, work = std::move(work)](const tasks::CancelToken &cancel) {
            ran->store(true);
            Context context;
            context.journal = journal;
            context.cancel = cancel;
            // El avance puede llegar miles de veces por segundo: a la interfaz
            // se le pasa como mucho unas diez veces por segundo.
            const auto lastSentMs = std::make_shared<std::atomic<qint64>>(0);
            context.progress = [self, id = initial.id, lastSentMs](const ops::Progress &progress) {
                const qint64 now = steadyNowMs();
                if (now - lastSentMs->load() < 100) {
                    return;
                }
                lastSentMs->store(now);
                QMetaObject::invokeMethod(qApp, [self, id, progress] {
                    if (self) {
                        self->onProgress(id, progress);
                    }
                }, Qt::QueuedConnection);
            };

            ActionResult result = initial;
            work(context, result);
            QMetaObject::invokeMethod(qApp, [self, result] {
                if (self) {
                    self->onFinished(result);
                }
            }, Qt::QueuedConnection);
        });

    m_running.insert(id, handle);
}

void FileActions::onProgress(quint64 id, const ops::Progress &progress)
{
    if (Transfer *transfer = transferById(id)) {
        transfer->info.progress = progress;
        // Velocidad: lo avanzado en los últimos 5 segundos.
        const qint64 now = steadyNowMs();
        transfer->samples.emplace_back(now, progress.bytesDone);
        while (transfer->samples.size() > 2 && now - transfer->samples.front().first > 5000) {
            transfer->samples.pop_front();
        }
        const auto &[firstMs, firstBytes] = transfer->samples.front();
        const qint64 elapsedMs = now - firstMs;
        transfer->info.bytesPerSecond = elapsedMs >= 500 ? double(progress.bytesDone - firstBytes) * 1000.0 / double(elapsedMs)
                                                          : transfer->info.bytesPerSecond;
        const qint64 left = progress.bytesTotal - progress.bytesDone;
        transfer->info.secondsLeft = transfer->info.bytesPerSecond > 1.0 && left >= 0
            ? qint64(double(left) / transfer->info.bytesPerSecond + 0.5)
            : -1;
        emit transfersChanged();
    }
    emit progressed(id, progress);
}

void FileActions::onFinished(const ActionResult &result)
{
    m_running.remove(result.id);
    m_ran.remove(result.id);
    m_initial.remove(result.id);
    if (Transfer *transfer = transferById(result.id)) {
        const ops::Outcome &outcome = result.outcome;
        transfer->unfinished = outcome.unfinished;
        transfer->info.bytesPerSecond = 0;
        transfer->info.secondsLeft = -1;
        if (outcome.cancelled) {
            transfer->info.state = TransferInfo::State::Cancelled;
        } else if (!outcome.errors.isEmpty()) {
            transfer->info.state = TransferInfo::State::Failed;
            transfer->info.problem = outcome.errors.size() == 1
                ? outcome.errors.first().message
                : tr("%1 (y %n problema(s) más)", nullptr, int(outcome.errors.size()) - 1)
                      .arg(outcome.errors.first().message);
        } else {
            transfer->info.state = TransferInfo::State::Finished;
            transfer->info.progress.bytesDone = transfer->info.progress.bytesTotal;
            transfer->info.progress.itemsDone = transfer->info.progress.itemsTotal;
        }
        if (result.neverRan) {
            transfer->neverStarted = true;   // reintentar es empezarla tal cual
        }
        transfer->info.canRetry = !transfer->unfinished.isEmpty() || result.neverRan;
        startQueuedTransfers();
        emit transfersChanged();
    }
    if (result.kind == ActionKind::Undo && !result.nothingToUndo && !result.outcome.ok()) {
        m_undoFailed.insert(result.outcome.operationId);
    }
    emit runningCountChanged(runningCount());
    emit finished(result);
}

quint64 FileActions::createFolder(const QString &parentDirectory, const QString &name)
{
    return start(ActionKind::CreateFolder, parentDirectory, tasks::Category::General,
                 [parentDirectory, name](const Context &context, ActionResult &result) {
        result.outcome = ops::Engine(context.journal).createFolder(parentDirectory, name);
    });
}

quint64 FileActions::createFile(const QString &parentDirectory, const QString &name)
{
    return start(ActionKind::CreateFile, parentDirectory, tasks::Category::General,
                 [parentDirectory, name](const Context &context, ActionResult &result) {
        result.outcome = ops::Engine(context.journal).createFile(parentDirectory, name);
    });
}

quint64 FileActions::rename(const QString &path, const QString &newName)
{
    // La carpeta se calcula sobre el texto de la ruta: sin tocar el disco.
    const QString directory = QFileInfo(QDir::cleanPath(path)).path();
    return start(ActionKind::Rename, directory, tasks::Category::General,
                 [path, newName](const Context &context, ActionResult &result) {
        result.outcome = ops::Engine(context.journal).rename(path, newName);
    });
}

quint64 FileActions::copy(const QStringList &sources, const QString &destinationDirectory,
                          const ops::TransferOptions &policy)
{
    const QString folder = QDir(destinationDirectory).dirName();
    return queueTransfer(ActionKind::Copy,
                         sources.size() == 1 ? tr("Copiar «%1» a «%2»").arg(QFileInfo(sources.first()).fileName(), folder)
                                             : tr("Copiar %1 elementos a «%2»").arg(sources.size()).arg(folder),
                         destinationDirectory, QStringList(sources) << destinationDirectory, policy,
                         [sources, destinationDirectory](const ops::TransferOptions &options, const Context &context) {
        return ops::Engine(context.journal).copy(sources, destinationDirectory, options, context.cancel, context.progress);
    });
}

quint64 FileActions::move(const QStringList &sources, const QString &destinationDirectory,
                          const ops::TransferOptions &policy)
{
    const QString folder = QDir(destinationDirectory).dirName();
    return queueTransfer(ActionKind::Move,
                         sources.size() == 1 ? tr("Mover «%1» a «%2»").arg(QFileInfo(sources.first()).fileName(), folder)
                                             : tr("Mover %1 elementos a «%2»").arg(sources.size()).arg(folder),
                         destinationDirectory, QStringList(sources) << destinationDirectory, policy,
                         [sources, destinationDirectory](const ops::TransferOptions &options, const Context &context) {
        return ops::Engine(context.journal).move(sources, destinationDirectory, options, context.cancel, context.progress);
    });
}

quint64 FileActions::queueTransfer(ActionKind kind, const QString &description, const QString &directory,
                                   const QStringList &devicePaths, ops::TransferOptions options, TransferRun run)
{
    auto transfer = std::make_unique<Transfer>();
    transfer->info.id = ++m_lastId;
    transfer->info.kind = kind;
    transfer->info.description = description;
    transfer->info.verifies = options.copy.verify;
    transfer->directory = directory;
    transfer->devicePaths = devicePaths;
    // La pausa se mira entre bloque y bloque de cada archivo.
    options.copy.pausePoint = [gate = transfer->gate](const tasks::CancelToken &cancel) {
        gate->waitWhilePaused(cancel);
    };
    transfer->options = std::move(options);
    transfer->run = std::move(run);
    const quint64 id = transfer->info.id;
    m_transfers.push_back(std::move(transfer));
    emit started(id, kind);
    startQueuedTransfers();
    emit runningCountChanged(runningCount());
    emit transfersChanged();
    return id;
}

void FileActions::startQueuedTransfers()
{
    for (const auto &transfer : m_transfers) {
        const TransferInfo::State state = transfer->info.state;
        if (m_sequential && (state == TransferInfo::State::Running || state == TransferInfo::State::Paused)) {
            return;   // de una en una: espera a que acabe la que está en marcha
        }
        if (state != TransferInfo::State::Queued) {
            continue;
        }
        transfer->info.state = TransferInfo::State::Running;
        transfer->neverStarted = false;
        transfer->samples.clear();
        launch(transfer->info.id, transfer->info.kind, transfer->directory, tasks::Category::Transfer,
               [run = transfer->run, options = transfer->options](const Context &context, ActionResult &result) {
            result.outcome = run(options, context);
        }, transfer->devicePaths);
        if (m_sequential) {
            return;
        }
    }
}

FileActions::Transfer *FileActions::transferById(quint64 id)
{
    for (const auto &transfer : m_transfers) {
        if (transfer->info.id == id) {
            return transfer.get();
        }
    }
    return nullptr;
}

QList<TransferInfo> FileActions::transfers() const
{
    QList<TransferInfo> all;
    for (const auto &transfer : m_transfers) {
        all.append(transfer->info);
    }
    return all;
}

TransferInfo FileActions::overallTransfer() const
{
    TransferInfo total;
    total.state = TransferInfo::State::Finished;
    qint64 bytesLeft = 0;
    for (const auto &transfer : m_transfers) {
        const TransferInfo &info = transfer->info;
        if (!info.isActive()) {
            continue;
        }
        total.state = TransferInfo::State::Running;
        total.progress.bytesDone += info.progress.bytesDone;
        total.progress.bytesTotal += info.progress.bytesTotal;
        total.progress.itemsDone += info.progress.itemsDone;
        total.progress.itemsTotal += info.progress.itemsTotal;
        if (info.state == TransferInfo::State::Running) {
            total.bytesPerSecond += info.bytesPerSecond;
        }
        bytesLeft += std::max<qint64>(0, info.progress.bytesTotal - info.progress.bytesDone);
    }
    total.secondsLeft = total.bytesPerSecond > 1.0 ? qint64(double(bytesLeft) / total.bytesPerSecond + 0.5) : -1;
    return total;
}

void FileActions::pauseTransfer(quint64 id)
{
    Transfer *transfer = transferById(id);
    if (transfer != nullptr && transfer->info.state == TransferInfo::State::Running) {
        transfer->gate->setPaused(true);
        transfer->info.state = TransferInfo::State::Paused;
        transfer->info.bytesPerSecond = 0;
        transfer->info.secondsLeft = -1;
        transfer->samples.clear();
        emit transfersChanged();
    }
}

void FileActions::resumeTransfer(quint64 id)
{
    Transfer *transfer = transferById(id);
    if (transfer != nullptr && transfer->info.state == TransferInfo::State::Paused) {
        transfer->gate->setPaused(false);
        transfer->info.state = TransferInfo::State::Running;
        emit transfersChanged();
    }
}

void FileActions::cancelTransfer(quint64 id)
{
    Transfer *transfer = transferById(id);
    if (transfer == nullptr) {
        return;
    }
    if (transfer->info.state == TransferInfo::State::Queued) {
        transfer->info.state = TransferInfo::State::Cancelled;
        transfer->info.canRetry = true;   // nada hecho: reintentar es empezarla
        emit runningCountChanged(runningCount());
        emit transfersChanged();
        return;
    }
    transfer->gate->setPaused(false);
    cancelTask(id);
}

quint64 FileActions::retryTransfer(quint64 id)
{
    Transfer *transfer = transferById(id);
    if (transfer == nullptr || !transfer->info.canRetry || transfer->info.isActive()) {
        return 0;
    }
    transfer->info.canRetry = false;
    ops::TransferOptions options = transfer->options;
    if (transfer->neverStarted) {
        // Se canceló antes de empezar: se pone otra vez tal cual.
        return queueTransfer(transfer->info.kind, transfer->info.description, transfer->directory,
                             transfer->devicePaths, options, transfer->run);
    }
    const QList<QPair<QString, QString>> items = transfer->unfinished;
    const bool moving = transfer->info.kind == ActionKind::Move;
    return queueTransfer(transfer->info.kind, tr("Reintentar: %1").arg(transfer->info.description),
                         transfer->directory, transfer->devicePaths, options,
                         [items, moving](const ops::TransferOptions &again, const Context &context) {
        return ops::Engine(context.journal).retry(items, moving, again, context.cancel, context.progress);
    });
}

void FileActions::prioritizeTransfer(quint64 id)
{
    const auto found = std::find_if(m_transfers.begin(), m_transfers.end(),
                                    [id](const auto &transfer) { return transfer->info.id == id; });
    if (found == m_transfers.end() || (*found)->info.state != TransferInfo::State::Queued) {
        return;
    }
    // Delante de la primera que espera.
    const auto firstQueued = std::find_if(m_transfers.begin(), m_transfers.end(), [](const auto &transfer) {
        return transfer->info.state == TransferInfo::State::Queued;
    });
    std::rotate(firstQueued, found, found + 1);
    emit transfersChanged();
}

void FileActions::clearFinishedTransfers()
{
    m_transfers.erase(std::remove_if(m_transfers.begin(), m_transfers.end(),
                                     [](const auto &transfer) { return !transfer->info.isActive(); }),
                      m_transfers.end());
    emit transfersChanged();
}

void FileActions::setSequentialTransfers(bool sequential)
{
    if (m_sequential == sequential) {
        return;
    }
    m_sequential = sequential;
    startQueuedTransfers();
    emit transfersChanged();
}

quint64 FileActions::extract(const QString &archivePath, const QStringList &innerPaths, const QString &innerFolder,
                             const QString &destinationDirectory, const QString &folderName)
{
    return start(ActionKind::Extract, destinationDirectory, tasks::Category::Transfer,
                 [archivePath, innerPaths, innerFolder, destinationDirectory, folderName](const Context &context,
                                                                                         ActionResult &result) {
        // Siempre en una carpeta nueva: lo extraído nunca se mezcla con lo que
        // ya hay ni pisa nada.
        const QString target = destinationDirectory + QLatin1Char('/') + folderName;
        if (!QDir().mkdir(target)) {
            result.outcome.errors.append(core::Error{core::ErrorCode::AlreadyExists,
                                                     QCoreApplication::translate("eu::ui", "No se pudo crear la carpeta"),
                                                     target, 0});
            return;
        }
        archive::ExtractOptions options;
        options.only = innerPaths;
        options.stripPrefix = innerFolder;
        const core::Result<archive::ExtractResult> extracted = archive::extractArchive(
            archivePath, target, options, context.cancel, [&context](qint64 bytes) {
                if (context.progress) {
                    ops::Progress progress;
                    progress.bytesDone = bytes;
                    context.progress(progress);
                }
            });
        result.outcome.resultPaths.append(target);
        if (!extracted.ok()) {
            if (extracted.error().code == core::ErrorCode::Cancelled) {
                result.outcome.cancelled = true;
            } else {
                result.outcome.errors.append(extracted.error());
            }
            return;
        }
        result.outcome.skipped = extracted.value().skipped;
        for (const QString &skipped : extracted.value().skipped) {
            qWarning("Extraer %s: omitido %s", qPrintable(archivePath), qPrintable(skipped));
        }
    }, {archivePath, destinationDirectory});
}

quint64 FileActions::compress(const QStringList &sources, const QString &outputPath, archive::Format format)
{
    return start(ActionKind::Compress, QFileInfo(outputPath).path(), tasks::Category::Transfer,
                 [sources, outputPath, format](const Context &context, ActionResult &result) {
        const core::Status created = archive::createArchive(outputPath, format, sources, context.cancel,
                                                            [&context](qint64 bytes) {
            if (context.progress) {
                ops::Progress progress;
                progress.bytesDone = bytes;
                context.progress(progress);
            }
        });
        if (!created.ok()) {
            if (created.error().code == core::ErrorCode::Cancelled) {
                result.outcome.cancelled = true;
            } else {
                result.outcome.errors.append(created.error());
            }
            return;
        }
        result.outcome.resultPaths.append(outputPath);
    }, QStringList(sources) << outputPath);
}

quint64 FileActions::trash(const QStringList &paths)
{
    // Enviar a la papelera es un renombrado por elemento: rápido, no ocupa un
    // hilo de transferencias.
    return start(ActionKind::Trash, {}, tasks::Category::General,
                 [paths](const Context &context, ActionResult &result) {
        result.outcome = ops::Engine(context.journal).trash(paths, context.cancel);
    });
}

quint64 FileActions::deletePermanently(const QStringList &paths)
{
    return start(ActionKind::Delete, {}, tasks::Category::Transfer,
                 [paths](const Context &context, ActionResult &result) {
        result.outcome = ops::Engine(context.journal).deletePermanently(paths, context.cancel);
    });
}

quint64 FileActions::undoLast()
{
    const QSet<journal::OperationId> skip = m_undoFailed;
    return start(ActionKind::Undo, {}, tasks::Category::Transfer,
                 [skip](const Context &context, ActionResult &result) {
        // Se mira lo más reciente del historial; con 200 operaciones sobra
        // para encontrar la última que se pueda deshacer.
        const auto recent = context.journal->recentOperations(200);
        if (!recent.ok()) {
            result.outcome.errors.append(recent.error());
            return;
        }
        for (const journal::Operation &operation : recent.value()) {
            if (skip.contains(operation.id) || !ops::Engine::canUndo(operation)) {
                continue;
            }
            result.undoneDescription = operation.description;
            result.outcome = ops::Engine(context.journal).undo(operation.id, context.cancel);
            return;
        }
        result.nothingToUndo = true;
    });
}

void FileActions::checkJournalAtStartup()
{
    if (!m_journal) {
        return;
    }
    const QPointer<FileActions> self(this);
    const std::shared_ptr<journal::Journal> journal = m_journal;
    m_scheduler.submit(tasks::Category::General, tasks::Priority::Background,
                       [self, journal](const tasks::CancelToken &) {
        journal->prune(10000, 30);
        const auto incomplete = journal->incompleteOperations();
        if (!incomplete.ok() || incomplete.value().isEmpty()) {
            return;
        }
        const QList<journal::Operation> operations = incomplete.value();
        QMetaObject::invokeMethod(qApp, [self, operations] {
            if (self) {
                emit self->interruptedOperationsFound(operations);
            }
        }, Qt::QueuedConnection);
    });
}

quint64 FileActions::revertInterrupted(const journal::Operation &operation)
{
    return start(ActionKind::Revert, {}, tasks::Category::Transfer,
                 [operation](const Context &context, ActionResult &result) {
        result.undoneDescription = operation.description;
        result.outcome = ops::Engine(context.journal).revertInterrupted(operation.id);
    });
}

quint64 FileActions::discardInterrupted(const journal::Operation &operation)
{
    return start(ActionKind::Discard, {}, tasks::Category::Transfer,
                 [operation](const Context &context, ActionResult &result) {
        result.undoneDescription = operation.description;
        ops::Engine(context.journal).discardInterrupted(operation.id);
        result.outcome.operationId = operation.id;
    });
}

void FileActions::listTrash()
{
    const QPointer<FileActions> self(this);
    m_scheduler.submit(tasks::Category::Listing, tasks::Priority::UserInteraction,
                       [self](const tasks::CancelToken &) {
        QList<trash::TrashItem> items;
        const QStringList directories = trash::existingTrashDirectories();
        for (const QString &directory : directories) {
            const auto listed = trash::listTrash(directory);
            if (listed.ok()) {
                items.append(listed.value());
            }
        }
        std::sort(items.begin(), items.end(), [](const trash::TrashItem &a, const trash::TrashItem &b) {
            return a.deletedMs > b.deletedMs;
        });
        QMetaObject::invokeMethod(qApp, [self, items] {
            if (self) {
                emit self->trashListed(items);
            }
        }, Qt::QueuedConnection);
    });
}

quint64 FileActions::restoreFromTrash(const QList<trash::TrashItem> &items)
{
    return start(ActionKind::RestoreTrash, {}, tasks::Category::General,
                 [items](const Context &context, ActionResult &result) {
        result.outcome = ops::Engine(context.journal).restoreFromTrash(items, context.cancel);
    });
}

quint64 FileActions::purgeFromTrash(const QList<trash::TrashItem> &items)
{
    return start(ActionKind::PurgeTrash, {}, tasks::Category::Transfer,
                 [items](const Context &context, ActionResult &result) {
        result.outcome = ops::Engine(context.journal).purgeFromTrash(items, context.cancel);
    });
}

} // namespace eu::ui
