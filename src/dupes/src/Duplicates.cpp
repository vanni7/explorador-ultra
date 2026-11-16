// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/dupes/Duplicates.h"

#include "eu/ops/FileOps.h"
#include "eu/search/Query.h"
#include "eu/search/Searcher.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>

#include <algorithm>
#include <map>
#include <vector>

namespace eu::dupes {

namespace {

constexpr qint64 kSample = 4096;   ///< Bytes de cada muestra del hash parcial.

struct Candidate
{
    File file;
    qint64 size = 0;
};

/// Hash de las muestras de inicio, medio y final (el archivo entero si es pequeño).
QByteArray partialHash(const QString &path, qint64 size, qint64 &bytesRead)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    ops::Hasher hasher(ops::HashAlgorithm::Sha256);
    QByteArray buffer;
    if (size <= 3 * kSample) {
        buffer = file.readAll();
        bytesRead += buffer.size();
        hasher.addData(buffer.constData(), buffer.size());
        return hasher.result();
    }
    for (const qint64 offset : {qint64(0), size / 2 - kSample / 2, size - kSample}) {
        if (!file.seek(offset)) {
            return {};
        }
        buffer = file.read(kSample);
        bytesRead += buffer.size();
        hasher.addData(buffer.constData(), buffer.size());
    }
    return hasher.result();
}

/// Reparte un conjunto de candidatos en subgrupos por una clave; se quedan
/// los subgrupos con al menos dos copias físicas distintas.
template<typename KeyFunction>
std::vector<std::vector<Candidate>> splitBy(const std::vector<Candidate> &group, KeyFunction key,
                                            const tasks::CancelToken &cancel)
{
    std::map<QByteArray, std::vector<Candidate>> buckets;
    for (const Candidate &candidate : group) {
        if (cancel.isCancelled()) {
            return {};
        }
        const QByteArray value = key(candidate);
        if (!value.isEmpty()) {
            buckets[value].push_back(candidate);
        }
    }
    std::vector<std::vector<Candidate>> out;
    for (auto &[value, members] : buckets) {
        // Más de una copia física (dos enlaces del mismo archivo no cuentan).
        bool distinct = false;
        for (size_t i = 1; i < members.size() && !distinct; ++i) {
            distinct = members[i].file.fileId != members[0].file.fileId
                || members[i].file.deviceId != members[0].file.deviceId || members[0].file.fileId == 0;
        }
        if (distinct) {
            out.push_back(std::move(members));
        }
    }
    return out;
}

} // namespace

qint64 Result::recoverable() const
{
    qint64 total = 0;
    for (const Group &group : groups) {
        total += group.recoverable();
    }
    return total;
}

Result findDuplicates(const Options &options, const tasks::CancelToken &cancel,
                      const std::function<void(const Progress &)> &onProgress)
{
    Result result;
    Progress progress;
    QElapsedTimer sinceReport;
    sinceReport.start();
    const auto report = [&](bool force) {
        if (onProgress && (force || sinceReport.elapsed() > 200)) {
            onProgress(progress);
            sinceReport.restart();
        }
    };
    const auto waitIfPaused = [&] {
        if (options.pause != nullptr) {
            options.pause->waitWhilePaused(cancel);
        }
    };

    // 1. Todos los archivos (como la búsqueda: sin seguir enlaces ni cruzar unidades).
    std::map<qint64, std::vector<Candidate>> bySize;
    search::SearchOptions walk;
    walk.includeHidden = options.includeHidden;
    walk.recursive = true;
    walk.maxHits = std::numeric_limits<qint64>::max();
    search::searchTree(options.roots, search::Query::parse(QStringLiteral("*")).value(), walk, cancel, options.pause,
                       [&](std::vector<search::Hit> &&hits, const search::SearchProgress &) {
        for (const search::Hit &hit : hits) {
            if (hit.entry.isDirectory || hit.entry.isSymlink || hit.entry.size < options.minSize) {
                continue;
            }
            Candidate candidate;
            candidate.file.path = hit.path();
            candidate.file.modifiedMs = hit.entry.modifiedMs;
            candidate.file.deviceId = hit.entry.deviceId;
            candidate.file.fileId = hit.entry.fileId;
            candidate.size = hit.entry.size;
            bySize[hit.entry.size].push_back(candidate);
            ++progress.filesSeen;
        }
        report(false);
    });
    result.filesSeen = progress.filesSeen;
    if (cancel.isCancelled()) {
        return result;
    }

    // 2. Por tamaño: solo cuenta lo que tiene al menos otra copia física del mismo tamaño.
    progress.stage = Progress::Stage::BySize;
    std::vector<std::vector<Candidate>> groups;
    for (auto &[size, members] : bySize) {
        if (members.size() < 2) {
            continue;
        }
        auto split = splitBy(members, [](const Candidate &) { return QByteArray("="); }, cancel);
        if (split.empty()) {
            ++result.hardlinkOnlySets;   // todos eran el mismo archivo
        }
        for (auto &group : split) {
            groups.push_back(std::move(group));
        }
    }
    report(true);

    // 3. Hash parcial y 4. hash completo; los de cada copia física se leen una vez.
    const auto runStage = [&](Progress::Stage stage, const std::function<QByteArray(const Candidate &)> &key) {
        progress.stage = stage;
        progress.candidates = 0;
        progress.done = 0;
        for (const auto &group : groups) {
            progress.candidates += qint64(group.size());
        }
        report(true);   // cada cambio de etapa se avisa
        std::vector<std::vector<Candidate>> next;
        for (const auto &group : groups) {
            waitIfPaused();
            if (cancel.isCancelled()) {
                return;
            }
            QHash<QPair<quint64, quint64>, QByteArray> known;   // (dispositivo, inodo) -> hash
            auto split = splitBy(group, [&](const Candidate &candidate) {
                const QPair<quint64, quint64> id{candidate.file.deviceId, candidate.file.fileId};
                QByteArray value = candidate.file.fileId != 0 ? known.value(id) : QByteArray();
                if (value.isEmpty()) {
                    value = key(candidate);
                    if (candidate.file.fileId != 0) {
                        known.insert(id, value);
                    }
                }
                ++progress.done;
                report(false);
                return value;
            }, cancel);
            for (auto &part : split) {
                next.push_back(std::move(part));
            }
        }
        groups = std::move(next);
    };
    runStage(Progress::Stage::PartialHash, [&](const Candidate &candidate) {
        return partialHash(candidate.file.path, candidate.size, progress.bytesRead);
    });
    runStage(Progress::Stage::FullHash, [&](const Candidate &candidate) -> QByteArray {
        if (candidate.size <= 3 * kSample) {
            return QByteArray("parcial=completo");   // el parcial ya leyó el archivo entero
        }
        const core::Result<QByteArray> hash = ops::hashFile(candidate.file.path, ops::HashAlgorithm::Sha256, cancel);
        if (hash.ok()) {
            progress.bytesRead += candidate.size;
        }
        return hash.ok() ? hash.value() : QByteArray();
    });

    // 5. Byte a byte, si se pide: cada uno contra el primero de su grupo.
    if (options.byteByByte) {
        progress.stage = Progress::Stage::Bytes;
        progress.candidates = 0;
        progress.done = 0;
        for (const auto &group : groups) {
            progress.candidates += qint64(group.size());
        }
        report(true);
        std::vector<std::vector<Candidate>> confirmed;
        for (const auto &group : groups) {
            waitIfPaused();
            if (cancel.isCancelled()) {
                return result;
            }
            const Candidate &reference = group.front();
            std::vector<Candidate> same{reference};
            for (size_t i = 1; i < group.size(); ++i) {
                const Candidate &candidate = group[i];
                bool equal = candidate.file.fileId != 0 && candidate.file.fileId == reference.file.fileId
                    && candidate.file.deviceId == reference.file.deviceId;   // el mismo archivo
                if (!equal) {
                    const core::Result<bool> compared =
                        ops::sameContent(reference.file.path, candidate.file.path, ops::Comparison::Bytes, cancel);
                    equal = compared.ok() && compared.value();
                    progress.bytesRead += 2 * candidate.size;
                }
                if (equal) {
                    same.push_back(candidate);
                }
                ++progress.done;
                report(false);
            }
            for (auto &part : splitBy(same, [](const Candidate &) { return QByteArray("="); }, cancel)) {
                confirmed.push_back(std::move(part));
            }
        }
        groups = std::move(confirmed);
    }
    if (cancel.isCancelled()) {
        return result;
    }

    // Los grupos, con su numeración de copias físicas.
    for (const auto &members : groups) {
        Group group;
        group.size = members.front().size;
        QHash<QPair<quint64, quint64>, int> physicalIndex;
        for (const Candidate &candidate : members) {
            File file = candidate.file;
            const QPair<quint64, quint64> id{file.deviceId, file.fileId};
            if (file.fileId == 0) {
                file.physical = group.physicalCopies++;
            } else if (physicalIndex.contains(id)) {
                file.physical = physicalIndex.value(id);
            } else {
                file.physical = group.physicalCopies++;
                physicalIndex.insert(id, file.physical);
            }
            group.files.append(file);
        }
        std::sort(group.files.begin(), group.files.end(),
                  [](const File &a, const File &b) { return a.path < b.path; });
        result.groups.append(group);
    }
    std::sort(result.groups.begin(), result.groups.end(),
              [](const Group &a, const Group &b) { return a.recoverable() > b.recoverable(); });
    result.bytesRead = progress.bytesRead;
    progress.stage = Progress::Stage::Done;
    report(true);
    return result;
}

QStringList proposeRemoval(const Group &group, Keep keep, const QString &preferredPath)
{
    if (group.files.isEmpty()) {
        return {};
    }
    const QString preferred = QDir::cleanPath(preferredPath);
    const auto inPreferred = [&](const File &file) {
        return !preferredPath.isEmpty()
            && (file.path == preferred || file.path.startsWith(preferred.endsWith(QLatin1Char('/')) ? preferred
                                                                                                    : preferred + QLatin1Char('/')));
    };
    const auto better = [&](const File &a, const File &b) {
        switch (keep) {
        case Keep::Newest:
            return a.modifiedMs > b.modifiedMs;
        case Keep::ShortestPath:
            return a.path.size() < b.path.size();
        case Keep::Preferred:
            if (inPreferred(a) != inPreferred(b)) {
                return inPreferred(a);
            }
            return a.modifiedMs < b.modifiedMs;
        case Keep::Oldest:
            break;
        }
        return a.modifiedMs < b.modifiedMs;
    };
    const File *kept = &group.files.first();
    for (const File &file : group.files) {
        if (better(file, *kept)) {
            kept = &file;
        }
    }
    QStringList remove;
    for (const File &file : group.files) {
        if (file.physical != kept->physical) {
            remove.append(file.path);
        }
    }
    return remove;
}

} // namespace eu::dupes
