// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/ThumbnailView.h"

#include "eu/thumbs/ThumbnailService.h"
#include "eu/ui/DirectoryModel.h"
#include "eu/ui/EntrySort.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QLineEdit>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>

#include <cmath>

namespace eu::ui {

namespace {

constexpr int padding = 6;

/// Tamaño de la miniatura que se pide, en píxeles físicos. Se redondea a unos
/// pocos escalones para que cambiar el tamaño de la vista reutilice la caché.
int requestedSide(int logicalSize, qreal devicePixelRatio)
{
    const int wanted = int(std::ceil(logicalSize * devicePixelRatio));
    for (const int step : {128, 256, 512}) {
        if (wanted <= step) {
            return step;
        }
    }
    return 512;
}

/// Longitud del nombre sin la extensión (lo que se selecciona al renombrar).
qsizetype stemOf(const QString &name)
{
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    return dot > 0 ? dot : name.size();
}

/// Pinta cada elemento: la miniatura (o el icono del tipo) arriba y el
/// nombre, en dos líneas como mucho, debajo.
class ThumbnailDelegate : public QStyledItemDelegate
{
public:
    ThumbnailDelegate(ThumbnailView *view, DirectoryModel *model)
        : QStyledItemDelegate(view), m_view(view), m_model(model)
    {
    }

    thumbs::ThumbnailService *service = nullptr;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        const int lines = 2 * option.fontMetrics.height();
        return QSize(m_view->thumbnailSize() + 2 * padding, m_view->thumbnailSize() + 3 * padding + lines);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        // Fondo: selección, foco y paso del ratón, como cualquier elemento.
        opt.text.clear();
        opt.icon = QIcon();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);

        const int size = m_view->thumbnailSize();
        const QRect pictureArea(opt.rect.left() + (opt.rect.width() - size) / 2, opt.rect.top() + padding, size, size);
        const int row = index.row();
        bool drawn = false;
        if (service != nullptr && row >= 0 && row < m_model->rowCount()) {
            const fs::Entry &entry = m_model->entryAt(row);
            if (!entry.isDirectory) {
                thumbs::ThumbnailRequest request;
                request.path = m_model->filePath(row);
                request.size = entry.size;
                request.modifiedMs = entry.modifiedMs;
                request.maxSide = requestedSide(size, painter->device()->devicePixelRatioF());
                const thumbs::ThumbnailCache::Entry thumbnail = service->request(request);
                if (thumbnail.state == thumbs::ThumbnailCache::State::Ready && !thumbnail.image.isNull()) {
                    QSize shown = thumbnail.image.size();
                    shown.scale(pictureArea.size(), Qt::KeepAspectRatio);
                    if (shown.width() > thumbnail.image.width()) {
                        shown = thumbnail.image.size();   // no se agranda una imagen pequeña
                    }
                    const QRect target(pictureArea.left() + (pictureArea.width() - shown.width()) / 2,
                                       pictureArea.bottom() + 1 - shown.height(), shown.width(), shown.height());
                    painter->save();
                    painter->setRenderHint(QPainter::SmoothPixmapTransform);
                    painter->drawImage(target, thumbnail.image);
                    painter->restore();
                    drawn = true;
                }
            }
        }
        if (!drawn) {
            // Mientras llega, o si no hay miniatura: el icono del tipo, grande.
            const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
            const int iconSide = size * 2 / 3;
            const QRect iconRect(pictureArea.left() + (size - iconSide) / 2,
                                 pictureArea.top() + (size - iconSide) / 2, iconSide, iconSide);
            icon.paint(painter, iconRect, Qt::AlignCenter,
                       (opt.state & QStyle::State_Enabled) ? QIcon::Normal : QIcon::Disabled);
        }

        // Nombre: centrado, dos líneas como mucho, recortado por el medio.
        const QRect textRect(opt.rect.left() + 2, pictureArea.bottom() + padding, opt.rect.width() - 4,
                             2 * opt.fontMetrics.height());
        const QString name = index.data(Qt::DisplayRole).toString();
        const QString shown = fitInTwoLines(name, opt.fontMetrics, textRect.width());
        const QVariant foreground = index.data(Qt::ForegroundRole);
        QColor color = (opt.state & QStyle::State_Selected) ? opt.palette.color(QPalette::HighlightedText)
                                                           : opt.palette.color(QPalette::Text);
        if (foreground.canConvert<QBrush>() && !(opt.state & QStyle::State_Selected)) {
            color = foreground.value<QBrush>().color();
        }
        painter->save();
        painter->setPen(color);
        painter->setFont(opt.font);
        painter->drawText(textRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWrapAnywhere, shown);
        painter->restore();
    }

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QWidget *editor = QStyledItemDelegate::createEditor(parent, option, index);
        if (m_view->onEditingStarted) {
            m_view->onEditingStarted();
        }
        if (auto *lineEdit = qobject_cast<QLineEdit *>(editor)) {
            const fs::Entry &entry = m_model->entryAt(index.row());
            const qsizetype length = entry.isDirectory ? entry.name.size() : stemOf(entry.name);
            lineEdit->setAlignment(Qt::AlignHCenter);
            // Como en la vista detallada: se selecciona el nombre sin la extensión.
            QTimer::singleShot(0, lineEdit, [lineEdit, length] { lineEdit->setSelection(0, int(length)); });
        }
        return editor;
    }

    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        // El cuadro de texto va donde está el nombre, no encima de la imagen.
        const int height = editor->sizeHint().height();
        editor->setGeometry(option.rect.left(), option.rect.top() + m_view->thumbnailSize() + 2 * padding,
                            option.rect.width(), height);
    }

private:
    static QString fitInTwoLines(const QString &name, const QFontMetrics &metrics, int width)
    {
        if (metrics.horizontalAdvance(name) <= width) {
            return name;
        }
        // Lo que cabe en la primera línea, y el resto recortado por el medio
        // en la segunda (así se ve el final del nombre, con la extensión).
        int cut = int(name.size());
        while (cut > 1 && metrics.horizontalAdvance(name.left(cut)) > width) {
            --cut;
        }
        const QString second = metrics.elidedText(name.mid(cut), Qt::ElideMiddle, width);
        return name.left(cut) + QChar(QChar::LineSeparator) + second;
    }

    ThumbnailView *m_view;
    DirectoryModel *m_model;
};

} // namespace

ThumbnailView::ThumbnailView(DirectoryModel *model, QWidget *parent) : QListView(parent), m_model(model)
{
    setViewMode(QListView::IconMode);
    setMovement(QListView::Static);         // los elementos no se recolocan a mano
    setResizeMode(QListView::Adjust);       // se reparten al cambiar el ancho
    setUniformItemSizes(true);
    setWrapping(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setSelectionRectVisible(true);
    // Carpetas enormes: la cuadrícula se calcula a tandas y la ventana responde.
    setLayoutMode(QListView::Batched);
    setBatchSize(2000);
    setEditTriggers(QAbstractItemView::EditKeyPressed);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDropIndicatorShown(false);
    setAccessibleName(tr("Miniaturas de la carpeta"));
    setModel(model);
    setItemDelegate(new ThumbnailDelegate(this, model));
    updateGrid();

    m_retainTimer.setSingleShot(true);
    m_retainTimer.setInterval(150);
    connect(&m_retainTimer, &QTimer::timeout, this, [this] {
        if (m_service != nullptr && isVisible()) {
            m_service->retainOnly(visiblePaths());
        }
    });
    m_repaintTimer.setSingleShot(true);
    m_repaintTimer.setInterval(40);
    connect(&m_repaintTimer, &QTimer::timeout, viewport(), qOverload<>(&QWidget::update));
}

void ThumbnailView::setThumbnailService(thumbs::ThumbnailService *service)
{
    if (m_service != nullptr) {
        disconnect(m_service, nullptr, this, nullptr);
    }
    m_service = service;
    static_cast<ThumbnailDelegate *>(itemDelegate())->service = service;
    if (service != nullptr) {
        connect(service, &thumbs::ThumbnailService::ready, this, [this] {
            if (isVisible() && !m_repaintTimer.isActive()) {
                m_repaintTimer.start();
            }
        });
    }
    viewport()->update();
}

void ThumbnailView::setThumbnailSize(int size)
{
    size = qBound(48, size, 512);
    if (size == m_size) {
        return;
    }
    m_size = size;
    updateGrid();
}

void ThumbnailView::updateGrid()
{
    const int lines = 2 * fontMetrics().height();
    setGridSize(QSize(m_size + 2 * padding + 8, m_size + 3 * padding + lines + 8));
    setIconSize(QSize(m_size, m_size));
    doItemsLayout();
    viewport()->update();
}

QSet<QString> ThumbnailView::visiblePaths() const
{
    // Se toca la cuadrícula en el centro de cada celda visible: con elementos
    // del mismo tamaño basta, y no hay que recorrer toda la carpeta.
    QSet<QString> paths;
    const QSize grid = gridSize();
    if (grid.isEmpty() || m_model->rowCount() == 0) {
        return paths;
    }
    const QRect area = viewport()->rect();
    for (int y = grid.height() / 4; y < area.height() + grid.height(); y += grid.height() / 2) {
        for (int x = grid.width() / 4; x < area.width(); x += grid.width() / 2) {
            const QModelIndex index = indexAt(QPoint(x, std::min(y, area.height() - 1)));
            if (index.isValid() && index.row() < m_model->rowCount()) {
                paths.insert(m_model->filePath(index.row()));
            }
        }
    }
    return paths;
}

void ThumbnailView::scheduleRetain()
{
    if (m_service != nullptr) {
        m_retainTimer.start();
    }
}

void ThumbnailView::scrollContentsBy(int dx, int dy)
{
    QListView::scrollContentsBy(dx, dy);
    scheduleRetain();
}

void ThumbnailView::resizeEvent(QResizeEvent *event)
{
    QListView::resizeEvent(event);
    scheduleRetain();
}

DropPlan ThumbnailView::planFor(const QDropEvent *event, int *targetRow) const
{
    if (m_model->path().isEmpty() || m_model->state() != DirectoryModel::State::Ready) {
        return {};
    }
    QString target = m_model->path();
    const QModelIndex index = indexAt(event->position().toPoint());
    if (index.isValid() && m_model->entryAt(index.row()).isDirectory) {
        *targetRow = index.row();
        target = m_model->filePath(index.row());
    }
    return planDrop(event->mimeData(), target, event->modifiers(), moveByDefault);
}

void ThumbnailView::setDropTarget(bool active, int row)
{
    if (m_dropActive != active || m_dropRow != row) {
        m_dropActive = active;
        m_dropRow = row;
        viewport()->update();
    }
}

void ThumbnailView::dragEnterEvent(QDragEnterEvent *event)
{
    if (fileSelectionFrom(event->mimeData()).isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
}

void ThumbnailView::dragMoveEvent(QDragMoveEvent *event)
{
    int targetRow = -1;
    const DropPlan plan = planFor(event, &targetRow);
    setDropTarget(plan.valid, targetRow);
    if (!plan.valid) {
        event->ignore();
        return;
    }
    event->setDropAction(plan.move ? Qt::MoveAction : Qt::CopyAction);
    event->accept();
}

void ThumbnailView::dragLeaveEvent(QDragLeaveEvent *event)
{
    setDropTarget(false, -1);
    event->accept();
}

void ThumbnailView::dropEvent(QDropEvent *event)
{
    int targetRow = -1;
    const DropPlan plan = planFor(event, &targetRow);
    setDropTarget(false, -1);
    if (!plan.valid) {
        event->ignore();
        return;
    }
    // Igual que en la vista detallada: al origen se le contesta "copiado".
    event->setDropAction(Qt::CopyAction);
    event->accept();
    if (onDrop) {
        onDrop(plan.paths, plan.target, plan.move);
    }
}

void ThumbnailView::paintEvent(QPaintEvent *event)
{
    QListView::paintEvent(event);
    if (!m_dropActive) {
        return;
    }
    QRect frame = viewport()->rect();
    if (m_dropRow >= 0 && m_dropRow < m_model->rowCount()) {
        frame = visualRect(m_model->index(m_dropRow, 0));
    }
    QPainter painter(viewport());
    painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
    painter.drawRect(frame.adjusted(1, 1, -2, -2));
}

} // namespace eu::ui
