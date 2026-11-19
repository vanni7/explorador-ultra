// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ops/Engine.h"

#include "eu/core/FileName.h"
#include "eu/fs/DirectoryLister.h"
#include "eu/fs/Volumes.h"
#include "eu/ops/FileOps.h"
#include "eu/trash/Trash.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <optional>

namespace eu::ops {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::ops::Engine", text);
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
    Run(journal::Journal &journal, const tasks::CancelToken &cancel, const ProgressCallback &progressCallback,
        TransferOptions collisions = {})
        : m_journal(journal), m_cancel(cancel), m_progressCallback(progressCallback),
          m_collisions(std::move(collisions))
    {
    }

    Outcome outcome;
    Progress progress;
    /// Se llama cuando un elemento de primer nivel ya sabe su destino.
    std::function<void(const QString &)> onTopLevelTarget;
    /// Reintento o reanudación: los destinos son los que dejó la vez anterior,
    /// así que las carpetas se completan y lo que ya llegó idéntico no se repite.
    bool retrying = false;

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
        if (outcome.cancelled && m_collisions.leaveOpenIfCancelled && m_collisions.leaveOpenIfCancelled()) {
            return;   // queda como interrumpida, para reanudarla
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
                }, m_collisions.copy);
            });
            ++progress.itemsDone;
            if (ok && m_collisions.copy.verify) {
                ++outcome.verified;
            }
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

    /// Copia o mueve `source` a `directory` con el nombre `name`, resolviendo
    /// los choques de nombre. Devuelve false si algo falló o se canceló;
    /// omitir no es un fallo.
    bool transfer(const QString &source, const QString &directory, const QString &name, bool moving, bool topLevel,
                  QString *chosenTarget = nullptr)
    {
        if (m_collisions.copy.pausePoint) {
            m_collisions.copy.pausePoint(m_cancel);
        }
        if (cancelled()) {
            if (chosenTarget != nullptr) {
                *chosenTarget = joinPath(directory, name);
            }
            return false;
        }
        const bool sameFolder = QDir::cleanPath(QFileInfo(source).absolutePath()) == QDir::cleanPath(directory);
        if (moving && sameFolder) {
            return true;   // ya está ahí
        }
        // Copiar en la misma carpeta siempre crea un duplicado con otro nombre.
        const Resolved resolved = resolve(source, directory, name, !moving && sameFolder);
        if (chosenTarget != nullptr) {
            *chosenTarget = resolved.target.isEmpty() ? joinPath(directory, name) : resolved.target;
        }
        if (topLevel && onTopLevelTarget && !resolved.target.isEmpty()) {
            onTopLevelTarget(resolved.target);   // al plan: por si hay que reanudar
        }
        switch (resolved.what) {
        case Resolution::Skip:
            return true;
        case Resolution::Failed:
            return false;
        case Resolution::Merge: {
            const bool merged = mergeInto(source, resolved.target, moving);
            if (merged && topLevel) {
                outcome.resultPaths.append(resolved.target);
            }
            return merged;
        }
        case Resolution::Replace:
            if (isSameOrInside(source, resolved.target)) {
                fail({core::ErrorCode::NotSupported,
                      tr("No se puede reemplazar una carpeta por algo que está dentro de ella."), source, 0});
                return false;
            }
            if (!clearForReplace(resolved.target)) {
                return false;
            }
            break;
        case Resolution::Write:
            break;
        }
        const bool done = moving ? moveOne(source, resolved.target) : copyTree(source, resolved.target);
        if (done && topLevel) {
            outcome.resultPaths.append(resolved.target);
        }
        return done;
    }

    /// Mueve a un destino libre (usado por applyMoves).
    bool moveTo(const QString &source, const QString &target) { return moveOne(source, target); }

private:
    enum class Resolution { Write, Skip, Merge, Replace, Failed };
    struct Resolved
    {
        Resolution what = Resolution::Write;
        QString target;
    };

    /// Decide qué hacer con `source` si en `directory` ya hay algo llamado `name`.
    Resolved resolve(const QString &source, const QString &directory, const QString &name, bool duplicateInPlace)
    {
        const QString target = joinPath(directory, name);
        const FileStamp targetStamp = stampOf(target);
        if (!targetStamp.exists) {
            return {Resolution::Write, target};
        }
        if (duplicateInPlace) {
            return {Resolution::Write, joinPath(directory, uniqueName(directory, name))};
        }
        const FileStamp sourceStamp = stampOf(source);
        const bool bothFolders = sourceStamp.isDirectory && targetStamp.isDirectory;
        const auto renamed = [&] { return Resolved{Resolution::Write, joinPath(directory, uniqueName(directory, name))}; };
        const auto skip = [&] {
            outcome.skipped.append(source);
            if (!sourceStamp.isDirectory && sourceStamp.size > 0) {
                progress.bytesDone += sourceStamp.size;   // para que el avance llegue al final
            }
            ++progress.itemsDone;
            return Resolved{Resolution::Skip, {}};
        };

        CollisionPolicy policy = m_forAll.value_or(m_collisions.policy);
        if (retrying && bothFolders) {
            return {Resolution::Merge, target};
        }
        if (retrying && (policy == CollisionPolicy::Overwrite || policy == CollisionPolicy::OverwriteIfNewer)) {
            const core::Result<bool> same = sameContent(source, target, m_collisions.comparison, m_cancel);
            if (same.ok() && same.value()) {
                outcome.identical.append(source);
                return skip();
            }
        }
        if (policy == CollisionPolicy::Ask) {
            if (bothFolders) {
                return {Resolution::Merge, target};   // se pregunta por cada archivo de dentro
            }
            if (!m_collisions.ask) {
                return renamed();
            }
            const CollisionDecision decision = m_collisions.ask(Conflict{source, target, sourceStamp, targetStamp}, m_cancel);
            if (decision.cancel) {
                outcome.cancelled = true;
                return {Resolution::Failed, {}};
            }
            policy = decision.policy == CollisionPolicy::Ask ? CollisionPolicy::Skip : decision.policy;
            if (decision.applyToAll) {
                m_forAll = policy;
            }
        }

        switch (policy) {
        case CollisionPolicy::Skip:
            return skip();
        case CollisionPolicy::AutoRename:
            return renamed();
        case CollisionPolicy::AppendDateTime: {
            // La fecha del original; las carpetas no la guardan: la de ahora.
            const qint64 when = sourceStamp.modifiedMs > 0 ? sourceStamp.modifiedMs
                                                           : QDateTime::currentMSecsSinceEpoch();
            const QString dated = nameWithDateTime(name, when);
            return {Resolution::Write, joinPath(directory, uniqueName(directory, dated))};
        }
        case CollisionPolicy::Overwrite:
            return {bothFolders ? Resolution::Merge : Resolution::Replace, target};
        case CollisionPolicy::OverwriteIfNewer:
            if (bothFolders) {
                return {Resolution::Merge, target};
            }
            return sourceStamp.modifiedMs > targetStamp.modifiedMs ? Resolved{Resolution::Replace, target} : skip();
        case CollisionPolicy::SkipIfIdentical: {
            if (bothFolders) {
                return {Resolution::Merge, target};
            }
            const core::Result<bool> same = sameContent(source, target, m_collisions.comparison, m_cancel);
            if (!same.ok()) {
                fail(same.error());
                return {Resolution::Failed, {}};
            }
            if (same.value()) {
                outcome.identical.append(source);
                return skip();
            }
            // También cuenta una copia anterior con número ("foto (2).jpg"):
            // copiar dos veces lo mismo no debe ir dejando duplicados.
            if (!sourceStamp.isDirectory) {
                const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
                const QString stem = dot > 0 ? name.left(dot) : name;
                const QString suffix = dot > 0 ? name.mid(dot) : QString();
                for (int number = 2; number < 1000; ++number) {
                    const QString candidate = joinPath(directory, QStringLiteral("%1 (%2)%3").arg(stem).arg(number).arg(suffix));
                    const FileStamp existing = stampOf(candidate);
                    if (!existing.exists) {
                        break;
                    }
                    if (existing.size != sourceStamp.size) {
                        continue;
                    }
                    const core::Result<bool> numbered = sameContent(source, candidate, m_collisions.comparison, m_cancel);
                    if (numbered.ok() && numbered.value()) {
                        outcome.identical.append(source);
                        return skip();
                    }
                }
            }
            return renamed();
        }
        case CollisionPolicy::Ask:
            break;
        }
        return skip();
    }

    /// Manda a la papelera propia lo que hay en `target` para dejar sitio.
    /// Queda anotado: deshacer la operación lo devuelve a su sitio.
    bool clearForReplace(const QString &target)
    {
        const core::Result<trash::TrashItem> item = trash::prepareItem(target);
        if (!item.ok()) {
            fail({item.error().code,
                  tr("No se reemplaza: se perdería lo que hay, porque no se puede enviar a la papelera (%1).")
                      .arg(item.error().message),
                  target, 0});
            return false;
        }
        const QString stored = item.value().storedPath();
        if (!step(journal::StepKind::Rename, target, stored, stored,
                  [&] { return trash::moveIntoTrash(item.value()); })) {
            trash::discardItemRecord(item.value());
            return false;
        }
        outcome.replaced.append(target);
        return true;
    }

    /// Junta la carpeta `source` con la carpeta `target`, que ya existe:
    /// cada elemento de dentro resuelve su propio choque.
    bool mergeInto(const QString &source, const QString &target, bool moving)
    {
        const auto entries = fs::listDirectoryAll(source);
        if (!entries.ok()) {
            fail(entries.error());
            return false;
        }
        ++progress.itemsDone;
        bool all = true;
        for (const fs::Entry &entry : entries.value()) {
            if (!transfer(joinPath(source, entry.name), target, entry.name, moving, false)) {
                all = false;
                if (outcome.cancelled) {
                    return false;
                }
            }
        }
        // Al mover, la carpeta de origen se quita si quedó vacía (lo omitido sigue en ella).
        if (moving && all) {
            const auto left = fs::listDirectoryAll(source);
            if (left.ok() && left.value().empty()) {
                return step(journal::StepKind::RemoveDirectory, source, target, {},
                            [&] { return removeEmptyDirectory(source); });
            }
        }
        return all;
    }

    /// Mueve `source` a `target` (libre). En el mismo volumen, renombrando;
    /// entre volúmenes, copiando, sincronizando y solo entonces borrando (SPEC §8.3).
    bool moveOne(const QString &source, const QString &target)
    {
        report(source);
        if (fs::isSameVolume(QFileInfo(source).absolutePath(), QFileInfo(target).absolutePath())) {
            const auto stepId = m_journal.addStep(outcome.operationId, journal::StepKind::Rename, source, target);
            if (!stepId.ok()) {
                fail(stepId.error());
                return false;
            }
            const core::Status renamed = renameNoReplace(source, target);
            if (renamed.ok()) {
                const FileStamp stamp = stampOf(target);
                m_journal.completeStep(stepId.value(), stamp.size, stamp.modifiedMs);
                return true;
            }
            m_journal.setStepState(stepId.value(), journal::StepState::Failed);
            if (renamed.error().code != core::ErrorCode::CrossDevice) {
                fail(renamed.error());
                return false;
            }
            // El sistema dice que son volúmenes distintos: se sigue por la vía larga.
        }
        if (!copyTree(source, target) || cancelled()) {
            return false;
        }
        return removeMovedTree(source, target);
    }

    journal::Journal &m_journal;
    const tasks::CancelToken &m_cancel;
    const ProgressCallback &m_progressCallback;
    TransferOptions m_collisions;
    std::optional<CollisionPolicy> m_forAll;   ///< "Aplicar a todos" de una pregunta anterior.
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

Outcome Engine::renameMany(const QList<QPair<QString, QString>> &items, const tasks::CancelToken &cancel)
{
    Run run(*m_journal, cancel, {});
    if (items.isEmpty()) {
        return run.outcome;
    }
    if (!run.begin(journal::OperationKind::Rename, tr("Renombrar %1 elemento(s)").arg(items.size()))) {
        return run.outcome;
    }
    // Lo más profundo primero: renombrar una carpeta cambia la ruta de lo de dentro.
    QList<QPair<QString, QString>> ordered = items;
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
        return QDir::cleanPath(a.first).count(QLatin1Char('/')) > QDir::cleanPath(b.first).count(QLatin1Char('/'));
    });
    // Por carpetas. Si algún nombre nuevo es el viejo de otro (cadena o ciclo)
    // o solo cambian mayúsculas, todo el grupo pasa antes por nombres temporales.
    QList<QPair<QString, QList<QPair<QString, QString>>>> groups;
    for (const auto &item : ordered) {
        const QString parent = QFileInfo(QDir::cleanPath(item.first)).absolutePath();
        auto found = std::find_if(groups.begin(), groups.end(), [&](const auto &group) { return group.first == parent; });
        if (found == groups.end()) {
            groups.append({parent, {}});
            found = groups.end() - 1;
        }
        found->second.append({QDir::cleanPath(item.first), item.second});
    }
    int temporary = 0;
    for (const auto &[parent, group] : groups) {
        const core::NameRules rules = nameRulesFor(parent);
        QSet<QString> oldNames;
        for (const auto &item : group) {
            oldNames.insert(QFileInfo(item.first).fileName().toCaseFolded());
        }
        bool needsTemporary = false;
        for (const auto &item : group) {
            needsTemporary = needsTemporary || oldNames.contains(item.second.toCaseFolded());
        }
        QList<QPair<QString, QString>> pending;   // (ruta actual, nombre final)
        for (const auto &item : group) {
            if (run.cancelled()) {
                break;
            }
            const core::NameCheck check = core::checkFileName(item.second, rules);
            if (!check.ok()) {
                run.fail({core::ErrorCode::InvalidName, core::describeNameProblem(check), item.second, 0});
                continue;
            }
            if (QFileInfo(item.first).fileName() == item.second) {
                continue;   // mismo nombre: nada que hacer
            }
            if (!needsTemporary) {
                pending.append(item);
                continue;
            }
            const QString parked = joinPath(parent, QStringLiteral(".eu-renombrando-%1-%2")
                                                        .arg(QCoreApplication::applicationPid())
                                                        .arg(++temporary));
            if (run.step(journal::StepKind::Rename, item.first, parked, parked,
                         [&] { return renameNoReplace(item.first, parked); })) {
                pending.append({parked, item.second});
            }
        }
        for (const auto &item : pending) {
            const QString target = joinPath(parent, item.second);
            if (run.step(journal::StepKind::Rename, item.first, target, target,
                         [&] { return renameNoReplace(item.first, target); })) {
                run.outcome.resultPaths.append(target);
            } else if (needsTemporary) {
                // No se pudo dar el nombre final (apareció algo con ese nombre):
                // vuelve a como estaba, para no dejarlo con el nombre temporal.
                const auto original = std::find_if(group.begin(), group.end(), [&](const auto &candidate) {
                    return candidate.second == item.second;
                });
                if (original != group.end()) {
                    run.step(journal::StepKind::Rename, item.first, original->first, original->first,
                             [&] { return renameNoReplace(item.first, original->first); });
                }
            }
        }
    }
    run.finish();
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

Outcome Engine::copy(const QStringList &sources, const QString &destinationDirectory,
                     const TransferOptions &collisions, const tasks::CancelToken &cancel,
                     const ProgressCallback &progress)
{
    return transferAll(sources, destinationDirectory, collisions, cancel, progress, false);
}

Outcome Engine::move(const QStringList &sources, const QString &destinationDirectory,
                     const TransferOptions &collisions, const tasks::CancelToken &cancel,
                     const ProgressCallback &progress)
{
    return transferAll(sources, destinationDirectory, collisions, cancel, progress, true);
}

Outcome Engine::transferAll(const QStringList &sources, const QString &destinationDirectory,
                            const TransferOptions &collisions, const tasks::CancelToken &cancel,
                            const ProgressCallback &progress, bool moving)
{
    Run run(*m_journal, cancel, progress, collisions);
    if (!stampOf(destinationDirectory).isDirectory) {
        run.outcome.errors.append(core::errorFromErrno(ENOENT, destinationDirectory));
        return run.outcome;
    }
    for (const QString &source : sources) {
        measureTree(source, cancel, run.progress);
    }
    const QString folderName = QDir(destinationDirectory).dirName();
    const QString description = moving ? tr("Mover %1 elemento(s) a «%2»").arg(sources.size()).arg(folderName)
                                       : tr("Copiar %1 elemento(s) a «%2»").arg(sources.size()).arg(folderName);
    if (!run.begin(moving ? journal::OperationKind::Move : journal::OperationKind::Copy, description)) {
        return run.outcome;
    }
    // El plan, para poder reanudar tras un cierre inesperado (SPEC §9).
    QList<QPair<QString, QString>> plan;
    for (const QString &source : sources) {
        plan.append({source, joinPath(destinationDirectory, QFileInfo(source).fileName())});
    }
    m_journal->setPlan(run.outcome.operationId, plan);
    int index = 0;
    for (const QString &source : sources) {
        const int position = index++;
        run.onTopLevelTarget = [&, position](const QString &target) {
            if (target != plan[position].second) {
                m_journal->setPlanTarget(run.outcome.operationId, position, target);
            }
        };
        if (run.cancelled()) {
            // Lo que ni se empezó por cancelar también queda pendiente.
            run.outcome.unfinished.append({source, joinPath(destinationDirectory, QFileInfo(source).fileName())});
            continue;
        }
        if (stampOf(source).isDirectory && isSameOrInside(destinationDirectory, source)) {
            run.fail({core::ErrorCode::NotSupported,
                      moving ? tr("No se puede mover una carpeta dentro de sí misma.")
                             : tr("No se puede copiar una carpeta dentro de sí misma."),
                      source, 0});
            continue;
        }
        QString target;
        if (!run.transfer(source, destinationDirectory, QFileInfo(source).fileName(), moving, true, &target)) {
            run.outcome.unfinished.append({source, target});
        }
    }
    run.finish();
    return run.outcome;
}

Outcome Engine::retry(const QList<QPair<QString, QString>> &items, bool moving, const TransferOptions &options,
                      const tasks::CancelToken &cancel, const ProgressCallback &progress)
{
    // Lo que ya está en el destino se compara: idéntico, se deja; a medias
    // (una carpeta), se completa. Lo demás, como se pidió la primera vez.
    TransferOptions again = options;
    if (again.policy == CollisionPolicy::AutoRename || again.policy == CollisionPolicy::AppendDateTime
        || again.policy == CollisionPolicy::Ask) {
        again.policy = CollisionPolicy::SkipIfIdentical;
    }
    Run run(*m_journal, cancel, progress, again);
    run.retrying = true;
    for (const auto &item : items) {
        measureTree(item.first, cancel, run.progress);
    }
    const QString description = moving ? tr("Reintentar: mover %1 elemento(s)").arg(items.size())
                                       : tr("Reintentar: copiar %1 elemento(s)").arg(items.size());
    if (!run.begin(moving ? journal::OperationKind::Move : journal::OperationKind::Copy, description)) {
        return run.outcome;
    }
    m_journal->setPlan(run.outcome.operationId, items);
    for (const auto &item : items) {
        if (run.cancelled()) {
            run.outcome.unfinished.append(item);
            continue;
        }
        // Al mover, lo que ya llegó entero dejó de estar en el origen.
        if (moving && !stampOf(item.first).exists && stampOf(item.second).exists) {
            continue;
        }
        const QFileInfo target(item.second);
        if (!run.transfer(item.first, target.absolutePath(), target.fileName(), moving, true)) {
            run.outcome.unfinished.append(item);
        }
    }
    run.finish();
    return run.outcome;
}

Outcome Engine::applyMoves(const QList<QPair<QString, QString>> &moves, const QStringList &foldersToCreate,
                           const QString &description, const tasks::CancelToken &cancel,
                           const ProgressCallback &progress)
{
    Run run(*m_journal, cancel, progress);
    for (const auto &move : moves) {
        measureTree(move.first, cancel, run.progress);
    }
    if (!run.begin(journal::OperationKind::Move, description)) {
        return run.outcome;
    }
    QList<QPair<QString, QString>> plan;
    for (const auto &move : moves) {
        plan.append(move);
    }
    m_journal->setPlan(run.outcome.operationId, plan);
    // Primero las carpetas (de fuera hacia dentro): deshacer las quita si quedan vacías.
    for (const QString &folder : foldersToCreate) {
        if (run.cancelled()) {
            break;
        }
        if (!stampOf(folder).exists) {
            run.step(journal::StepKind::MakeDirectory, {}, folder, {}, [&] { return makeDirectory(folder); });
        }
    }
    for (const auto &[source, wanted] : moves) {
        if (run.cancelled()) {
            run.outcome.unfinished.append({source, wanted});
            continue;
        }
        // Si entre la simulación y ahora apareció algo con ese nombre, otro nombre.
        const QFileInfo target(wanted);
        const QString free = joinPath(target.absolutePath(), uniqueName(target.absolutePath(), target.fileName()));
        if (run.moveTo(source, free)) {
            run.outcome.resultPaths.append(free);
            ++run.progress.itemsDone;
        } else {
            run.outcome.unfinished.append({source, free});
        }
    }
    run.finish();
    return run.outcome;
}

Outcome Engine::synchronize(const QList<QPair<QString, QString>> &copies, const QStringList &toTrash,
                            const QString &description, const CopyOptions &copyOptions,
                            const tasks::CancelToken &cancel, const ProgressCallback &progress)
{
    TransferOptions options(CollisionPolicy::Overwrite);
    options.copy = copyOptions;
    Run run(*m_journal, cancel, progress, options);
    for (const auto &copy : copies) {
        measureTree(copy.first, cancel, run.progress);
    }
    run.progress.itemsTotal += int(toTrash.size());
    if (!run.begin(journal::OperationKind::Copy, description)) {
        return run.outcome;
    }
    for (const auto &[source, target] : copies) {
        if (run.cancelled()) {
            run.outcome.unfinished.append({source, target});
            continue;
        }
        const QFileInfo info(target);
        const QString directory = info.absolutePath();
        // Las carpetas que falten hasta el destino, de fuera hacia dentro
        // (deshacer las quita si quedan vacías).
        QStringList missing;
        for (QString folder = directory; !stampOf(folder).exists;) {
            missing.prepend(folder);
            const QString parent = QFileInfo(folder).absolutePath();
            if (parent == folder) {
                break;
            }
            folder = parent;
        }
        bool ready = true;
        for (const QString &folder : std::as_const(missing)) {
            if (!run.step(journal::StepKind::MakeDirectory, {}, folder, {}, [&] { return makeDirectory(folder); })) {
                ready = false;
                break;
            }
        }
        if (!ready || !run.transfer(source, directory, info.fileName(), false, true)) {
            run.outcome.unfinished.append({source, target});
        }
    }
    for (const QString &path : toTrash) {
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
            ++run.progress.itemsDone;
        } else {
            trash::discardItemRecord(item.value());
        }
    }
    run.finish();
    return run.outcome;
}

Outcome Engine::resumeInterrupted(journal::OperationId operationId, const TransferOptions &options,
                                  const tasks::CancelToken &cancel, const ProgressCallback &progress)
{
    Outcome outcome;
    const core::Result<journal::Operation> operation = m_journal->operation(operationId);
    const core::Result<QList<QPair<QString, QString>>> plan = m_journal->plan(operationId);
    if (!operation.ok() || !plan.ok()) {
        outcome.errors.append(operation.ok() ? plan.error() : operation.error());
        return outcome;
    }
    const bool moving = operation.value().kind == journal::OperationKind::Move;
    if ((!moving && operation.value().kind != journal::OperationKind::Copy) || plan.value().isEmpty()) {
        outcome.errors.append({core::ErrorCode::NotSupported,
                               tr("Esta operación no se puede reanudar; se puede revertir o dar por perdida."), {}, 0});
        return outcome;
    }
    // La interrumpida queda cerrada; lo que falta se hace como una operación nueva.
    m_journal->finishOperation(operationId, journal::OperationState::Cancelled);
    return retry(plan.value(), moving, options, cancel, progress);
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

Outcome Engine::restoreFromTrash(const QList<trash::TrashItem> &items, const tasks::CancelToken &cancel)
{
    Run run(*m_journal, cancel, {});
    if (!run.begin(journal::OperationKind::Restore,
                   tr("Restaurar %1 elemento(s) de la papelera").arg(items.size()))) {
        return run.outcome;
    }
    for (const trash::TrashItem &item : items) {
        if (run.cancelled()) {
            break;
        }
        if (run.step(journal::StepKind::Rename, item.storedPath(), item.originalPath, item.originalPath,
                     [&] { return trash::restoreItem(item); })) {
            run.outcome.resultPaths.append(item.originalPath);
        }
    }
    run.finish();
    return run.outcome;
}

Outcome Engine::purgeFromTrash(const QList<trash::TrashItem> &items, const tasks::CancelToken &cancel)
{
    Run run(*m_journal, cancel, {});
    if (!run.begin(journal::OperationKind::Delete,
                   tr("Eliminar permanentemente %1 elemento(s) de la papelera").arg(items.size()))) {
        return run.outcome;
    }
    for (const trash::TrashItem &item : items) {
        if (run.cancelled()) {
            break;
        }
        const journal::StepKind kind = item.isDirectory ? journal::StepKind::RemoveDirectory
                                                        : journal::StepKind::RemoveFile;
        run.step(kind, item.storedPath(), {}, {}, [&] { return trash::purgeItem(item, cancel); });
    }
    run.finish();
    return run.outcome;
}

bool Engine::canUndo(const journal::Operation &operation)
{
    if (operation.kind == journal::OperationKind::Delete || operation.kind == journal::OperationKind::Restore) {
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
    if (operation.value().kind == journal::OperationKind::Restore) {
        // El registro del elemento en la papelera ya no existe: devolverlo
        // allí lo dejaría sin datos de origen. Se vuelve a enviar a la papelera.
        outcome.errors.append({core::ErrorCode::NotSupported,
                               tr("Una restauración no se deshace; envía de nuevo el elemento a la papelera."),
                               {}, 0});
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
            removePartialsUnder(step.target);
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
