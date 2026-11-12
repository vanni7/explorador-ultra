// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ops/Engine.h"

#include "eu/core/FileName.h"
#include "eu/fs/DirectoryLister.h"
#include "eu/fs/Volumes.h"
#include "eu/ops/FileOps.h"
#include "eu/trash/Trash.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

namespace eu::ops {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::ops::Engine", text);
}

/// Reglas de nombres del volumen donde está `directory`. Un disco NTFS o exFAT
/// montado en Linux exige las reglas de Windows aunque Linux no las imponga:
/// si no, el archivo sería ilegible al conectar el disco a un equipo Windows.
core::NameRules nameRulesFor(const QString &directory)
{
    const QString fileSystem = fs::volumeForPath(directory).fileSystem.toLower();
    static const QStringList windowsLike{QStringLiteral("ntfs"), QStringLiteral("ntfs3"), QStringLiteral("fuseblk"),
                                         QStringLiteral("exfat"), QStringLiteral("vfat"), QStringLiteral("fat"),
                                         QStringLiteral("fat32"), QStringLiteral("msdos")};
    if (windowsLike.contains(fileSystem)) {
        return core::NameRules::Windows;
    }
    return core::nativeNameRules();
}

/// true si `inner` es `outer` o está dentro de `outer`.
bool isSameOrInside(const QString &inner, const QString &outer)
{
    const QString a = QDir::cleanPath(inner);
    const QString b = QDir::cleanPath(outer);
    return a == b || a.startsWith(b.endsWith(QLatin1Char('/')) ? b : b + QLatin1Char('/'));
}

QString joinPath(const QString &directory, const QString &name)
{
    return directory.endsWith(QLatin1Char('/')) ? directory + name : directory + QLatin1Char('/') + name;
}

/// Suma tamaños y cuenta elementos de un árbol, para poder mostrar el avance.
void measureTree(const QString &path, const tasks::CancelToken &cancel, Progress &totals)
{
    if (cancel.isCancelled()) {
        return;
    }
    const FileStamp stamp = stampOf(path);
    if (!stamp.exists) {
        return;
    }
    ++totals.itemsTotal;
    if (stamp.isDirectory) {
        const auto entries = fs::listDirectoryAll(path);
        if (entries.ok()) {
            for (const fs::Entry &entry : entries.value()) {
                if (entry.isDirectory && !entry.isSymlink) {
                    measureTree(joinPath(path, entry.name), cancel, totals);
                } else {
                    ++totals.itemsTotal;
                    totals.bytesTotal += entry.isSymlink ? 0 : entry.size;
                }
            }
        }
    } else if (stamp.size > 0) {
        totals.bytesTotal += stamp.size;
    }
}

} // namespace

/// Una operación en curso: lleva el historial, el resultado y el avance.
/// Cada cambio en el disco pasa por step(), que lo anota antes de hacerlo.
class Engine::Run
{
public:
    Run(journal::Journal &journal, const tasks::CancelToken &cancel, const ProgressCallback &progressCallback)
        : m_journal(journal), m_cancel(cancel), m_progressCallback(progressCallback)
    {
    }

    Outcome outcome;
    Progress progress;

    bool begin(journal::OperationKind kind, const QString &description)
    {
        const auto id = m_journal.beginOperation(kind, description);
        if (!id.ok()) {
            // Sin historial no se hace nada: la regla es anotar antes de actuar.
            outcome.errors.append(id.error());
            return false;
        }
        outcome.operationId = id.value();
        return true;
    }

    void finish()
    {
        if (outcome.operationId == 0) {
            return;
        }
        journal::OperationState state = journal::OperationState::Completed;
        if (outcome.cancelled) {
            state = journal::OperationState::Cancelled;
        } else if (!outcome.errors.isEmpty()) {
            state = journal::OperationState::Failed;
        }
        m_journal.finishOperation(outcome.operationId, state);
    }

    bool cancelled()
    {
        if (m_cancel.isCancelled()) {
            outcome.cancelled = true;
        }
        return outcome.cancelled;
    }

    const tasks::CancelToken &cancelToken() const { return m_cancel; }

    void fail(const core::Error &error)
    {
        if (error.code == core::ErrorCode::Cancelled) {
            outcome.cancelled = true;
        } else {
            outcome.errors.append(error);
        }
    }

    void report(const QString &currentPath)
    {
        if (m_progressCallback) {
            progress.currentPath = currentPath;
            m_progressCallback(progress);
        }
    }

    /// Anota el paso, lo ejecuta y anota el resultado. `stampPath` es la ruta
    /// cuya huella se guarda para poder comprobarla al deshacer.
    bool step(journal::StepKind kind, const QString &source, const QString &target, const QString &stampPath,
              const std::function<core::Status()> &action)
    {
        const auto stepId = m_journal.addStep(outcome.operationId, kind, source, target);
        if (!stepId.ok()) {
            fail(stepId.error());
            return false;
        }
        const core::Status status = action();
        if (!status.ok()) {
            m_journal.setStepState(stepId.value(), journal::StepState::Failed);
            fail(status.error());
            return false;
        }
        const FileStamp stamp = stampPath.isEmpty() ? FileStamp() : stampOf(stampPath);
        m_journal.completeStep(stepId.value(), stamp.size, stamp.modifiedMs);
        return true;
    }

    /// Copia un archivo, una carpeta entera o un enlace. Devuelve false si
    /// algo de ese árbol no se pudo copiar.
    bool copyTree(const QString &source, const QString &target)
    {
        if (cancelled()) {
            return false;
        }
        const FileStamp stamp = stampOf(source);
        if (!stamp.exists) {
            fail(core::errorFromErrno(ENOENT, source));
            return false;
        }
        report(source);

        if (stamp.isSymlink) {
            const bool ok = step(journal::StepKind::CopyLink, source, target, {},
                                 [&] { return copySymlink(source, target); });
            ++progress.itemsDone;
            return ok;
        }
        if (!stamp.isDirectory) {
            const bool ok = step(journal::StepKind::CopyFile, source, target, target, [&] {
                return copyFile(source, target, m_cancel, [&](qint64 bytes) {
                    progress.bytesDone += bytes;
                    report(source);
                });
            });
            ++progress.itemsDone;
            return ok;
        }

        if (!step(journal::StepKind::MakeDirectory, source, target, {}, [&] { return makeDirectory(target); })) {
            return false;
        }
        ++progress.itemsDone;
        const auto entries = fs::listDirectoryAll(source);
        if (!entries.ok()) {
            fail(entries.error());
            return false;
        }
        bool allCopied = true;
        for (const fs::Entry &entry : entries.value()) {
            if (!copyTree(joinPath(source, entry.name), joinPath(target, entry.name))) {
                allCopied = false;
                if (outcome.cancelled) {
                    break;
                }
            }
        }
        return allCopied;
    }

    /// Borra el origen de un movimiento entre unidades, ya copiado. Cada
    /// archivo se borra solo si su copia existe y mide lo mismo.
    bool removeMovedTree(const QString &source, const QString &target)
    {
        const FileStamp stamp = stampOf(source);
        if (!stamp.exists) {
            return true;
        }
        if (stamp.isDirectory) {
            const auto entries = fs::listDirectoryAll(source);
            if (!entries.ok()) {
                fail(entries.error());
                return false;
            }
            for (const fs::Entry &entry : entries.value()) {
                if (!removeMovedTree(joinPath(source, entry.name), joinPath(target, entry.name))) {
                    return false;
                }
            }
            return step(journal::StepKind::RemoveDirectory, source, target, {},
                        [&] { return removeEmptyDirectory(source); });
        }
        const FileStamp copied = stampOf(target);
        const bool copyLooksRight = copied.exists
            && (stamp.isSymlink ? copied.isSymlink : (!copied.isDirectory && copied.size == stamp.size));
        if (!copyLooksRight) {
            fail(core::Error{core::ErrorCode::IoError,
                             tr("La copia no coincide con el original; el original no se borra."), source, 0});
            return false;
        }
        return step(journal::StepKind::RemoveFile, source, target, {}, [&] { return removeFile(source); });
    }

private:
    journal::Journal &m_journal;
    const tasks::CancelToken &m_cancel;
    const ProgressCallback &m_progressCallback;
};

Engine::Engine(std::shared_ptr<journal::Journal> journal)
    : m_journal(std::move(journal))
{
}

Outcome Engine::createFolder(const QString &parentDirectory, const QString &name)
{
    const tasks::CancelToken neverCancelled;
    Run run(*m_journal, neverCancelled, {});
    const core::NameCheck check = core::checkFileName(name, nameRulesFor(parentDirectory));
    if (!check.ok()) {
        run.outcome.errors.append({core::ErrorCode::InvalidName, core::describeNameProblem(check), name, 0});
        return run.outcome;
    }
    const QString target = joinPath(parentDirectory, name);
    if (run.begin(journal::OperationKind::Create, tr("Crear la carpeta «%1»").arg(name))) {
        if (run.step(journal::StepKind::MakeDirectory, {}, target, {}, [&] { return makeDirectory(target); })) {
            run.outcome.resultPaths.append(target);
        }
        run.finish();
    }
    return run.outcome;
}

Outcome Engine::createFile(const QString &parentDirectory, const QString &name)
{
    const tasks::CancelToken neverCancelled;
    Run run(*m_journal, neverCancelled, {});
    const core::NameCheck check = core::checkFileName(name, nameRulesFor(parentDirectory));
    if (!check.ok()) {
        run.outcome.errors.append({core::ErrorCode::InvalidName, core::describeNameProblem(check), name, 0});
        return run.outcome;
    }
    const QString target = joinPath(parentDirectory, name);
    if (run.begin(journal::OperationKind::Create, tr("Crear el archivo «%1»").arg(name))) {
        if (run.step(journal::StepKind::CreateFile, {}, target, target, [&] { return createEmptyFile(target); })) {
            run.outcome.resultPaths.append(target);
        }
        run.finish();
    }
    return run.outcome;
}

Outcome Engine::rename(const QString &path, const QString &newName)
{
    const tasks::CancelToken neverCancelled;
    Run run(*m_journal, neverCancelled, {});
    const QString parent = QFileInfo(path).absolutePath();
    const core::NameCheck check = core::checkFileName(newName, nameRulesFor(parent));
    if (!check.ok()) {
        run.outcome.errors.append({core::ErrorCode::InvalidName, core::describeNameProblem(check), newName, 0});
        return run.outcome;
    }
    const QString target = joinPath(parent, newName);
    if (QDir::cleanPath(path) == target) {
        return run.outcome;   // mismo nombre: no hay nada que hacer
    }
    if (run.begin(journal::OperationKind::Rename,
                  tr("Renombrar «%1» a «%2»").arg(QFileInfo(path).fileName(), newName))) {
        if (run.step(journal::StepKind::Rename, path, target, target, [&] { return renameNoReplace(path, target); })) {
            run.outcome.resultPaths.append(target);
        }
        run.finish();
    }
    return run.outcome;
}

Outcome Engine::copy(const QStringList &sources, const QString &destinationDirectory, CollisionPolicy policy,
                     const tasks::CancelToken &cancel, const ProgressCallback &progress)
{
    Run run(*m_journal, cancel, progress);
    if (!stampOf(destinationDirectory).isDirectory) {
        run.outcome.errors.append(core::errorFromErrno(ENOENT, destinationDirectory));
        return run.outcome;
    }
    for (const QString &source : sources) {
        measureTree(source, cancel, run.progress);
    }
    if (!run.begin(journal::OperationKind::Copy,
                   tr("Copiar %1 elemento(s) a «%2»").arg(sources.size()).arg(QDir(destinationDirectory).dirName()))) {
        return run.outcome;
    }

    for (const QString &source : sources) {
        if (run.cancelled()) {
            break;
        }
        const QString name = QFileInfo(source).fileName();
        if (stampOf(source).isDirectory && isSameOrInside(destinationDirectory, source)) {
            run.fail({core::ErrorCode::NotSupported,
                      tr("No se puede copiar una carpeta dentro de sí misma."), source, 0});
            continue;
        }
        QString targetName = name;
        if (stampOf(joinPath(destinationDirectory, name)).exists) {
            // Copiar en la misma carpeta siempre crea un duplicado con otro nombre.
            const bool sameFolder = QDir::cleanPath(QFileInfo(source).absolutePath())
                == QDir::cleanPath(destinationDirectory);
            if (policy == CollisionPolicy::Skip && !sameFolder) {
                run.outcome.skipped.append(source);
                continue;
            }
            targetName = uniqueName(destinationDirectory, name);
        }
        const QString target = joinPath(destinationDirectory, targetName);
        if (run.copyTree(source, target)) {
            run.outcome.resultPaths.append(target);
        }
    }
    run.finish();
    return run.outcome;
}

Outcome Engine::move(const QStringList &sources, const QString &destinationDirectory, CollisionPolicy policy,
                     const tasks::CancelToken &cancel, const ProgressCallback &progress)
{
    Run run(*m_journal, cancel, progress);
    if (!stampOf(destinationDirectory).isDirectory) {
        run.outcome.errors.append(core::errorFromErrno(ENOENT, destinationDirectory));
        return run.outcome;
    }
    for (const QString &source : sources) {
        measureTree(source, cancel, run.progress);
    }
    if (!run.begin(journal::OperationKind::Move,
                   tr("Mover %1 elemento(s) a «%2»").arg(sources.size()).arg(QDir(destinationDirectory).dirName()))) {
        return run.outcome;
    }

    for (const QString &source : sources) {
        if (run.cancelled()) {
            break;
        }
        const QString name = QFileInfo(source).fileName();
        const QString sourceParent = QDir::cleanPath(QFileInfo(source).absolutePath());
        if (sourceParent == QDir::cleanPath(destinationDirectory)) {
            continue;   // ya está ahí
        }
        if (stampOf(source).isDirectory && isSameOrInside(destinationDirectory, source)) {
            run.fail({core::ErrorCode::NotSupported,
                      tr("No se puede mover una carpeta dentro de sí misma."), source, 0});
            continue;
        }
        QString targetName = name;
        if (stampOf(joinPath(destinationDirectory, name)).exists) {
            if (policy == CollisionPolicy::Skip) {
                run.outcome.skipped.append(source);
                continue;
            }
            targetName = uniqueName(destinationDirectory, name);
        }
        const QString target = joinPath(destinationDirectory, targetName);
        run.report(source);

        // Dentro del mismo volumen mover es renombrar: instantáneo y atómico.
        if (fs::isSameVolume(sourceParent, destinationDirectory)) {
            const auto stepId = m_journal->addStep(run.outcome.operationId, journal::StepKind::Rename, source, target);
            if (!stepId.ok()) {
                run.fail(stepId.error());
                break;
            }
            const core::Status renamed = renameNoReplace(source, target);
            if (renamed.ok()) {
                const FileStamp stamp = stampOf(target);
                m_journal->completeStep(stepId.value(), stamp.size, stamp.modifiedMs);
                run.outcome.resultPaths.append(target);
                continue;
            }
            m_journal->setStepState(stepId.value(), journal::StepState::Failed);
            if (renamed.error().code != core::ErrorCode::CrossDevice) {
                run.fail(renamed.error());
                continue;
            }
            // El sistema dice que son volúmenes distintos: se sigue por la vía larga.
        }

        // Entre volúmenes: copiar, sincronizar a disco y solo entonces borrar
        // el origen (SPEC §8.3). Si la copia falla, el origen queda intacto.
        if (run.copyTree(source, target) && !run.cancelled()) {
            if (run.removeMovedTree(source, target)) {
                run.outcome.resultPaths.append(target);
            }
        }
    }
    run.finish();
    return run.outcome;
}

Outcome Engine::trash(const QStringList &paths, const tasks::CancelToken &cancel)
{
    Run run(*m_journal, cancel, {});
    if (!run.begin(journal::OperationKind::Trash, tr("Enviar %1 elemento(s) a la papelera").arg(paths.size()))) {
        return run.outcome;
    }
    for (const QString &path : paths) {
        if (run.cancelled()) {
            break;
        }
        const core::Result<trash::TrashItem> item = trash::prepareItem(path);
        if (!item.ok()) {
            run.fail(item.error());
            continue;
        }
        const QString stored = item.value().storedPath();
        if (run.step(journal::StepKind::Rename, item.value().originalPath, stored, stored,
                     [&] { return trash::moveIntoTrash(item.value()); })) {
            run.outcome.resultPaths.append(stored);
        } else {
            trash::discardItemRecord(item.value());
        }
    }
    run.finish();
    return run.outcome;
}

Outcome Engine::deletePermanently(const QStringList &paths, const tasks::CancelToken &cancel)
{
    Run run(*m_journal, cancel, {});
    if (!run.begin(journal::OperationKind::Delete,
                   tr("Eliminar permanentemente %1 elemento(s)").arg(paths.size()))) {
        return run.outcome;
    }
    for (const QString &path : paths) {
        if (run.cancelled()) {
            break;
        }
        const FileStamp stamp = stampOf(path);
        const journal::StepKind kind = stamp.isDirectory ? journal::StepKind::RemoveDirectory
                                                         : journal::StepKind::RemoveFile;
        run.step(kind, path, {}, {}, [&] { return trash::removeTreePermanently(path, cancel); });
    }
    run.finish();
    return run.outcome;
}

bool Engine::canUndo(const journal::Operation &operation)
{
    if (operation.kind == journal::OperationKind::Delete) {
        return false;
    }
    return operation.state == journal::OperationState::Completed
        || operation.state == journal::OperationState::Failed
        || operation.state == journal::OperationState::Cancelled;
}

Outcome Engine::undo(journal::OperationId operationId, const tasks::CancelToken &cancel)
{
    Outcome outcome;
    outcome.operationId = operationId;

    const auto operation = m_journal->operation(operationId);
    const auto steps = m_journal->steps(operationId);
    if (!operation.ok() || !steps.ok()) {
        outcome.errors.append(operation.ok() ? steps.error() : operation.error());
        return outcome;
    }
    if (operation.value().kind == journal::OperationKind::Delete) {
        outcome.errors.append({core::ErrorCode::NotSupported,
                               tr("Una eliminación permanente no se puede deshacer."), {}, 0});
        return outcome;
    }

    const QList<journal::Step> &all = steps.value();
    // Orígenes de un movimiento entre unidades que no se pudieron devolver:
    // su copia en el destino no debe borrarse.
    QSet<QString> sourcesNotRestored;

    const auto changedOutside = [&](const QString &path) {
        outcome.errors.append({core::ErrorCode::Busy,
                               tr("Cambió desde que se hizo la operación; no se deshace."), path, 0});
    };

    for (qsizetype index = all.size() - 1; index >= 0; --index) {
        const journal::Step &step = all[index];
        if (step.state != journal::StepState::Done) {
            continue;
        }
        if (cancel.isCancelled()) {
            outcome.cancelled = true;
            break;
        }

        core::Status status;
        bool skip = false;
        switch (step.kind) {
        case journal::StepKind::MakeDirectory:
            // Si dentro hay algo que no creó la operación, la carpeta se queda.
            status = removeEmptyDirectory(step.target);
            break;

        case journal::StepKind::CreateFile: {
            const FileStamp now = stampOf(step.target);
            if (!now.exists) {
                break;   // ya no está: nada que deshacer
            }
            if (now.size != 0 || now.modifiedMs != step.resultModifiedMs) {
                changedOutside(step.target);
                skip = true;
                break;
            }
            status = removeFile(step.target);
            break;
        }

        case journal::StepKind::Rename: {
            const FileStamp now = stampOf(step.target);
            if (!now.exists) {
                changedOutside(step.target);
                skip = true;
                break;
            }
            if (step.resultSize >= 0 && !now.isDirectory
                && (now.size != step.resultSize || now.modifiedMs != step.resultModifiedMs)) {
                changedOutside(step.target);
                skip = true;
                break;
            }
            status = renameNoReplace(step.target, step.source);
            if (status.ok() && trash::isInsideTrash(step.target)) {
                // Salió de la papelera: se limpian su carpeta contenedora y su .info.
                const QString holder = QFileInfo(step.target).absolutePath();
                removeEmptyDirectory(holder);
                QFile::remove(holder + QStringLiteral(".info"));
            }
            if (status.ok()) {
                outcome.resultPaths.append(step.source);
            }
            break;
        }

        case journal::StepKind::CopyFile: {
            if (sourcesNotRestored.contains(step.source)) {
                skip = true;   // es la única copia que queda: no se borra
                break;
            }
            const FileStamp now = stampOf(step.target);
            if (!now.exists) {
                break;
            }
            if (now.size != step.resultSize || now.modifiedMs != step.resultModifiedMs) {
                changedOutside(step.target);
                skip = true;
                break;
            }
            status = removeFile(step.target);
            break;
        }

        case journal::StepKind::CopyLink:
            if (sourcesNotRestored.contains(step.source)) {
                skip = true;
                break;
            }
            if (stampOf(step.target).isSymlink) {
                status = removeFile(step.target);
            }
            break;

        case journal::StepKind::RemoveFile: {
            // Movimiento entre unidades: el origen se borró; se recupera
            // copiándolo de vuelta desde el destino.
            if (stampOf(step.source).exists) {
                break;
            }
            const FileStamp copy = stampOf(step.target);
            status = copy.isSymlink ? copySymlink(step.target, step.source)
                                    : copyFile(step.target, step.source, cancel);
            if (!status.ok()) {
                sourcesNotRestored.insert(step.source);
            } else {
                outcome.resultPaths.append(step.source);
            }
            break;
        }

        case journal::StepKind::RemoveDirectory:
            if (!stampOf(step.source).exists) {
                status = makeDirectory(step.source);
            }
            break;
        }

        if (skip) {
            continue;
        }
        if (!status.ok()) {
            if (status.error().code == core::ErrorCode::Cancelled) {
                outcome.cancelled = true;
                break;
            }
            outcome.errors.append(status.error());
            continue;
        }
        m_journal->setStepState(step.id, journal::StepState::Undone);
    }

    if (outcome.ok()) {
        m_journal->finishOperation(operationId, journal::OperationState::Undone);
    }
    return outcome;
}

/// Pone al día los pasos que quedaron "pendientes" cuando la aplicación se
/// cerró: mira el disco para saber si llegaron a hacerse y limpia los restos.
static void settlePendingSteps(journal::Journal &journal, journal::OperationId operationId)
{
    const auto steps = journal.steps(operationId);
    if (!steps.ok()) {
        return;
    }
    for (const journal::Step &step : steps.value()) {
        if (step.state != journal::StepState::Pending) {
            continue;
        }
        bool happened = false;
        FileStamp result;
        switch (step.kind) {
        case journal::StepKind::CopyFile: {
            // Si se quedó a medias, lo escrito está en el archivo temporal.
            QFile::remove(step.target + partialSuffix());
            const FileStamp source = stampOf(step.source);
            result = stampOf(step.target);
            // El destino con el nombre final solo aparece al terminar; se
            // reconoce como nuestro si mide y data igual que el origen.
            happened = result.exists && source.exists && result.size == source.size
                && result.modifiedMs == source.modifiedMs;
            break;
        }
        case journal::StepKind::Rename:
            result = stampOf(step.target);
            happened = result.exists && !stampOf(step.source).exists;
            if (!happened && trash::isInsideTrash(step.target)) {
                // Envío a la papelera que no llegó a moverse: se retira su registro.
                const QString holder = QFileInfo(step.target).absolutePath();
                removeEmptyDirectory(holder);
                QFile::remove(holder + QStringLiteral(".info"));
            }
            break;
        case journal::StepKind::MakeDirectory:
            happened = stampOf(step.target).isDirectory;
            break;
        case journal::StepKind::CreateFile:
            result = stampOf(step.target);
            happened = result.exists && result.size == 0;
            break;
        case journal::StepKind::CopyLink:
            happened = stampOf(step.target).isSymlink;
            break;
        case journal::StepKind::RemoveFile:
        case journal::StepKind::RemoveDirectory:
            happened = !stampOf(step.source).exists;
            break;
        }
        if (happened) {
            journal.completeStep(step.id, result.size, result.modifiedMs);
        } else {
            journal.setStepState(step.id, journal::StepState::Failed);
        }
    }
}

Outcome Engine::revertInterrupted(journal::OperationId operationId)
{
    settlePendingSteps(*m_journal, operationId);
    // Se marca como fallida para que undo() la acepte y, si todo va bien, la
    // deje como deshecha.
    m_journal->finishOperation(operationId, journal::OperationState::Failed);
    return undo(operationId, tasks::CancelToken());
}

void Engine::discardInterrupted(journal::OperationId operationId)
{
    settlePendingSteps(*m_journal, operationId);
    m_journal->finishOperation(operationId, journal::OperationState::Failed);
}

} // namespace eu::ops
