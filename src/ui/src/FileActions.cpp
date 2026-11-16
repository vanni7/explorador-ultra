// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/FileActions.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>

#include <algorithm>
#include <atomic>
#include <chrono>

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

FileActions::FileActions(tasks::Scheduler &scheduler, std::shared_ptr<journal::Journal> journal, QObject *parent)
    : QObject(parent), m_scheduler(scheduler), m_journal(std::move(journal))
{
}

FileActions::~FileActions()
{
    // Las tareas no tocan este objeto, así que no hace falta esperarlas: basta
    // pedirles que paren. El planificador las espera al destruirse.
    for (tasks::TaskHandle &task : m_running) {
        task.cancel();
    }
}

void FileActions::cancelAll()
{
    for (tasks::TaskHandle &task : m_running) {
        task.cancel();
    }
}

quint64 FileActions::start(ActionKind kind, const QString &directory, tasks::Category category, Work work,
                           const QStringList &devicePaths)
{
    const quint64 id = ++m_lastId;
    ActionResult initial;
    initial.id = id;
    initial.kind = kind;
    initial.directory = directory;

    if (!m_journal) {
        initial.outcome.errors.append(journalUnavailable());
        // Se avisa en la siguiente vuelta del bucle de eventos, igual que una
        // acción real: quien llama siempre recibe finished() después de volver.
        QMetaObject::invokeMethod(this, [this, initial] { emit finished(initial); }, Qt::QueuedConnection);
        return id;
    }

    const QPointer<FileActions> self(this);
    const std::shared_ptr<journal::Journal> journal = m_journal;
    // Copiar y mover esperan su turno detrás de lo que el usuario acaba de pedir.
    const tasks::Priority priority = (kind == ActionKind::Copy || kind == ActionKind::Move)
        ? tasks::Priority::ActiveTransfer
        : tasks::Priority::UserInteraction;

    const tasks::TaskHandle handle = m_scheduler.submitForPaths(
        category, priority, devicePaths, [self, journal, initial, work = std::move(work)](const tasks::CancelToken &cancel) {
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
                        emit self->progressed(id, progress);
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
    emit started(id, kind);
    emit runningCountChanged(runningCount());
    return id;
}

void FileActions::onFinished(const ActionResult &result)
{
    m_running.remove(result.id);
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
    return start(ActionKind::Copy, destinationDirectory, tasks::Category::Transfer,
                 [sources, destinationDirectory, policy](const Context &context, ActionResult &result) {
        result.outcome = ops::Engine(context.journal)
                             .copy(sources, destinationDirectory, policy, context.cancel, context.progress);
    }, QStringList(sources) << destinationDirectory);
}

quint64 FileActions::move(const QStringList &sources, const QString &destinationDirectory,
                          const ops::TransferOptions &policy)
{
    return start(ActionKind::Move, destinationDirectory, tasks::Category::Transfer,
                 [sources, destinationDirectory, policy](const Context &context, ActionResult &result) {
        result.outcome = ops::Engine(context.journal)
                             .move(sources, destinationDirectory, policy, context.cancel, context.progress);
    }, QStringList(sources) << destinationDirectory);
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
