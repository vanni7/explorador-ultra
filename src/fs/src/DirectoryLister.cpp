// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/fs/DirectoryLister.h"

namespace eu::fs {

core::Result<std::vector<Entry>> listDirectoryAll(const QString &path)
{
    std::vector<Entry> all;
    const core::Status status = listDirectory(path, ListOptions(), tasks::CancelToken(),
                                              [&all](std::vector<Entry> &&batch) {
        all.insert(all.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
    });
    if (!status.ok()) {
        return status.error();
    }
    return all;
}

} // namespace eu::fs
