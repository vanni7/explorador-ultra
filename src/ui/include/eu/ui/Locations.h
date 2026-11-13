// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace eu::ui {

// Ubicaciones virtuales: lo que una pestaña puede mostrar además de una
// carpeta del disco. Se reconocen por su texto, que nunca puede ser una ruta
// absoluta, y viajan por el historial de navegación igual que una carpeta.

/// "Unidades": la lista de discos con su espacio (el equivalente a "Este equipo").
inline QString volumesLocation()
{
    return QStringLiteral("unidades:");
}

inline bool isVolumesLocation(const QString &path)
{
    return path.compare(volumesLocation(), Qt::CaseInsensitive) == 0;
}

/// Separa la consulta de la carpeta dentro de una ubicación de búsqueda. Es un
/// carácter de control que no puede aparecer en ninguna de las dos.
inline constexpr QChar searchSeparator = QChar(0x1f);

/// Resultados de buscar `query` en `root` y sus subcarpetas.
inline QString searchLocation(const QString &root, const QString &query)
{
    return QStringLiteral("buscar:") + query + searchSeparator + root;
}

inline bool isSearchLocation(const QString &path)
{
    return path.startsWith(QLatin1String("buscar:")) && path.contains(searchSeparator);
}

/// Carpeta donde busca una ubicación de búsqueda.
inline QString searchRootOf(const QString &location)
{
    return location.mid(location.indexOf(searchSeparator) + 1);
}

/// Texto de la consulta de una ubicación de búsqueda.
inline QString searchQueryOf(const QString &location)
{
    const qsizetype start = 7;   // tras "buscar:"
    return location.mid(start, location.indexOf(searchSeparator) - start);
}

/// true si no es una carpeta del disco.
inline bool isVirtualLocation(const QString &path)
{
    return isVolumesLocation(path) || isSearchLocation(path);
}

} // namespace eu::ui
