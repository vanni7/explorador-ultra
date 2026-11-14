// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>

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

/// Separa los campos de una ubicación de búsqueda. Es un carácter de control
/// que no puede aparecer en una consulta ni en una ruta.
inline constexpr QChar searchSeparator = QChar(0x1f);
/// Separa las carpetas de partida entre sí.
inline constexpr QChar searchRootSeparator = QChar(0x1e);

/// Dónde se busca (SPEC §6.2).
enum class SearchScope {
    Folder,          ///< Solo la carpeta, sin entrar en las subcarpetas.
    Subfolders,      ///< La carpeta y todo lo que cuelga de ella.
    Volume,          ///< La unidad entera donde está la carpeta.
    AllVolumes,      ///< Todas las unidades accesibles.
    ChosenVolumes,   ///< Las unidades que eligió el usuario.
};

/// Todo lo que define una búsqueda. Viaja entero dentro de la ubicación, de
/// modo que Atrás, Adelante y "Buscar de nuevo" repiten exactamente la misma.
struct SearchSpec
{
    QString query;
    /// Lo que mostraba la pestaña al buscar: adonde se vuelve con Subir.
    QString origin;
    SearchScope scope = SearchScope::Subfolders;
    /// Carpetas de partida. Con los alcances de carpeta es solo `origin`.
    QStringList roots;
    bool caseSensitive = false;
    /// Las palabras sueltas se buscan dentro de los archivos, no en el nombre.
    bool inContent = false;
};

inline QString searchLocation(const SearchSpec &spec)
{
    QString flags = QString::number(int(spec.scope));
    if (spec.caseSensitive) {
        flags += QLatin1Char('m');
    }
    if (spec.inContent) {
        flags += QLatin1Char('c');
    }
    const QStringList roots = spec.roots.isEmpty() ? QStringList{spec.origin} : spec.roots;
    return QStringLiteral("buscar:") + spec.query + searchSeparator + spec.origin + searchSeparator + flags
        + searchSeparator + roots.join(searchRootSeparator);
}

/// Resultados de buscar `query` en `root` y sus subcarpetas.
inline QString searchLocation(const QString &root, const QString &query)
{
    SearchSpec spec;
    spec.query = query;
    spec.origin = root;
    return searchLocation(spec);
}

inline bool isSearchLocation(const QString &path)
{
    return path.startsWith(QLatin1String("buscar:")) && path.contains(searchSeparator);
}

inline SearchSpec searchSpecOf(const QString &location)
{
    SearchSpec spec;
    const QStringList fields = location.mid(7).split(searchSeparator);   // tras "buscar:"
    spec.query = fields.value(0);
    spec.origin = fields.value(1);
    const QString flags = fields.value(2);
    const int scope = flags.left(1).toInt();
    spec.scope = (scope >= 0 && scope <= int(SearchScope::ChosenVolumes)) ? SearchScope(scope)
                                                                           : SearchScope::Subfolders;
    spec.caseSensitive = flags.contains(QLatin1Char('m'));
    spec.inContent = flags.contains(QLatin1Char('c'));
    spec.roots = fields.value(3).split(searchRootSeparator, Qt::SkipEmptyParts);
    if (spec.roots.isEmpty()) {
        spec.roots = QStringList{spec.origin};
    }
    return spec;
}

/// Lo que mostraba la pestaña cuando se lanzó la búsqueda.
inline QString searchRootOf(const QString &location)
{
    return searchSpecOf(location).origin;
}

/// Texto de la consulta de una ubicación de búsqueda.
inline QString searchQueryOf(const QString &location)
{
    return searchSpecOf(location).query;
}

/// true si no es una carpeta del disco.
inline bool isVirtualLocation(const QString &path)
{
    return isVolumesLocation(path) || isSearchLocation(path);
}

} // namespace eu::ui
