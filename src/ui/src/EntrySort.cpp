// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/EntrySort.h"

#include "eu/core/NaturalCompare.h"

#include <algorithm>

namespace eu::ui {

QString suffixOf(const QString &name)
{
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == name.size() - 1) {
        return {};
    }
    return name.mid(dot + 1).toLower();
}

void sortEntries(std::vector<fs::Entry> &entries, Column column, Qt::SortOrder order)
{
    const bool ascending = (order == Qt::AscendingOrder);

    const auto lessThan = [column, ascending](const fs::Entry &a, const fs::Entry &b) {
        // Las carpetas primero, sea cual sea la columna y el sentido.
        if (a.isDirectory != b.isDirectory) {
            return a.isDirectory;
        }

        int result = 0;
        switch (column) {
        case Column::Size:
            // El tamaño de una carpeta no es comparable: entre carpetas manda el nombre.
            if (!a.isDirectory && a.size != b.size) {
                result = (a.size < b.size) ? -1 : 1;
            }
            break;
        case Column::Type:
            if (!a.isDirectory) {
                result = QString::compare(suffixOf(a.name), suffixOf(b.name));
            }
            break;
        case Column::Modified:
            if (a.modifiedMs != b.modifiedMs) {
                result = (a.modifiedMs < b.modifiedMs) ? -1 : 1;
            }
            break;
        case Column::Name:
        case Column::Count:
            break;
        }

        if (result == 0) {
            // Desempate (y criterio principal en la columna Nombre).
            result = core::naturalCompare(a.name, b.name);
        }
        return ascending ? result < 0 : result > 0;
    };

    std::sort(entries.begin(), entries.end(), lessThan);
}

} // namespace eu::ui
