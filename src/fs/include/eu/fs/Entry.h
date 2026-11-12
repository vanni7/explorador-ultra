// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QMetaType>
#include <QString>

#include <cstdint>

namespace eu::fs {

/// Un elemento de una carpeta, con los datos que muestra la vista detallada.
/// Es deliberadamente pequeño: una carpeta puede tener cientos de miles.
struct Entry
{
    QString name;
    qint64 size = 0;             ///< Bytes. Para carpetas no es el tamaño del contenido.
    qint64 modifiedMs = 0;       ///< Milisegundos desde 1970 (UTC). 0 = desconocido.
    qint64 createdMs = 0;        ///< Ídem; 0 si el sistema de archivos no lo guarda.
    std::uint64_t fileId = 0;    ///< Inodo (POSIX). 0 = desconocido.
    std::uint64_t deviceId = 0;  ///< Volumen al que pertenece. 0 = desconocido.
    bool isDirectory = false;    ///< Para enlaces: si el destino es una carpeta.
    bool isSymlink = false;
    bool isBrokenSymlink = false;
    bool isHidden = false;
    bool statFailed = false;     ///< Se vio el nombre pero no se pudieron leer sus datos.
};

} // namespace eu::fs

Q_DECLARE_METATYPE(eu::fs::Entry)
