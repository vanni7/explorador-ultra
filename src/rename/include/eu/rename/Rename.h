// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/FileName.h"

#include <QHash>
#include <QList>
#include <QString>

#include <functional>

namespace eu::rename {

/// Un paso de la cadena de reglas (SPEC §11). Se aplican en orden, cada una
/// sobre lo que dejó la anterior.
struct Rule
{
    enum class Kind {
        Case,      ///< MAYÚSCULAS, minúsculas, Tipo Título, Tipo oración.
        Replace,   ///< Sustituir un texto por otro.
        Remove,    ///< Quitar caracteres: unos concretos, los primeros, los últimos o un tramo.
        Insert,    ///< Añadir texto al principio, al final o en una posición (prefijo y sufijo).
        Regex,     ///< Expresión regular con referencias ($1, \1…).
        Counter,   ///< Número con ceros a la izquierda, al principio o al final.
        Template,  ///< Nombre nuevo a partir de una plantilla con datos: {nombre}, {fecha:yyyy-MM-dd}…
    };
    enum class CaseMode { Upper, Lower, Title, Sentence };
    enum class RemoveMode { Characters, First, Last, Range };
    enum class Position { Start, End, At };

    Kind kind = Kind::Case;
    CaseMode caseMode = CaseMode::Lower;
    QString find;              ///< Replace: lo que se busca. Regex: el patrón.
    QString with;              ///< Replace y Regex: lo que se pone. Insert: el texto. Template: la plantilla.
    bool caseSensitive = false;
    RemoveMode removeMode = RemoveMode::Characters;
    QString characters;        ///< Remove/Characters: cada uno de estos caracteres se quita.
    int count = 1;             ///< Remove: cuántos. Counter: cifras mínimas.
    int from = 0;              ///< Remove/Range e Insert/At: posición (desde 0).
    Position position = Position::Start;
    int start = 1;             ///< Counter: primer número.
    int step = 1;              ///< Counter: incremento.
    QString separator = QStringLiteral(" ");   ///< Counter: entre el número y el nombre.
};

/// Qué hacer con la extensión.
enum class ExtensionMode {
    Keep,     ///< Las reglas no la tocan.
    Lower,    ///< Las reglas no la tocan, y se pasa a minúsculas.
    Upper,
    Include,  ///< Las reglas actúan sobre el nombre completo.
};

struct Options
{
    QList<Rule> rules;
    ExtensionMode extension = ExtensionMode::Keep;
};

/// Un elemento que se quiere renombrar.
struct Item
{
    QString path;
    bool isDirectory = false;
    qint64 modifiedMs = 0;
    qint64 createdMs = 0;
};

/// Datos de un archivo para la plantilla: "fecha_foto", "camara", "artista",
/// "album", "pista", "titulo", "año", "genero", "ancho", "alto"… Lo que falta
/// simplemente no está.
using Metadata = QHash<QString, QString>;

/// Los nombres de datos que entiende la plantilla, con su explicación, para
/// mostrarlos en la ventana.
QList<QPair<QString, QString>> templateTokens();

/// El nombre nuevo de `item` (posición `index` en la lista, para el contador).
QString proposeName(const Item &item, int index, const Options &options, const Metadata &metadata = {});

/// Una fila de la vista previa: "Nombre actual → Nombre propuesto".
struct Proposal
{
    enum class Status {
        Unchanged,  ///< Queda igual: no se toca.
        Ok,
        Conflict,   ///< Choca con otro propuesto o con algo que ya existe.
        Invalid,    ///< Nombre no válido en esa unidad.
        Protected,  ///< No se puede escribir en su carpeta.
    };
    QString path;
    QString oldName;
    QString newName;
    Status status = Status::Ok;
    QString problem;   ///< Explicación para el usuario (también avisos en filas Ok).

    bool willRename() const { return status == Status::Ok; }
};

struct PlanOptions
{
    /// Reglas de nombres de cada carpeta (un disco NTFS en Linux exige las de Windows).
    std::function<core::NameRules(const QString &directory)> rulesFor;
    /// Datos de cada archivo para la plantilla. Vacío: ninguno.
    std::function<Metadata(const Item &)> metadataFor;
};

/// La vista previa completa, con conflictos, nombres no válidos, carpetas
/// protegidas y choques que solo difieren en mayúsculas (que en Windows y
/// macOS son el mismo nombre). Los cambios en cadena o en ciclo (a→b, b→a)
/// no son conflicto: se resuelven al renombrar con nombres temporales.
/// Lee el disco (lo que existe y los permisos): se llama desde una tarea.
QList<Proposal> plan(const QList<Item> &items, const Options &options, const PlanOptions &planOptions = {});

} // namespace eu::rename
