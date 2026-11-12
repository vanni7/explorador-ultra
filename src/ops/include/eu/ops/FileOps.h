// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/tasks/Cancel.h"

#include <QString>

#include <functional>

namespace eu::ops {

/// Operaciones elementales sobre el disco. Son bloqueantes: se llaman desde
/// tareas del planificador. Ninguna sigue enlaces simbólicos ni sobrescribe
/// nada que ya exista (SPEC §8.3): si el destino existe, fallan con AlreadyExists.

/// Huella de un elemento, para comprobar más tarde si sigue igual.
struct FileStamp
{
    bool exists = false;
    bool isDirectory = false;   ///< Carpeta real (un enlace a carpeta no cuenta).
    bool isSymlink = false;
    qint64 size = -1;           ///< Solo para archivos normales.
    qint64 modifiedMs = 0;      ///< Solo para archivos normales.
};

FileStamp stampOf(const QString &path);

core::Status makeDirectory(const QString &path);
core::Status createEmptyFile(const QString &path);

/// Renombra o mueve dentro del mismo volumen, de forma atómica y sin
/// sobrescribir. Entre volúmenes falla con CrossDevice.
core::Status renameNoReplace(const QString &source, const QString &target);

/// Se llama con los bytes escritos desde el aviso anterior.
using CopyProgress = std::function<void(qint64 bytesJustWritten)>;

/// Sufijo del archivo temporal que usa copyFile mientras escribe.
QString partialSuffix();

/// Copia un archivo normal. Escribe en `<target>.eu-parcial`, sincroniza a
/// disco, copia permisos y fecha, y solo entonces lo renombra al nombre final:
/// así nunca queda un destino a medias con el nombre bueno (SPEC §8.3).
/// Si se cancela o falla, borra el temporal.
core::Status copyFile(const QString &source, const QString &target, const tasks::CancelToken &cancel,
                      const CopyProgress &progress = {});

/// Copia un enlace simbólico como enlace (no copia aquello a lo que apunta).
core::Status copySymlink(const QString &source, const QString &target);

/// Borra un archivo o un enlace (el enlace, no su destino). Definitivo.
core::Status removeFile(const QString &path);
/// Borra una carpeta vacía. Si tiene contenido falla con DirectoryNotEmpty.
core::Status removeEmptyDirectory(const QString &path);

/// Devuelve un nombre libre en `directory` a partir de `name`:
/// "foto.jpg" -> "foto (2).jpg", "foto (3).jpg"… Si `name` está libre, lo devuelve tal cual.
QString uniqueName(const QString &directory, const QString &name);

} // namespace eu::ops
