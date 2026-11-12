// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/tasks/Cancel.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace eu::trash {

/// Nombre de la carpeta de la papelera. Lleva punto inicial para quedar oculta
/// en Linux y macOS; en Windows se le pone además el atributo de oculto.
QString trashFolderName();

/// Un elemento que está en la papelera.
///
/// En disco ocupa dos cosas dentro de la carpeta de la papelera:
///   <id>/<nombre original>   el elemento tal cual estaba
///   <id>.info                 sus datos (JSON): de dónde vino, cuándo, tamaño
/// El archivo .info permite restaurar aunque se pierda cualquier base de datos.
struct TrashItem
{
    QString id;
    QString trashDirectory;   ///< Carpeta de la papelera que lo contiene.
    QString originalPath;     ///< Dónde estaba.
    qint64 deletedMs = 0;
    qint64 size = 0;          ///< Tamaño si era un archivo; 0 para carpetas.
    bool isDirectory = false;

    QString storedPath() const;   ///< Dónde está ahora el elemento.
    QString infoPath() const;     ///< Su archivo de datos.
    QString name() const;         ///< Nombre original, sin ruta.
};

/// Carpeta de papelera que corresponde a `path` (SPEC §10.2), creándola si no
/// existe. Siempre está en el mismo volumen que `path`:
///  1. `<raíz del volumen>/.PapeleraExploradorUltra` si se puede escribir ahí;
///  2. si no, `<carpeta personal>/.PapeleraExploradorUltra`, solo si la carpeta
///     personal está en ese mismo volumen;
///  3. si no hay dónde, error NotSupported: ese volumen no admite papelera.
core::Result<QString> trashDirectoryFor(const QString &path);

/// Paso 1 de enviar a la papelera: reserva el sitio y escribe el archivo .info.
/// No mueve nada todavía. Devuelve el elemento con su storedPath() de destino.
core::Result<TrashItem> prepareItem(const QString &path);

/// Paso 2: mueve el elemento a su sitio en la papelera. Es un renombrado dentro
/// del mismo volumen: rápido y atómico (SPEC §10.1).
core::Status moveIntoTrash(const TrashItem &item);

/// Atajo de los dos pasos (sin historial; `ops` usa los pasos por separado
/// para anotarlos).
core::Result<TrashItem> trashPath(const QString &path);

/// Deshace un prepareItem que no llegó a usarse, o limpia los restos de un
/// elemento que ya salió de la papelera.
void discardItemRecord(const TrashItem &item);

/// Elementos de una carpeta de papelera, del más reciente al más antiguo.
core::Result<QList<TrashItem>> listTrash(const QString &trashDirectory);

/// Devuelve el elemento a `targetPath` (por defecto, su ruta original). No
/// sobrescribe: si ahí hay algo, falla con AlreadyExists. Si la carpeta de
/// destino ya no existe, falla con NotFound.
core::Status restoreItem(const TrashItem &item, const QString &targetPath = {});

/// Elimina para siempre un elemento de la papelera. No sigue enlaces.
core::Status purgeItem(const TrashItem &item, const tasks::CancelToken &cancel);

/// Papeleras existentes en los volúmenes conectados ahora mismo.
QStringList existingTrashDirectories();

/// true si la ruta está dentro de una carpeta de papelera.
bool isInsideTrash(const QString &path);

/// Borra para siempre una carpeta y su contenido, sin seguir enlaces. Se
/// expone para la eliminación permanente de `ops`.
core::Status removeTreePermanently(const QString &path, const tasks::CancelToken &cancel);

} // namespace eu::trash
