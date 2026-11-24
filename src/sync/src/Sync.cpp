// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/sync/Sync.h"

#include "eu/core/NaturalCompare.h"
#include "eu/fs/DirectoryLister.h"
#include "eu/ops/FileOps.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSaveFile>

#include <cstdlib>

namespace eu::sync {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::sync", text);
}

QString joinPath(const QString &root, const QString &relative)
{
    if (relative.isEmpty()) {
        return root;
    }
    return root.endsWith(QLatin1Char('/')) ? root + relative : root + QLatin1Char('/') + relative;
}

struct Walk
{
    Comparison &result;
    const CompareOptions &options;
    const tasks::CancelToken &cancel;
    const std::function<void(const CompareProgress &)> &progress;
    CompareProgress state;

    bool stop()
    {
        if (cancel.isCancelled()) {
            result.cancelled = true;
        }
        return result.cancelled;
    }

    void report(const QString &path)
    {
        if (progress) {
            state.currentPath = path;
            progress(state);
        }
    }

    /// Lo que hay en una carpeta, por nombre, sin enlaces (ni ocultos si no se piden).
    QMap<QString, fs::Entry> read(const QString &path, bool *ok)
    {
        QMap<QString, fs::Entry> entries;
        const auto listed = fs::listDirectoryAll(path);
        if (!listed.ok()) {
            result.errors.append(listed.error());
            *ok = false;
            return entries;
        }
        *ok = true;
        for (const fs::Entry &entry : listed.value()) {
            if (entry.isSymlink) {
                ++result.skippedLinks;
                continue;
            }
            if (entry.isHidden && !options.includeHidden) {
                continue;
            }
            entries.insert(entry.name, entry);
        }
        ++state.foldersRead;
        report(path);
        return entries;
    }

    /// Lo que suma una carpeta que solo está en un lado (para el plan).
    qint64 treeSize(const QString &path)
    {
        qint64 total = 0;
        bool ok = false;
        const QMap<QString, fs::Entry> entries = read(path, &ok);
        for (const fs::Entry &entry : entries) {
            if (stop()) {
                break;
            }
            total += entry.isDirectory ? treeSize(joinPath(path, entry.name)) : entry.size;
        }
        return total;
    }

    State fileState(const Side &left, const Side &right, const QString &leftPath, const QString &rightPath)
    {
        const qint64 delta = left.modifiedMs - right.modifiedMs;
        const qint64 tolerance = options.timeToleranceMs;
        const auto byDate = [&] {
            if (delta > tolerance) {
                return State::LeftNewer;
            }
            if (delta < -tolerance) {
                return State::RightNewer;
            }
            return State::Different;
        };
        ++state.filesCompared;
        switch (options.criterion) {
        case Criterion::Name:
            return State::Identical;
        case Criterion::Size:
            return left.size == right.size ? State::Identical : byDate();
        case Criterion::SizeAndDate:
            return left.size == right.size && std::llabs(delta) <= tolerance ? State::Identical : byDate();
        case Criterion::Hash:
        case Criterion::Content: {
            if (left.size != right.size) {
                return byDate();
            }
            report(leftPath);
            const core::Result<bool> same = ops::sameContent(
                leftPath, rightPath, options.criterion == Criterion::Hash ? ops::Comparison::Hash : ops::Comparison::Bytes,
                cancel);
            if (!same.ok()) {
                if (same.error().code != core::ErrorCode::Cancelled) {
                    result.errors.append(same.error());
                }
                return byDate();
            }
            return same.value() ? State::Identical : byDate();
        }
        }
        return State::Different;
    }

    /// Compara una carpeta que está en los dos lados. Devuelve true si algo
    /// de dentro difiere.
    bool folder(const QString &relative)
    {
        if (stop()) {
            return false;
        }
        const QString leftDir = joinPath(result.left, relative);
        const QString rightDir = joinPath(result.right, relative);
        bool leftOk = false;
        bool rightOk = false;
        const QMap<QString, fs::Entry> leftEntries = read(leftDir, &leftOk);
        const QMap<QString, fs::Entry> rightEntries = read(rightDir, &rightOk);
        if (!leftOk || !rightOk) {
            return true;   // lo que no se pudo leer no se da por igual
        }
        QStringList names = leftEntries.keys();
        for (auto it = rightEntries.cbegin(); it != rightEntries.cend(); ++it) {
            if (!leftEntries.contains(it.key())) {
                names.append(it.key());
            }
        }
        // En el orden de las listas: natural y sin distinguir mayúsculas.
        std::sort(names.begin(), names.end(),
                  [](const QString &a, const QString &b) { return core::naturalCompare(a, b) < 0; });
        bool differs = false;
        for (const QString &name : std::as_const(names)) {
            if (stop()) {
                break;
            }
            Item item;
            item.relativePath = relative.isEmpty() ? name : relative + QLatin1Char('/') + name;
            const auto fill = [](Side &side, const fs::Entry &entry) {
                side.exists = true;
                side.isDirectory = entry.isDirectory;
                side.size = entry.isDirectory ? 0 : entry.size;
                side.modifiedMs = entry.modifiedMs;
            };
            if (const auto it = leftEntries.constFind(name); it != leftEntries.cend()) {
                fill(item.left, it.value());
            }
            if (const auto it = rightEntries.constFind(name); it != rightEntries.cend()) {
                fill(item.right, it.value());
            }
            const QString leftPath = joinPath(result.left, item.relativePath);
            const QString rightPath = joinPath(result.right, item.relativePath);
            if (!item.right.exists) {
                item.state = State::LeftOnly;
                if (item.left.isDirectory) {
                    item.left.size = treeSize(leftPath);
                }
            } else if (!item.left.exists) {
                item.state = State::RightOnly;
                if (item.right.isDirectory) {
                    item.right.size = treeSize(rightPath);
                }
            } else if (item.left.isDirectory != item.right.isDirectory) {
                item.state = State::TypeConflict;
            } else if (item.left.isDirectory) {
                item.state = State::Identical;
                const qsizetype index = result.items.size();
                result.items.append(item);
                const bool inside = folder(item.relativePath);
                result.items[index].differsInside = inside;
                differs = differs || inside;
                continue;
            } else {
                item.state = fileState(item.left, item.right, leftPath, rightPath);
            }
            differs = differs || item.state != State::Identical;
            result.items.append(item);
        }
        return differs;
    }
};

} // namespace

int Comparison::count(State state) const
{
    int total = 0;
    for (const Item &item : items) {
        total += item.state == state && !(item.left.isDirectory && item.right.isDirectory) ? 1 : 0;
    }
    return total;
}

bool Comparison::sameTrees() const
{
    if (!errors.isEmpty() || cancelled) {
        return false;
    }
    for (const Item &item : items) {
        if (item.state != State::Identical) {
            return false;
        }
    }
    return true;
}

Comparison compare(const QString &left, const QString &right, const CompareOptions &options,
                   const tasks::CancelToken &cancel, const std::function<void(const CompareProgress &)> &progress)
{
    Comparison result;
    result.left = QDir::cleanPath(left);
    result.right = QDir::cleanPath(right);
    Walk walk{result, options, cancel, progress, {}};
    walk.folder(QString());
    return result;
}

// ---------------------------------------------------------------- plan

int Plan::count(ActionKind kind) const
{
    int total = 0;
    for (const Action &action : actions) {
        total += action.kind == kind ? 1 : 0;
    }
    return total;
}

bool Plan::hasChanges() const
{
    for (const Action &action : actions) {
        if (action.kind != ActionKind::Skip) {
            return true;
        }
    }
    return false;
}

Plan plan(const Comparison &comparison, const SyncOptions &options, const SyncState *previous, qint64 timeToleranceMs)
{
    Plan result;
    for (const Item &item : comparison.items) {
        if (item.left.exists && item.right.exists && item.left.isDirectory && item.right.isDirectory) {
            continue;   // carpetas en los dos lados: decide lo de dentro
        }
        if (item.state == State::Identical) {
            continue;
        }
        Action action;
        action.relativePath = item.relativePath;
        action.isDirectory = item.left.exists ? item.left.isDirectory : item.right.isDirectory;
        const auto copyFrom = [&](bool fromLeft, const QString &reason) {
            const Side &source = fromLeft ? item.left : item.right;
            const Side &target = fromLeft ? item.right : item.left;
            action.kind = fromLeft ? ActionKind::CopyToRight : ActionKind::CopyToLeft;
            action.bytes = source.size;
            action.replaces = target.exists;
            action.reason = reason;
        };
        const auto skip = [&](const QString &reason, bool conflict) {
            action.kind = ActionKind::Skip;
            action.conflict = conflict;
            action.reason = reason;
        };
        const auto keepDeletion = [&](const QString &reason) {
            skip(reason, false);
            action.deletionKept = true;
        };

        // Cómo estaba en la última sincronización (si la hubo).
        const SyncState::Entry *was = nullptr;
        if (previous != nullptr) {
            const auto found = previous->entries.constFind(item.relativePath);
            if (found != previous->entries.cend()) {
                was = &found.value();
            }
        }
        const auto changed = [&](const Side &side, bool isLeft) {
            if (was == nullptr || side.isDirectory != was->isDirectory) {
                return true;
            }
            if (side.isDirectory) {
                return false;
            }
            const qint64 before = isLeft ? was->leftModifiedMs : was->rightModifiedMs;
            return side.size != was->size || std::llabs(side.modifiedMs - before) > timeToleranceMs;
        };
        const bool oneSide = item.state == State::LeftOnly || item.state == State::RightOnly;
        const bool deletedOnLeft = item.state == State::RightOnly;
        // Se borró en un lado: estaba en los dos y el que queda no cambió.
        const bool deleted = oneSide && was != nullptr
            && !changed(deletedOnLeft ? item.right : item.left, !deletedOnLeft);
        const auto applyDeletion = [&] {
            action.deletion = true;
            action.deletedOnLeft = deletedOnLeft;
            DeletionChoice choice = options.propagateDeletions ? DeletionChoice::Propagate : DeletionChoice::Keep;
            if (options.direction != Direction::Both) {
                const bool inTarget = (options.direction == Direction::LeftToRight) != deletedOnLeft;
                if (inTarget) {
                    choice = DeletionChoice::Restore;
                }
            }
            choice = options.deletionChoices.value(item.relativePath, choice);
            const QString where = deletedOnLeft ? tr("Se borró a la izquierda") : tr("Se borró a la derecha");
            switch (choice) {
            case DeletionChoice::Propagate:
                action.kind = deletedOnLeft ? ActionKind::TrashRight : ActionKind::TrashLeft;
                action.reason = where + tr(": también se quita del otro lado (a la papelera)");
                break;
            case DeletionChoice::Restore:
                copyFrom(deletedOnLeft == false, where + tr(": se recupera desde el otro lado"));
                break;
            case DeletionChoice::Keep:
                keepDeletion(where + tr(": se deja como está"));
                ++result.deletionsNotPropagated;
                break;
            }
        };

        if (item.state == State::TypeConflict) {
            skip(tr("Un archivo en un lado y una carpeta en el otro"), true);
        } else if (deleted) {
            applyDeletion();
        } else if (options.direction != Direction::Both) {
            const bool fromLeft = options.direction == Direction::LeftToRight;
            const Side &source = fromLeft ? item.left : item.right;
            const Side &target = fromLeft ? item.right : item.left;
            if (!source.exists) {
                if (options.propagateDeletions) {
                    action.kind = fromLeft ? ActionKind::TrashRight : ActionKind::TrashLeft;
                    action.reason = tr("No está en el origen: a la papelera");
                } else {
                    skip(tr("Solo en el destino: no se borra"), false);
                    ++result.deletionsNotPropagated;
                }
            } else if (!target.exists) {
                copyFrom(fromLeft, tr("Falta en el destino"));
            } else {
                const bool targetNewer = (fromLeft && item.state == State::RightNewer)
                    || (!fromLeft && item.state == State::LeftNewer);
                if (!targetNewer) {
                    copyFrom(fromLeft, item.state == State::Different ? tr("Distinto") : tr("Más reciente en el origen"));
                } else if (options.conflicts == ConflictPolicy::LeftWins && fromLeft) {
                    copyFrom(true, tr("El destino es más reciente, pero manda el origen"));
                    action.conflict = true;
                } else if (options.conflicts == ConflictPolicy::RightWins && !fromLeft) {
                    copyFrom(false, tr("El destino es más reciente, pero manda el origen"));
                    action.conflict = true;
                } else {
                    skip(tr("El destino es más reciente"), options.conflicts == ConflictPolicy::Skip);
                }
            }
        } else {
            // Bidireccional: qué cambió desde la última vez en cada lado.
            const auto resolve = [&](const QString &why) {
                action.conflict = true;
                const bool leftNewer = item.state == State::LeftNewer;
                const bool rightNewer = item.state == State::RightNewer;
                switch (options.conflicts) {
                case ConflictPolicy::LeftWins:
                    copyFrom(true, why + tr(" — manda la izquierda"));
                    action.conflict = true;
                    return;
                case ConflictPolicy::RightWins:
                    copyFrom(false, why + tr(" — manda la derecha"));
                    action.conflict = true;
                    return;
                case ConflictPolicy::NewerWins:
                    if (leftNewer || rightNewer) {
                        copyFrom(leftNewer, why + tr(" — gana el más reciente"));
                        action.conflict = true;
                        return;
                    }
                    break;
                case ConflictPolicy::Skip:
                    break;
                }
                skip(why, true);
            };
            if (item.state == State::LeftOnly || item.state == State::RightOnly) {
                const bool onLeft = item.state == State::LeftOnly;
                if (was == nullptr) {
                    copyFrom(onLeft, onLeft ? tr("Nuevo a la izquierda") : tr("Nuevo a la derecha"));
                } else {
                    // (Si el que queda no cambió, ya se trató arriba como borrado.)
                    resolve(onLeft ? tr("Se borró a la derecha y cambió a la izquierda")
                                   : tr("Se borró a la izquierda y cambió a la derecha"));
                }
            } else {
                const bool leftChanged = changed(item.left, true);
                const bool rightChanged = changed(item.right, false);
                if (leftChanged && !rightChanged) {
                    copyFrom(true, tr("Cambió a la izquierda"));
                } else if (rightChanged && !leftChanged) {
                    copyFrom(false, tr("Cambió a la derecha"));
                } else {
                    resolve(was == nullptr ? tr("Distintos y sin sincronización anterior") : tr("Cambió en los dos lados"));
                }
            }
        }
        if (action.kind == ActionKind::Skip && action.conflict) {
            ++result.conflicts;
        }
        if (action.kind == ActionKind::CopyToLeft || action.kind == ActionKind::CopyToRight) {
            result.bytesToCopy += action.bytes;
        }
        result.actions.append(action);
    }
    return result;
}

SyncState stateFrom(const Comparison &after, const Plan &applied, const SyncState *previous)
{
    SyncState state;
    state.savedMs = QDateTime::currentMSecsSinceEpoch();
    for (const Item &item : after.items) {
        if (item.state != State::Identical || !item.left.exists || !item.right.exists) {
            continue;
        }
        state.entries.insert(item.relativePath, {item.left.isDirectory, item.left.size, item.left.modifiedMs,
                                                 item.right.modifiedMs});
    }
    // Un borrado que no se propagó se sigue recordando: si no, la próxima vez
    // parecería nuevo en el otro lado y se volvería a copiar.
    if (previous != nullptr) {
        for (const Action &action : applied.actions) {
            if (action.deletionKept) {
                const auto found = previous->entries.constFind(action.relativePath);
                if (found != previous->entries.cend()) {
                    state.entries.insert(action.relativePath, found.value());
                }
            }
        }
    }
    return state;
}

QString stateFile(const QString &directory, const QString &left, const QString &right)
{
    const QByteArray key = (QDir::cleanPath(left) + QLatin1Char('\n') + QDir::cleanPath(right)).toUtf8();
    const QString name = QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(20));
    return joinPath(directory, QStringLiteral("sincronizacion/") + name + QStringLiteral(".json"));
}

core::Result<SyncState> loadState(const QString &file)
{
    QFile input(file);
    if (!input.exists()) {
        return SyncState();
    }
    if (!input.open(QIODevice::ReadOnly)) {
        return core::Error{core::ErrorCode::IoError, input.errorString(), file, 0};
    }
    const QJsonObject object = QJsonDocument::fromJson(input.readAll()).object();
    if (object.value(QStringLiteral("version")).toInt() != 1) {
        return core::Error{core::ErrorCode::Corrupted, tr("El estado guardado de la sincronización no se entiende"),
                           file, 0};
    }
    SyncState state;
    state.savedMs = qint64(object.value(QStringLiteral("savedMs")).toDouble());
    const QJsonArray entries = object.value(QStringLiteral("entries")).toArray();
    for (const QJsonValue &value : entries) {
        const QJsonArray entry = value.toArray();
        state.entries.insert(entry.at(0).toString(),
                             {entry.at(1).toBool(), qint64(entry.at(2).toDouble()), qint64(entry.at(3).toDouble()),
                              qint64(entry.at(4).toDouble())});
    }
    return state;
}

core::Status saveState(const QString &file, const SyncState &state)
{
    QDir().mkpath(QFileInfo(file).absolutePath());
    QJsonArray entries;
    for (auto it = state.entries.cbegin(); it != state.entries.cend(); ++it) {
        entries.append(QJsonArray{it.key(), it->isDirectory, double(it->size), double(it->leftModifiedMs),
                                  double(it->rightModifiedMs)});
    }
    const QJsonObject object{{QStringLiteral("version"), 1},
                             {QStringLiteral("savedMs"), double(state.savedMs)},
                             {QStringLiteral("entries"), entries}};
    QSaveFile output(file);
    if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0
        || !output.commit()) {
        return core::Error{core::ErrorCode::IoError, output.errorString(), file, 0};
    }
    return {};
}

} // namespace eu::sync
