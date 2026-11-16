// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/ui/FileClipboard.h"

#include <QListView>
#include <QSet>
#include <QTimer>

#include <functional>

namespace eu::thumbs {
class ThumbnailService;
}

namespace eu::ui {

class DirectoryModel;

/// Vista de miniaturas de una carpeta (SPEC §5.5): una cuadrícula con la
/// imagen de cada archivo y su nombre debajo.
///
/// Muestra el mismo modelo que la vista detallada y, si se le da su modelo
/// de selección, la misma selección: cambiar de vista no pierde nada.
/// Las miniaturas se piden solo al pintar, es decir, solo de lo que se ve;
/// lo pedido que deja de verse al desplazarse se cancela.
class ThumbnailView : public QListView
{
    Q_OBJECT

public:
    ThumbnailView(DirectoryModel *model, QWidget *parent = nullptr);

    /// Sin servicio se ven los iconos del tipo, a tamaño grande.
    void setThumbnailService(thumbs::ThumbnailService *service);
    /// Lado de la miniatura en píxeles lógicos (96, 160, 256…).
    void setThumbnailSize(int size);
    int thumbnailSize() const { return m_size; }

    /// Rutas de los elementos que se ven ahora mismo.
    QSet<QString> visiblePaths() const;

    MoveByDefaultRule moveByDefault;
    std::function<void(const QStringList &, const QString &, bool)> onDrop;
    /// Se llama al empezar a renombrar en esta vista.
    std::function<void()> onEditingStarted;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    DropPlan planFor(const QDropEvent *event, int *targetRow) const;
    void setDropTarget(bool active, int row);
    void scheduleRetain();
    void updateGrid();

    DirectoryModel *m_model;
    thumbs::ThumbnailService *m_service = nullptr;
    int m_size = 160;
    bool m_dropActive = false;
    int m_dropRow = -1;
    QTimer m_retainTimer;   ///< Tras desplazarse, cancela lo que dejó de verse.
    QTimer m_repaintTimer;  ///< Junta los avisos de miniaturas listas en un repintado.
};

} // namespace eu::ui
