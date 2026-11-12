// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/fs/Volumes.h"
#include "eu/tasks/Scheduler.h"
#include "eu/ui/FileClipboard.h"

#include <QPersistentModelIndex>
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

    /// Regla para soltar archivos sin teclas (ver BrowserPane).
    void setMoveByDefaultRule(MoveByDefaultRule rule) { m_moveByDefault = std::move(rule); }
    /// Carpeta que se está viendo en la pestaña activa: un clic sobre su nodo
    /// ya no navega, solo despliega o recoge.
    void setCurrentPath(const QString &path) { m_currentPath = path; }
    /// Ruta del nodo que hay en un punto del panel, o vacía (para las pruebas).
    QString pathAt(const QPoint &position) const;
    /// Posición del nodo de una ruta ya cargada, o un punto nulo si no está.
    QPoint positionOf(const QString &path) const;

signals:
    /// El usuario eligió un lugar.
    void placeActivated(const QString &path);
    /// Clic derecho (o tecla de menú) sobre una carpeta del árbol.
    void contextMenuRequested(const QString &path, const QPoint &globalPosition);
    /// Se soltaron archivos sobre una carpeta del árbol.
    void filesDropped(const QStringList &paths, const QString &destination, bool move);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

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

    void onClicked(const QModelIndex &index);
    DropPlan planFor(const QDropEvent *event) const;
    void setDropTarget(const QModelIndex &index);
    MoveByDefaultRule m_moveByDefault;
    QPersistentModelIndex m_dropIndex;   ///< Carpeta sobre la que se está arrastrando.
    QString m_currentPath;   ///< Carpeta que muestra la pestaña activa.
};

} // namespace eu::ui
