// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/SizeFormat.h"
#include "eu/trash/Trash.h"

#include <QDialog>

class QLabel;
class QPushButton;
class QTreeWidget;

namespace eu::ui {

class FileActions;

/// Ventana de la papelera: reúne la de todas las unidades conectadas (SPEC §10.3)
/// y permite restaurar, eliminar para siempre o vaciarla.
///
/// No modifica nada por sí misma: pide las acciones con sus señales y la
/// ventana principal las ejecuta (con la confirmación que corresponda).
class TrashDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TrashDialog(FileActions *actions, QWidget *parent = nullptr);

    void setSizeUnits(core::SizeUnits units);

    /// Lo que se está mostrando (para las pruebas).
    QList<trash::TrashItem> items() const { return m_items; }
    /// Selecciona los elementos cuya ubicación original está en `originalPaths`.
    void selectOriginalPaths(const QStringList &originalPaths);
    bool isLoading() const { return m_loading; }

public slots:
    /// Vuelve a leer la papelera.
    void reload();
    void restoreSelected();
    void purgeSelected();
    void emptyTrash();

signals:
    void restoreRequested(const QList<eu::trash::TrashItem> &items);
    /// `everything` es true cuando se pidió vaciar la papelera entera.
    void purgeRequested(const QList<eu::trash::TrashItem> &items, bool everything);

private:
    void showItems(const QList<trash::TrashItem> &items);
    QList<trash::TrashItem> selectedItems() const;
    void updateButtons();

    FileActions *m_actions = nullptr;
    QList<trash::TrashItem> m_items;
    core::SizeUnits m_sizeUnits = core::SizeUnits::Decimal;
    bool m_loading = false;

    QTreeWidget *m_list = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_restoreButton = nullptr;
    QPushButton *m_purgeButton = nullptr;
    QPushButton *m_emptyButton = nullptr;
};

} // namespace eu::ui
