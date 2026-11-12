// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QStringList>

#include <functional>

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

/// Regla para soltar sin teclas: recibe (un origen, carpeta de destino) y
/// devuelve true si debe moverse en vez de copiarse.
using MoveByDefaultRule = std::function<bool(const QString &, const QString &)>;

/// Qué hacer cuando se sueltan archivos sobre una carpeta.
struct DropPlan
{
    bool valid = false;   ///< false: no hay nada que hacer ahí (o no son archivos).
    bool move = false;
    QStringList paths;
    QString target;
};

/// Decide el resultado de soltar `data` sobre la carpeta `target`: con Ctrl se
/// copia, con Mayús se mueve y sin teclas decide `rule` (sin regla, se copia).
/// No es válido soltar una carpeta dentro de sí misma ni mover algo a la
/// carpeta donde ya está. No toca el disco.
DropPlan planDrop(const QMimeData *data, const QString &target, Qt::KeyboardModifiers modifiers,
                  const MoveByDefaultRule &rule);

} // namespace eu::ui
