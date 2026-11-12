// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace eu::core {

/// Reglas de nombres que se quieren respetar. Dependen del sistema de archivos
/// de destino, no solo del sistema operativo: un disco NTFS o exFAT montado en
/// Linux exige las reglas de Windows.
enum class NameRules {
    Posix,    ///< ext4, btrfs, APFS…: solo se prohíben '/' y el carácter nulo.
    MacOS,    ///< Como Posix, y además ':' (el Finder lo usa como separador).
    Windows,  ///< NTFS, exFAT, FAT32: caracteres y nombres reservados de Windows.
    Portable, ///< Las más estrictas: válido en cualquier sistema.
};

/// Reglas del sistema operativo en el que corre la aplicación.
NameRules nativeNameRules();

enum class NameProblem {
    None,
    Empty,              ///< Nombre vacío.
    DotOrDotDot,        ///< "." o "..".
    InvalidCharacter,   ///< Contiene un carácter prohibido.
    ReservedName,       ///< CON, NUL, COM1… (Windows).
    TrailingDotOrSpace, ///< Termina en punto o espacio (Windows los descarta).
    TooLong,            ///< Supera el límite de longitud de un componente.
};

struct NameCheck
{
    NameProblem problem = NameProblem::None;
    QChar character;   ///< El carácter prohibido, si problem == InvalidCharacter.

    bool ok() const { return problem == NameProblem::None; }
};

/// Comprueba un nombre de archivo o carpeta (un solo componente, sin ruta).
NameCheck checkFileName(const QString &name, NameRules rules);

/// Explicación para el usuario del problema encontrado.
QString describeNameProblem(const NameCheck &check);

} // namespace eu::core
