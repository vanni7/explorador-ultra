// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/fs/Entry.h"

#include <QString>
#include <QStringList>
#include <QStringView>

#include <memory>

namespace eu::search {

/// Los datos de un elemento que una consulta puede examinar.
struct Item
{
    QStringView name;
    qint64 size = 0;
    qint64 modifiedMs = 0;   ///< 0 = desconocida.
    qint64 createdMs = 0;    ///< 0 = desconocida.
    bool isDirectory = false;
    bool isHidden = false;
};

Item itemFrom(const fs::Entry &entry);

/// Da el texto de un archivo a los filtros de contenido. Solo se le pide
/// cuando la consulta lo necesita y el resto de condiciones ya se cumple:
/// leer el archivo es, con diferencia, lo más caro.
class ContentSource
{
public:
    virtual ~ContentSource() = default;
    /// El texto del archivo, o nullptr si no se puede leer como texto.
    virtual const QString *text() = 0;
};

struct QueryOptions
{
    /// Distinguir mayúsculas de minúsculas (SPEC §6.1: desactivado por defecto).
    bool caseSensitive = false;
    /// Los términos sueltos se buscan en el contenido del archivo en vez de
    /// en el nombre. Los filtros (`ext:`, `name:`, `size:`…) no cambian.
    bool termsInContent = false;
    /// "Ahora", para resolver `hoy` y `ayer`. 0 = el momento de analizar la
    /// consulta. Las pruebas ponen una fecha fija.
    qint64 nowMs = 0;
};

/// Una consulta de búsqueda ya analizada. La gramática completa está en
/// docs/SEARCH_SYNTAX.md. Resumen:
///
///     fot*.jpg                    comodines: * y ?
///     factura 2025                dos términos: deben estar los dos (AND)
///     walter riso*                con comodines también vale la frase entera
///     informe OR acta             operadores AND, OR, NOT y paréntesis
///     ext:pdf size:>10MB          filtros
///     regex:/^IMG_\d+/            expresión regular sobre el nombre
///
/// Evaluar una consulta no toca el disco y se puede hacer desde cualquier
/// hilo: el objeto no cambia después de crearse y copiarlo es barato.
class Query
{
public:
    /// Consulta vacía: la cumplen todos los elementos.
    Query();

    /// Analiza el texto. Si tiene un error de sintaxis devuelve un Error cuyo
    /// mensaje dice qué falla y en qué posición.
    static core::Result<Query> parse(const QString &text, const QueryOptions &options = {});

    bool isEmpty() const { return m_root == nullptr; }
    /// `content` puede ser nullptr: entonces ningún elemento cumple una
    /// condición sobre el contenido.
    bool matches(const Item &item, ContentSource *content = nullptr) const;
    bool matches(const fs::Entry &entry) const { return matches(itemFrom(entry)); }

    /// true si la consulta mira dentro de los archivos (`content:` o términos
    /// con `termsInContent`): hay que evaluarla con un ContentSource.
    bool usesContent() const { return m_usesContent; }

    /// Filtros que aparecen en la consulta y esta versión todavía no sabe
    /// evaluar (`tag:`). Un elemento nunca los cumple; la
    /// interfaz debe avisar en vez de aparentar que se aplicaron.
    QStringList unavailableFilters() const { return m_unavailable; }

    QString text() const { return m_text; }

    struct Node;

private:
    std::shared_ptr<const Node> m_root;
    QStringList m_unavailable;
    QString m_text;
    bool m_usesContent = false;
};

/// Interpreta un tamaño escrito por el usuario: "10MB", "1,5 GiB", "300".
/// Unidades decimales (kB, MB, GB, TB = 1000ⁿ) y binarias (KiB, MiB… = 1024ⁿ);
/// sin unidad son bytes. `unitBytes` recibe el valor de una unidad.
/// Devuelve -1 si no se entiende.
qint64 parseSize(QStringView text, qint64 *unitBytes = nullptr);

} // namespace eu::search
