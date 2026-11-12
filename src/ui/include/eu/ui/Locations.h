// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace eu::ui {

/// Ubicación virtual "Unidades": la lista de discos con su espacio (el
/// equivalente a "Este equipo"). No es una carpeta del disco: las pestañas la
/// reconocen por este texto, que nunca puede ser una ruta absoluta.
inline QString volumesLocation()
{
    return QStringLiteral("unidades:");
}

inline bool isVolumesLocation(const QString &path)
{
    return path.compare(volumesLocation(), Qt::CaseInsensitive) == 0;
}

} // namespace eu::ui
