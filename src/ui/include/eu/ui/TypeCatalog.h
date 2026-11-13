// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/fs/Entry.h"

#include <QHash>
#include <QIcon>
#include <QString>

namespace eu::ui {

/// Descripción ("Documento PDF") e icono de un elemento según su tipo.
///
/// Se deduce solo por el nombre, sin abrir el archivo: no hay E/S. El
/// resultado se guarda por extensión, así que una carpeta con 100 000 fotos
/// solo consulta el tipo una vez. Se usa desde el hilo de interfaz.
class TypeCatalog
{
public:
    struct Info
    {
        QString description;
        QIcon icon;
    };

    const Info &infoFor(const fs::Entry &entry) const;

private:
    mutable QHash<QString, Info> m_cache;
};

} // namespace eu::ui
