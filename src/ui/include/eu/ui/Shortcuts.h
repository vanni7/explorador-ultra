// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QKeySequence>
#include <QList>
#include <QString>

class QAction;
class QMenuBar;
class QSettings;

/// Atajos de teclado personalizables (SPEC §18). Lo cambiado se guarda en el
/// grupo «atajos» de la configuración, por el identificador de cada acción;
/// lo que no está guardado conserva el atajo de fábrica.
namespace eu::ui {

/// Identificador estable de una acción: su texto sin «&» ni «…», en minúsculas.
QString shortcutId(const QAction *action);

/// Las acciones de los menús (sin separadores ni submenús), en el orden en que
/// aparecen; cada una con el nombre de su menú en la propiedad «eu-menu».
QList<QAction *> menuActions(const QMenuBar *bar);

/// Anota en cada acción sus atajos de fábrica (solo la primera vez).
void rememberDefaultShortcuts(const QList<QAction *> &actions);
QList<QKeySequence> defaultShortcuts(const QAction *action);

/// Aplica los atajos guardados (y los de fábrica a lo que no lo está).
void applyShortcuts(const QList<QAction *> &actions, const QSettings &settings);
/// Guarda los atajos de una acción (si son los de fábrica, se quita lo guardado).
void saveShortcuts(QSettings &settings, const QAction *action);

/// Otra acción de la lista que ya usa `sequence`; nullptr si ninguna.
QAction *shortcutConflict(const QList<QAction *> &actions, const QKeySequence &sequence, const QAction *except);

/// true si el sistema se reserva la combinación (Alt+Tab, Alt+F4,
/// Ctrl+Alt+Supr, Ctrl+Alt+T en Ubuntu, ⌘Tab, ⌘Espacio…): no se debe usar.
bool isReservedBySystem(const QKeySequence &sequence);

} // namespace eu::ui
