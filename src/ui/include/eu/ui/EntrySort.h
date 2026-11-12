// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/fs/Entry.h"

#include <Qt>

#include <vector>

namespace eu::ui {

/// Columnas de la vista detallada (SPEC §5.4).
enum class Column {
    Name = 0,
    Size = 1,
    Type = 2,
    Modified = 3,
    Count = 4,
};

/// Extensión en minúsculas, sin el punto ("JPG" -> "jpg"). Vacía si no tiene.
/// Un nombre oculto como ".bashrc" no tiene extensión.
QString suffixOf(const QString &name);

/// Ordena los elementos para mostrarlos. Las carpetas van siempre primero; el
/// nombre usa orden natural y sirve de desempate en las demás columnas.
void sortEntries(std::vector<fs::Entry> &entries, Column column, Qt::SortOrder order);

} // namespace eu::ui
