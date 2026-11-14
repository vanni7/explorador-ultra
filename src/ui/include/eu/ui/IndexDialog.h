// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/fs/Volumes.h"
#include "eu/index/IndexService.h"

#include <QDialog>

#include <functional>

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace eu::ui {

/// Texto corto del estado de un índice ("Al día", "Indexando… 1.200 carpetas").
QString indexStateText(const index::RootStatus &status);

/// Ventana "Índice de búsqueda": qué unidades se indexan y cómo va cada una
/// (SPEC §6.4). Marcar una unidad empieza a indexarla; desmarcarla borra su índice.
class IndexDialog : public QDialog
{
    Q_OBJECT

public:
    using VolumeSource = std::function<QList<fs::Volume>()>;

    IndexDialog(index::IndexService &service, VolumeSource volumes, QWidget *parent = nullptr);

    QTreeWidget *list() const { return m_list; }
    /// Marca o desmarca una unidad, como haría el usuario.
    void setIndexed(const QString &root, bool indexed);

signals:
    /// Cambió la lista de unidades indexadas (para guardarla).
    void rootsChanged();

private:
    void rebuild();
    void updateRows();
    QTreeWidgetItem *itemFor(const QString &root) const;
    QString selectedRoot() const;

    index::IndexService &m_service;
    VolumeSource m_volumes;
    QTreeWidget *m_list = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_rebuildButton = nullptr;
    bool m_updating = false;
};

} // namespace eu::ui
