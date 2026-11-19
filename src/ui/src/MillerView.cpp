// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/MillerView.h"

#include "eu/ui/BrowserPane.h"
#include "eu/ui/DirectoryModel.h"

#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTimer>

#include <algorithm>

namespace eu::ui {

namespace {

constexpr int gripWidth = 5;
constexpr int minimumColumnWidth = 120;
constexpr int maximumColumnWidth = 1000;

/// Pinta el nombre y, en las carpetas, «›» a la derecha (como Finder): así se
/// sabe qué se abre en otra columna.
class MillerDelegate : public QStyledItemDelegate
{
public:
    explicit MillerDelegate(MillerColumn *column) : QStyledItemDelegate(column), m_column(column) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const bool folder = isFolder(index);
        QStyleOptionViewItem item(option);
        if (folder) {
            // El fondo de la fila entero; el nombre, sin pisar la flecha.
            QStyleOptionViewItem background(option);
            initStyleOption(&background, index);
            background.text.clear();
            background.icon = QIcon();
            const QWidget *widget = option.widget;
            QStyle *style = widget != nullptr ? widget->style() : QApplication::style();
            style->drawPrimitive(QStyle::PE_PanelItemViewItem, &background, painter, widget);
            item.rect.setRight(item.rect.right() - arrowSpace(option));
        }
        QStyledItemDelegate::paint(painter, item, index);
        if (!folder) {
            return;
        }
        painter->save();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const QPalette::ColorGroup group =
            option.state.testFlag(QStyle::State_Active) ? QPalette::Active : QPalette::Inactive;
        painter->setPen(option.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text));
        QRect arrow = option.rect;
        arrow.setLeft(arrow.right() - arrowSpace(option));
        painter->drawText(arrow, Qt::AlignCenter, QStringLiteral("›"));
        painter->restore();
    }

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (m_column->onEditingStarted) {
            m_column->onEditingStarted();
        }
        return QStyledItemDelegate::createEditor(parent, option, index);
    }

private:
    static int arrowSpace(const QStyleOptionViewItem &option) { return option.fontMetrics.height(); }

    static bool isFolder(const QModelIndex &index)
    {
        const auto *model = qobject_cast<const DirectoryModel *>(index.model());
        return model != nullptr && index.row() < model->rowCount() && model->entryAt(index.row()).isDirectory;
    }

    MillerColumn *m_column;
};

} // namespace

// ------------------------------------------------------------- MillerColumn

MillerColumn::MillerColumn(DirectoryModel *model, QWidget *parent) : QListView(parent), m_model(model)
{
    setModel(model);
    setModelColumn(int(Column::Name));
    setUniformItemSizes(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setTextElideMode(Qt::ElideMiddle);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setItemDelegate(new MillerDelegate(this));
    setDragEnabled(true);
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDropIndicatorShown(false);   // el destino se marca con un recuadro
}

DropPlan MillerColumn::planFor(const QDropEvent *event, int *targetRow) const
{
    if (m_model->path().isEmpty() || m_model->state() != DirectoryModel::State::Ready
        || isArchiveLocation(m_model->path())) {
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

void MillerColumn::setDropTarget(bool active, int row)
{
    if (m_dropActive != active || m_dropRow != row) {
        m_dropActive = active;
        m_dropRow = row;
        viewport()->update();
    }
}

void MillerColumn::dragEnterEvent(QDragEnterEvent *event)
{
    if (fileSelectionFrom(event->mimeData()).isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
}

void MillerColumn::dragMoveEvent(QDragMoveEvent *event)
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

void MillerColumn::dragLeaveEvent(QDragLeaveEvent *event)
{
    setDropTarget(false, -1);
    event->accept();
}

void MillerColumn::dropEvent(QDropEvent *event)
{
    int targetRow = -1;
    const DropPlan plan = planFor(event, &targetRow);
    setDropTarget(false, -1);
    if (!plan.valid) {
        event->ignore();
        return;
    }
    // Como en las otras vistas: al origen se le contesta "copiado".
    event->setDropAction(Qt::CopyAction);
    event->accept();
    if (onDrop) {
        onDrop(plan.paths, plan.target, plan.move);
    }
}

void MillerColumn::paintEvent(QPaintEvent *event)
{
    QListView::paintEvent(event);
    if (!m_dropActive) {
        return;
    }
    QRect frame = viewport()->rect();
    if (m_dropRow >= 0 && m_dropRow < m_model->rowCount()) {
        frame = visualRect(m_model->index(m_dropRow, int(Column::Name)));
    }
    QPainter painter(viewport());
    painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
    painter.drawRect(frame.adjusted(1, 1, -2, -2));
}

// --------------------------------------------------------------- MillerView

MillerView::MillerView(tasks::Scheduler &scheduler, DirectoryModel *currentModel, QItemSelectionModel *selection,
                       QWidget *parent)
    : QScrollArea(parent), m_scheduler(scheduler), m_currentModel(currentModel)
{
    setWidgetResizable(true);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);   // cada columna se desplaza sola
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_strip = new QWidget;
    m_layout = new QHBoxLayout(m_strip);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    setWidget(m_strip);

    m_current = new MillerColumn(currentModel, m_strip);
    {
        QItemSelectionModel *own = m_current->selectionModel();
        m_current->setSelectionModel(selection);
        delete own;
    }
    m_current->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_current->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_current->setAccessibleName(tr("Contenido de la carpeta"));
    m_current->onDrop = [this](const QStringList &paths, const QString &target, bool move) {
        if (onDrop) {
            onDrop(paths, target, move);
        }
    };
    m_currentGrip = makeGrip(m_current);

    // La columna de la derecha espera un momento: al recorrer carpetas con
    // las flechas no se lee cada una por la que se pasa.
    m_childTimer.setSingleShot(true);
    m_childTimer.setInterval(120);
    connect(&m_childTimer, &QTimer::timeout, this, [this] {
        if (!m_childFolder.isEmpty()) {
            ensureChildColumn();
            m_child.path = m_childFolder;
            m_child.model->setPath(m_childFolder);
            relayout();
            revealEnd();
        }
    });
    relayout();
}

QWidget *MillerView::makeGrip(MillerColumn *column)
{
    auto *grip = new QFrame(m_strip);
    grip->setFrameShape(QFrame::VLine);
    grip->setFrameShadow(QFrame::Plain);
    grip->setFixedWidth(gripWidth);
    grip->setCursor(Qt::SplitHCursor);
    grip->setToolTip(tr("Arrastra para cambiar el ancho; doble clic para ajustarlo al contenido"));
    grip->installEventFilter(this);
    m_gripColumns.insert(grip, column);
    return grip;
}

MillerView::Level MillerView::makeColumn(const QString &path, const QString &selectedName)
{
    Level column;
    column.path = path;
    column.selectedName = selectedName;
    column.model = new DirectoryModel(m_scheduler, this);
    column.model->setShowHidden(m_showHidden);
    column.view = new MillerColumn(column.model, m_strip);
    column.view->setSelectionMode(QAbstractItemView::SingleSelection);
    column.view->setFocusPolicy(Qt::ClickFocus);
    if (m_iconSize > 0) {
        column.view->setIconSize(QSize(m_iconSize, m_iconSize));
    }
    column.view->moveByDefault = m_moveByDefault;
    column.view->setAccessibleName(tr("Carpeta %1").arg(QDir::toNativeSeparators(path)));
    column.view->onDrop = m_current->onDrop;
    column.grip = makeGrip(column.view);
    MillerColumn *view = column.view;
    // Elegir algo en una columna que no es la actual es ir a su carpeta con
    // eso seleccionado.
    const auto choose = [this, view](const QModelIndex &index) {
        DirectoryModel *model = view->directoryModel();
        if (index.isValid() && index.row() < model->rowCount()) {
            emit navigateRequested(model->path(), model->entryAt(index.row()).name);
        }
    };
    connect(view, &QAbstractItemView::clicked, this, choose);
    connect(view, &QAbstractItemView::activated, this, choose);
    connect(column.model, &DirectoryModel::stateChanged, this, [this, view](DirectoryModel::State state) {
        if (state != DirectoryModel::State::Ready) {
            return;
        }
        for (Level &each : m_ancestors) {
            if (each.view == view) {
                markSelected(each);
            }
        }
    });
    column.model->setPath(path);
    return column;
}

void MillerView::destroyColumn(Level &column)
{
    if (column.grip != nullptr) {
        m_gripColumns.remove(column.grip);
        column.grip->deleteLater();
    }
    if (column.view != nullptr) {
        column.view->deleteLater();
    }
    if (column.model != nullptr) {
        column.model->deleteLater();
    }
    column = Level();
}

void MillerView::ensureChildColumn()
{
    if (m_child.view == nullptr) {
        m_child = makeColumn(QString(), QString());
    }
}

void MillerView::markSelected(Level &column)
{
    if (column.model->state() != DirectoryModel::State::Ready || column.selectedName.isEmpty()) {
        return;
    }
    const int row = column.model->rowOfName(column.selectedName);
    if (row < 0) {
        return;
    }
    const QModelIndex index = column.model->index(row, int(Column::Name));
    column.view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect
                                                              | QItemSelectionModel::Rows);
    column.view->scrollTo(index, QAbstractItemView::PositionAtCenter);
}

void MillerView::setPath(const QString &path)
{
    m_path = path;
    // Las carpetas de la ruta, de la raíz hacia abajo, sin la actual.
    QStringList chain;
    for (QString up = parentPathOf(path); !up.isEmpty(); up = parentPathOf(up)) {
        chain.prepend(up);
    }
    qsizetype kept = 0;
    while (kept < m_ancestors.size() && kept < chain.size() && m_ancestors.at(kept).path == chain.at(kept)) {
        ++kept;
    }
    while (m_ancestors.size() > kept) {
        destroyColumn(m_ancestors.last());
        m_ancestors.removeLast();
    }
    for (qsizetype i = 0; i < chain.size(); ++i) {
        const QString next = i + 1 < chain.size() ? chain.at(i + 1) : path;
        const QString name = QFileInfo(next).fileName();
        if (i < kept) {
            Level &column = m_ancestors[i];
            if (column.selectedName != name) {
                column.selectedName = name;
                markSelected(column);
            }
        } else {
            m_ancestors.append(makeColumn(chain.at(i), name));
        }
    }
    // Lo de la derecha era de la carpeta anterior.
    showChild(QString());
    relayout();
    revealEnd();
}

void MillerView::showChild(const QString &folder)
{
    if (folder == m_childFolder) {
        return;
    }
    m_childFolder = folder;
    m_childTimer.stop();
    if (m_child.view != nullptr) {
        m_child.model->clear();
        m_child.path.clear();
    }
    if (folder.isEmpty()) {
        relayout();
        return;
    }
    m_childTimer.start();
}

void MillerView::setShowHidden(bool show)
{
    m_showHidden = show;
    for (Level &column : m_ancestors) {
        column.model->setShowHidden(show);
    }
    if (m_child.model != nullptr) {
        m_child.model->setShowHidden(show);
    }
}

void MillerView::setIconSize(int size)
{
    m_iconSize = size;
    const QSize iconSize(size, size);
    m_current->setIconSize(iconSize);
    for (Level &column : m_ancestors) {
        column.view->setIconSize(iconSize);
    }
    if (m_child.view != nullptr) {
        m_child.view->setIconSize(iconSize);
    }
}

void MillerView::setMoveByDefaultRule(MoveByDefaultRule rule)
{
    m_moveByDefault = std::move(rule);
    m_current->moveByDefault = m_moveByDefault;
    for (Level &column : m_ancestors) {
        column.view->moveByDefault = m_moveByDefault;
    }
    if (m_child.view != nullptr) {
        m_child.view->moveByDefault = m_moveByDefault;
    }
}

int MillerView::widthFor(const QString &path) const
{
    return m_widths.value(path, defaultColumnWidth);
}

void MillerView::relayout()
{
    while (QLayoutItem *item = m_layout->takeAt(0)) {
        delete item;
    }
    int total = 0;
    const auto add = [this, &total](MillerColumn *view, QWidget *grip, const QString &path) {
        const int width = widthFor(path);
        view->setFixedWidth(width);
        m_layout->addWidget(view);
        m_layout->addWidget(grip);
        view->show();
        grip->show();
        total += width + gripWidth;
    };
    for (const Level &column : std::as_const(m_ancestors)) {
        add(column.view, column.grip, column.path);
    }
    add(m_current, m_currentGrip, m_path);
    const bool childShown = m_child.view != nullptr && !m_child.path.isEmpty() && m_child.path == m_childFolder;
    if (childShown) {
        add(m_child.view, m_child.grip, m_child.path);
    } else if (m_child.view != nullptr) {
        m_child.view->hide();
        m_child.grip->hide();
    }
    m_layout->addStretch(1);
    m_strip->setMinimumWidth(total);
}

void MillerView::revealEnd()
{
    // Tras colocar las columnas: que se vean la actual y la de la derecha.
    QTimer::singleShot(0, this, [this] {
        const bool childShown = m_child.view != nullptr && m_child.view->isVisible();
        ensureWidgetVisible(childShown ? static_cast<QWidget *>(m_child.view) : m_current, 0, 0);
        if (childShown) {
            ensureWidgetVisible(m_current, 0, 0);
        }
    });
}

bool MillerView::eventFilter(QObject *watched, QEvent *event)
{
    const auto grip = m_gripColumns.constFind(watched);
    if (grip == m_gripColumns.cend()) {
        return QScrollArea::eventFilter(watched, event);
    }
    MillerColumn *column = grip.value();
    const auto setWidth = [this, column](int width) {
        width = std::clamp(width, minimumColumnWidth, maximumColumnWidth);
        column->setFixedWidth(width);
        m_widths.insert(column->directoryModel()->path(), width);
        int total = 0;
        for (int i = 0; i < m_layout->count(); ++i) {
            if (QWidget *widget = m_layout->itemAt(i)->widget(); widget != nullptr && widget->isVisible()) {
                total += widget->width() == 0 ? widget->minimumWidth() : widget->maximumWidth() < QWIDGETSIZE_MAX
                                                                            ? widget->maximumWidth()
                                                                            : widget->width();
            }
        }
        m_strip->setMinimumWidth(total);
    };
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton) {
            m_dragColumn = column;
            m_dragStartX = int(mouse->globalPosition().x());
            m_dragStartWidth = column->width();
            return true;
        }
        break;
    }
    case QEvent::MouseMove:
        if (m_dragColumn == column) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            setWidth(m_dragStartWidth + int(mouse->globalPosition().x()) - m_dragStartX);
            return true;
        }
        break;
    case QEvent::MouseButtonRelease:
        if (m_dragColumn == column) {
            m_dragColumn = nullptr;
            return true;
        }
        break;
    case QEvent::MouseButtonDblClick:
        // Al ancho del nombre más largo (con su icono y la flecha).
        setWidth(column->sizeHintForColumn(0) + 2 * column->fontMetrics().height());
        return true;
    default:
        break;
    }
    return QScrollArea::eventFilter(watched, event);
}

QStringList MillerView::columnPaths() const
{
    QStringList paths;
    for (const Level &column : m_ancestors) {
        paths.append(column.path);
    }
    paths.append(m_path);
    if (m_child.view != nullptr && m_child.view->isVisible() && !m_child.path.isEmpty()) {
        paths.append(m_child.path);
    }
    return paths;
}

MillerColumn *MillerView::columnFor(const QString &folder) const
{
    if (folder == m_path) {
        return m_current;
    }
    for (const Level &column : m_ancestors) {
        if (column.path == folder) {
            return column.view;
        }
    }
    return m_child.path == folder ? m_child.view : nullptr;
}

} // namespace eu::ui
