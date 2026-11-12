// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QStringList>

class QMimeData;

namespace eu::ui {

/// Archivos que hay en el portapapeles (o que se están arrastrando).
struct FileSelection
{
    QStringList paths;   ///< Solo rutas locales.
    bool cut = false;    ///< true si se "cortaron": al pegar se mueven.

    bool isEmpty() const { return paths.isEmpty(); }
};

/// Prepara los datos para Copiar o Cortar. Además de la lista estándar
/// (`text/uri-list`) escribe las marcas con las que cada escritorio distingue
/// cortar de copiar, para que pegar en Nautilus, Dolphin o el Explorador de
/// Windows haga lo mismo que aquí. Quien llama pasa a ser dueño del objeto.
QMimeData *makeFileMimeData(const QStringList &paths, bool cut);

/// Lee una lista de archivos puesta por esta u otra aplicación.
FileSelection fileSelectionFrom(const QMimeData *data);

} // namespace eu::ui
