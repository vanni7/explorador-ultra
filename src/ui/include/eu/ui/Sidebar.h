// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/fs/Volumes.h"
#include "eu/tasks/Scheduler.h"

#include <QTreeView>

class QStandardItem;
class QStandardItemModel;

namespace eu::ui {

/// Panel izquierdo: favoritos y unidades, con sus carpetas desplegables (SPEC §5.3).
///
/// Las subcarpetas se leen en segundo plano al desplegar cada nodo, de modo
/// que una unidad lenta o de red nunca congela la ventana.
class Sidebar : public QTreeView
{
    Q_OBJECT

public:
    Sidebar(tasks::Scheduler &scheduler, fs::VolumeMonitor &volumes, QWidget *parent = nullptr);

signals:
    /// El usuario eligió un lugar.
    void placeActivated(const QString &path);

private:
    void buildFavorites();
    void rebuildVolumes(const QList<fs::Volume> &volumes);
    QStandardItem *makeFolderItem(const QString &text, const QString &path, const QIcon &icon);
    void loadChildren(QStandardItem *item);
    void applyChildren(const QString &path, const QStringList &folderNames, bool failed);
    QStandardItem *findLoadingItem(const QString &path) const;

    tasks::Scheduler &m_scheduler;
    QStandardItemModel *m_model = nullptr;
    QStandardItem *m_favoritesSection = nullptr;
    QStandardItem *m_volumesSection = nullptr;
    QList<QStandardItem *> m_loadingItems;   ///< Nodos cuya lectura está en curso.
};

} // namespace eu::ui
