// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/fs/TreeSize.h"

#include "eu/fs/DirectoryLister.h"

#include <QStringList>

namespace eu::fs {

TreeSize measureTree(const QString &path, const tasks::CancelToken &cancel, const TreeSizeCallback &onProgress)
{
    TreeSize total;
    // Pila de carpetas por leer en lugar de recursión: un árbol muy profundo
    // no puede desbordar la pila del hilo.
    QStringList pending{path};
    ListOptions options;
    options.firstBatchSize = options.batchSize;   // aquí no hay nada que pintar pronto

    while (!pending.isEmpty()) {
        if (cancel.isCancelled()) {
            total.cancelled = true;
            return total;
        }
        const QString directory = pending.takeLast();
        const QString prefix = directory.endsWith(QLatin1Char('/')) ? directory : directory + QLatin1Char('/');
        const core::Status status = listDirectory(directory, options, cancel, [&](std::vector<Entry> &&batch) {
            for (const Entry &entry : batch) {
                if (entry.isDirectory && !entry.isSymlink) {
                    ++total.folders;
                    pending.append(prefix + entry.name);
                } else {
                    ++total.files;
                    if (!entry.isSymlink && entry.size > 0) {
                        total.bytes += entry.size;
                    }
                }
            }
        });
        if (!status.ok()) {
            if (status.error().code == core::ErrorCode::Cancelled) {
                total.cancelled = true;
                return total;
            }
            total.incomplete = true;   // carpeta ilegible: se sigue con las demás
        }
        if (onProgress) {
            onProgress(total);
        }
    }
    return total;
}

} // namespace eu::fs
