// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/ui/Appearance.h"

#include <QDialog>

#include <functional>

class QCheckBox;
class QComboBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTreeWidget;

namespace eu::ui {

/// Preferencias (SPEC §5.2, §18, §20): apariencia (tema, colores, letra e
/// iconos), atajos de teclado con detección de conflictos, y exportar,
/// importar o volver a la configuración de fábrica. Todo se aplica al instante
/// y se guarda en la configuración.
class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    /// `actions`: las acciones con atajo que se pueden cambiar.
    PreferencesDialog(QList<QAction *> actions, QWidget *parent = nullptr);

    // Para las pruebas.
    QTabWidget *tabs() const { return m_tabs; }
    void setAppearance(const Appearance &appearance);
    /// Como elegirla en la página: se guarda, se aplica y se avisa.
    void chooseAppearance(const Appearance &appearance);
    Appearance appearance() const { return m_appearance; }
    /// Asigna `sequence` a `action` como si se hiciera a mano (con las mismas comprobaciones).
    bool assignShortcut(QAction *action, const QKeySequence &sequence);
    void restoreDefaultShortcuts();
    bool exportTo(const QString &file);
    bool importFrom(const QString &file);
    void restoreFactorySettings();
    /// Respuesta a «¿quitárselo a la otra acción?» sin ventana.
    void setConflictAnswerForTesting(std::function<bool(QAction *)> answer) { m_conflictAnswer = std::move(answer); }
    /// Respuesta a «¿restaurar todo?» sin ventana.
    void setConfirmForTesting(std::function<bool()> confirm) { m_confirm = std::move(confirm); }
    QLabel *shortcutMessage() const { return m_shortcutMessage; }

signals:
    /// Cambió la apariencia (ya está guardada): aplicarla.
    void appearanceChanged(const eu::ui::Appearance &appearance);
    /// Se importó o se restauró la configuración: volver a leerla entera.
    void settingsReplaced();

private:
    QWidget *makeAppearancePage();
    QWidget *makeShortcutsPage();
    QWidget *makeSettingsPage();
    void showAppearance();
    void appearanceEdited();
    void fillShortcuts();
    void onShortcutSelected();
    QAction *selectedAction() const;
    bool confirm(const QString &question);

    QList<QAction *> m_actions;
    Appearance m_appearance;
    bool m_updating = false;
    std::function<bool(QAction *)> m_conflictAnswer;
    std::function<bool()> m_confirm;

    QTabWidget *m_tabs = nullptr;
    QComboBox *m_theme = nullptr;
    QList<QPair<QPushButton *, QColor Appearance::*>> m_colorButtons;
    QSpinBox *m_fontSize = nullptr;
    QComboBox *m_iconSize = nullptr;
    QLineEdit *m_shortcutFilter = nullptr;
    QTreeWidget *m_shortcutList = nullptr;
    QKeySequenceEdit *m_sequenceEdit = nullptr;
    QPushButton *m_assignButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_defaultButton = nullptr;
    QLabel *m_shortcutMessage = nullptr;
};

} // namespace eu::ui
