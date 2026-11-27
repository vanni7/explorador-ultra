// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/fs/Volumes.h"
#include "eu/tasks/Scheduler.h"
#include "eu/ui/FileClipboard.h"

#include <QPersistentModelIndex>
#include <QTimer>
#include <QTreeView>

class QMimeData;
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
    /// Añade un lugar a "Favoritos" solo durante esta sesión (no se guarda).
    void addPlace(const QString &name, const QString &path);
    /// Agrega una carpeta a Favoritos y lo guarda en la configuración. false si
    /// ya era un favorito o no es una carpeta.
    bool addFavorite(const QString &path);
    /// Quita un favorito agregado por el usuario. Los de fábrica no se quitan.
    bool removeFavorite(const QString &path);
    /// true si `path` ya está en Favoritos (de fábrica o agregado).
    bool isFavorite(const QString &path) const;
    /// true si `path` es un favorito agregado por el usuario (se puede quitar).
    bool isCustomFavorite(const QString &path) const;
    /// Los favoritos agregados por el usuario, en su orden.
    QStringList customFavorites() const;
    /// Clave de la configuración donde se guardan los favoritos agregados.
    static QString favoritesSettingsKey();
    /// Vuelve a leer las subcarpetas de `path` donde ya se hayan leído (tras
    /// crear, mover o borrar carpetas), conservando lo desplegado. Si nunca
    /// se desplegó, solo se corrige si lleva flecha o no.
    void refreshFolder(const QString &path);
    /// true si la carpeta tiene alguna subcarpeta visible (para la flecha). Lee el disco.
    static bool hasSubfolders(const QString &path);
    /// Ruta del nodo que tiene el cursor del teclado, o vacía.
    QString currentPlace() const;
    /// Ruta del nodo con el cursor si es una subcarpeta desplegada, o vacía.
    /// Los lugares de primer nivel (favoritos y unidades) son accesos, no
    /// carpetas que se puedan cortar o borrar desde aquí: suprimir
    /// «Documentos» por error sería grave.
    QString currentSubfolder() const;
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
    /// Se agregó o se quitó un favorito.
    void favoritesChanged();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void buildFavorites();
    /// Lee de la configuración los favoritos agregados por el usuario.
    void loadCustomFavorites();
    void saveCustomFavorites() const;
    QStandardItem *makeFavoriteItem(const QString &path);
    /// Pone un favorito como disponible o no (su carpeta no existe: disco
    /// desconectado, carpeta movida…). No se quita: vuelve al reconectar.
    void setAvailable(QStandardItem *item, bool available);
    /// Carpetas de lo arrastrado que se pueden agregar a Favoritos.
    QStringList foldersToFavorite(const QMimeData *mime) const;
    /// true si el punto está sobre el rótulo "Favoritos". Soltar ahí agrega
    /// favoritos; soltar sobre un favorito copia o mueve dentro, como siempre.
    bool isOverFavoritesLabel(const QPoint &position) const;
    void rebuildVolumes(const QList<fs::Volume> &volumes);
    QStandardItem *makeFolderItem(const QString &text, const QString &path, const QIcon &icon,
                                  bool mayHaveChildren = true);
    /// `again`: aunque ya se haya leído (para poner el árbol al día).
    void loadChildren(QStandardItem *item, bool again = false);
    void applyChildren(const QString &path, const QStringList &folderNames, const QList<bool> &withSubfolders,
                       bool failed);
    /// Quita la flecha de los nodos de primer nivel que no tienen subcarpetas.
    void checkTopLevelArrows(QStandardItem *section);
    void setArrow(QStandardItem *item, bool hasChildren);
    void refreshFolderArrow(const QString &path, bool hasChildren);
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
    QTimer m_followTimer;    ///< Espera breve antes de abrir la carpeta a la que se llegó con las flechas.
};

} // namespace eu::ui
