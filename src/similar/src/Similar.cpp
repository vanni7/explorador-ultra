// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/similar/Similar.h"

#include "eu/search/Query.h"
#include "eu/search/Searcher.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>

#include <algorithm>
#include <bit>
#include <numeric>
#include <vector>

namespace eu::similar {

Fingerprint fingerprint(const QImage &image)
{
    Fingerprint print;
    if (image.isNull()) {
        return print;
    }
    // 9×9 en grises: cada píxel contra el de su derecha y el de debajo.
    const QImage small = image.convertToFormat(QImage::Format_Grayscale8)
                             .scaled(9, 9, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    int bit = 0;
    for (int y = 0; y < 8; ++y) {
        const uchar *row = small.constScanLine(y);
        const uchar *below = small.constScanLine(y + 1);
        for (int x = 0; x < 8; ++x, ++bit) {
            if (row[x] < row[x + 1]) {
                print.horizontal |= quint64(1) << bit;
            }
            if (row[x] < below[x]) {
                print.vertical |= quint64(1) << bit;
            }
        }
    }
    print.valid = true;
    return print;
}

int distance(const Fingerprint &a, const Fingerprint &b)
{
    return std::popcount(a.horizontal ^ b.horizontal) + std::popcount(a.vertical ^ b.vertical);
}

int similarityPercent(int bits)
{
    return 100 - (bits * 100 + 64) / 128;
}

int distanceForPercent(int percent)
{
    return std::clamp((100 - percent) * 128 / 100, 0, 128);
}

QImage decodeSmall(const QString &path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize original = reader.size();
    if (original.isValid() && (original.width() > 64 || original.height() > 64)) {
        // JPEG decodifica a escala: mucho más rápido que leer la foto entera.
        reader.setScaledSize(original.scaled(64, 64, Qt::KeepAspectRatio));
    }
    return reader.read();
}

Result findSimilar(const Options &options, const Decoder &decoder, const tasks::CancelToken &cancel,
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

    // Las imágenes (como la búsqueda: sin seguir enlaces ni cruzar unidades).
    std::vector<Image> images;
    search::SearchOptions walk;
    walk.includeHidden = options.includeHidden;
    walk.maxHits = std::numeric_limits<qint64>::max();
    search::searchTree(options.roots, search::Query::parse(QStringLiteral("*")).value(), walk, cancel, options.pause,
                       [&](std::vector<search::Hit> &&hits, const search::SearchProgress &) {
        for (const search::Hit &hit : hits) {
            if (hit.entry.isDirectory || hit.entry.isSymlink || hit.entry.size <= 0
                || !options.extensions.contains(QFileInfo(hit.entry.name).suffix().toLower())) {
                continue;
            }
            images.push_back(Image{hit.path(), hit.entry.size, hit.entry.modifiedMs, 0});
            ++progress.found;
        }
        report(false);
    });

    // La huella de cada una.
    std::vector<Fingerprint> prints(images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        if (options.pause != nullptr) {
            options.pause->waitWhilePaused(cancel);
        }
        if (cancel.isCancelled()) {
            return result;
        }
        prints[i] = fingerprint(decoder ? decoder(images[i].path, cancel) : decodeSmall(images[i].path));
        if (!prints[i].valid) {
            ++result.unreadable;
        }
        ++progress.done;
        report(false);
    }
    result.images = qint64(images.size());

    // Todas contra todas; las que se parecen lo bastante, juntas (unión de conjuntos).
    progress.comparing = true;
    report(true);
    std::vector<size_t> parent(images.size());
    std::iota(parent.begin(), parent.end(), size_t(0));
    const std::function<size_t(size_t)> root = [&](size_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    for (size_t i = 0; i < images.size(); ++i) {
        if (!prints[i].valid) {
            continue;
        }
        if (i % 512 == 0 && cancel.isCancelled()) {
            return result;
        }
        for (size_t j = i + 1; j < images.size(); ++j) {
            if (prints[j].valid && distance(prints[i], prints[j]) <= options.maxDistance) {
                parent[root(j)] = root(i);
            }
        }
    }
    std::vector<std::vector<size_t>> members(images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        if (prints[i].valid) {
            members[root(i)].push_back(i);
        }
    }
    for (const auto &indexes : members) {
        if (indexes.size() < 2) {
            continue;
        }
        Group group;
        // La primera, la de más bytes (suele ser el original); las demás, por parecido a ella.
        size_t first = indexes.front();
        for (const size_t index : indexes) {
            if (images[index].size > images[first].size) {
                first = index;
            }
        }
        for (const size_t index : indexes) {
            Image image = images[index];
            image.distance = distance(prints[first], prints[index]);
            group.maxDistance = std::max(group.maxDistance, image.distance);
            group.images.append(image);
        }
        std::sort(group.images.begin(), group.images.end(), [&](const Image &a, const Image &b) {
            if (a.distance != b.distance) {
                return a.distance < b.distance;
            }
            return a.size > b.size;
        });
        result.groups.append(group);
    }
    std::sort(result.groups.begin(), result.groups.end(),
              [](const Group &a, const Group &b) { return a.images.size() > b.images.size(); });
    report(true);
    return result;
}

} // namespace eu::similar
