// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/Shortcuts.h"

#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>

namespace eu::ui {

namespace {

const char *defaultsProperty = "eu-default-shortcuts";

void collect(const QMenu *menu, const QString &menuName, QList<QAction *> &into)
{
    for (QAction *action : menu->actions()) {
        if (action->isSeparator()) {
            continue;
        }
        if (action->menu() != nullptr) {
            collect(action->menu(), menuName, into);
            continue;
        }
        if (action->text().isEmpty() || into.contains(action)) {
            continue;
        }
        action->setProperty("eu-menu", menuName);
        into.append(action);
    }
}

} // namespace

QString shortcutId(const QAction *action)
{
    QString id = action->text();
    id.remove(QLatin1Char('&'));
    id.remove(QStringLiteral("…"));
    id.remove(QStringLiteral("..."));
    return id.trimmed().toLower();
}

QList<QAction *> menuActions(const QMenuBar *bar)
{
    QList<QAction *> actions;
    for (const QAction *top : bar->actions()) {
        if (top->menu() != nullptr) {
            QString name = top->text();
            name.remove(QLatin1Char('&'));
            collect(top->menu(), name, actions);
        }
    }
    return actions;
}

void rememberDefaultShortcuts(const QList<QAction *> &actions)
{
    for (QAction *action : actions) {
        if (!action->property(defaultsProperty).isValid()) {
            action->setProperty(defaultsProperty, QVariant::fromValue(action->shortcuts()));
        }
    }
}

QList<QKeySequence> defaultShortcuts(const QAction *action)
{
    const QVariant stored = action->property(defaultsProperty);
    return stored.isValid() ? stored.value<QList<QKeySequence>>() : action->shortcuts();
}

void applyShortcuts(const QList<QAction *> &actions, const QSettings &settings)
{
    for (QAction *action : actions) {
        const QString key = QStringLiteral("atajos/") + shortcutId(action);
        if (settings.contains(key)) {
            const QString stored = settings.value(key).toString();
            action->setShortcuts(stored == QLatin1String("ninguno")
                                     ? QList<QKeySequence>()
                                     : QKeySequence::listFromString(stored, QKeySequence::PortableText));
        } else {
            action->setShortcuts(defaultShortcuts(action));
        }
    }
}

void saveShortcuts(QSettings &settings, const QAction *action)
{
    const QString key = QStringLiteral("atajos/") + shortcutId(action);
    if (action->shortcuts() == defaultShortcuts(action)) {
        settings.remove(key);
    } else if (action->shortcuts().isEmpty()) {
        settings.setValue(key, QStringLiteral("ninguno"));   // un valor vacío no se distingue bien de «no está»
    } else {
        settings.setValue(key, QKeySequence::listToString(action->shortcuts(), QKeySequence::PortableText));
    }
}

QAction *shortcutConflict(const QList<QAction *> &actions, const QKeySequence &sequence, const QAction *except)
{
    if (sequence.isEmpty()) {
        return nullptr;
    }
    for (QAction *action : actions) {
        if (action != except && action->shortcuts().contains(sequence)) {
            return action;
        }
    }
    return nullptr;
}

bool isReservedBySystem(const QKeySequence &sequence)
{
    if (sequence.isEmpty()) {
        return false;
    }
    static const QList<QKeySequence> reserved{
        QKeySequence(Qt::ALT | Qt::Key_Tab),
        QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Tab),
        QKeySequence(Qt::ALT | Qt::Key_F4),
        QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Delete),
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Escape),   // administrador de tareas de Windows
        QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T),          // terminal en Ubuntu
        QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_L),          // bloquear la sesión en Linux
        QKeySequence(Qt::META | Qt::Key_L),                    // bloquear en Windows
        QKeySequence(Qt::META | Qt::Key_D),
        QKeySequence(Qt::META | Qt::Key_E),
        QKeySequence(Qt::META | Qt::Key_Tab),
        QKeySequence(Qt::META | Qt::Key_Space),
        QKeySequence(Qt::CTRL | Qt::Key_Space),                // ⌘Espacio (Spotlight) en macOS
        QKeySequence(Qt::CTRL | Qt::Key_Tab),                  // en macOS ⌘Tab cambia de aplicación
    };
#ifdef Q_OS_MACOS
    return reserved.contains(sequence);
#else
    // En Windows y Linux Ctrl+Espacio y Ctrl+Tab son de la aplicación.
    return reserved.contains(sequence) && sequence != QKeySequence(Qt::CTRL | Qt::Key_Space)
        && sequence != QKeySequence(Qt::CTRL | Qt::Key_Tab);
#endif
}

} // namespace eu::ui
