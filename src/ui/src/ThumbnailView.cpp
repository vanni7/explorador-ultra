// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/ThumbnailView.h"

#include "eu/thumbs/Thumbnail.h"
#include "eu/thumbs/ThumbnailService.h"
#include "eu/ui/DirectoryModel.h"
#include "eu/ui/EntrySort.h"
#include "eu/ui/InlinePlayer.h"
#include "eu/ui/Locations.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QDragEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
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
        // Fondo de toda la celda (imagen y nombre): el estilo, en modo icono,
        // solo resalta el texto, y una foto seleccionada no se distinguiría.
        Q_UNUSED(style);
        const QRect cell = opt.rect.adjusted(1, 1, -1, -1);
        QColor background;
        if (opt.state & QStyle::State_Selected) {
            background = opt.palette.color(QPalette::Highlight);
        } else if (opt.state & QStyle::State_MouseOver) {
            background = opt.palette.color(QPalette::Highlight);
            background.setAlpha(45);
        }
        if (background.isValid()) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(background);
            painter->drawRoundedRect(cell, 4, 4);
            painter->restore();
        }
        if ((opt.state & QStyle::State_HasFocus) && !(opt.state & QStyle::State_Selected)) {
            painter->save();
            painter->setPen(QPen(opt.palette.color(QPalette::Highlight), 1, Qt::DotLine));
            painter->drawRoundedRect(cell, 4, 4);
            painter->restore();
        }

        const int size = m_view->thumbnailSize();
        const QRect pictureArea(opt.rect.left() + (opt.rect.width() - size) / 2, opt.rect.top() + padding, size, size);
        const int row = index.row();
        thumbs::ThumbnailCache::Entry thumbnail;
        bool isDirectory = false;
        if (service != nullptr && row >= 0 && row < m_model->rowCount()) {
            const fs::Entry &entry = m_model->entryAt(row);
            isDirectory = entry.isDirectory && !entry.isSymlink;
            thumbs::ThumbnailRequest request;
            request.path = m_model->filePath(row);
            request.size = isDirectory ? 0 : entry.size;
            request.modifiedMs = entry.modifiedMs;
            request.isDirectory = isDirectory;
            request.maxSide = requestedSide(size, painter->device()->devicePixelRatioF());
            thumbnail = service->request(request);
        }
        const bool ready = thumbnail.state == thumbs::ThumbnailCache::State::Ready && !thumbnail.image.isNull();
        if (ready && !isDirectory) {
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
        } else {
            // Mientras llega, o si no hay miniatura: el icono del tipo, grande.
            // Una carpeta con miniatura enseña su contenido delante del icono.
            const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
            const int iconSide = (isDirectory && ready) ? size : size * 2 / 3;
            const QRect iconRect(pictureArea.left() + (size - iconSide) / 2,
                                 pictureArea.top() + (size - iconSide) / 2, iconSide, iconSide);
            icon.paint(painter, iconRect, Qt::AlignCenter,
                       (opt.state & QStyle::State_Enabled) ? QIcon::Normal : QIcon::Disabled);
            if (isDirectory && ready) {
                // Sobre la parte delantera de la carpeta, que es la mayor parte del icono.
                const int inner = size * 7 / 10;
                const QRect contents(pictureArea.left() + (size - inner) / 2,
                                     pictureArea.top() + (size - inner) / 2 + size / 14, inner, inner);
                painter->save();
                painter->setRenderHint(QPainter::SmoothPixmapTransform);
                painter->drawImage(contents, thumbnail.image);
                painter->restore();
            }
        }

        paintPlayback(painter, opt, index, pictureArea);

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
    /// Video o audio: el fotograma en reproducción, la barra de avance y el
    /// botón ▶ / ■ (al pasar el puntero, o en el elemento con el cursor).
    void paintPlayback(QPainter *painter, const QStyleOptionViewItem &opt, const QModelIndex &index,
                       const QRect &pictureArea) const
    {
        const int row = index.row();
        if (!m_view->playbackEnabled() || !m_view->isPlayableRow(row)) {
            return;
        }
        InlinePlayer *player = m_view->inlinePlayer();
        const bool playing = player->isPlaying() && player->path() == m_model->filePath(row);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        if (playing && !player->frame().isNull()) {
            // El fotograma tapa la miniatura en el mismo sitio y con el mismo tamaño.
            const QImage frame = player->frame();
            QSize shown = frame.size();
            shown.scale(pictureArea.size(), Qt::KeepAspectRatio);
            const QRect target(pictureArea.left() + (pictureArea.width() - shown.width()) / 2,
                               pictureArea.bottom() + 1 - shown.height(), shown.width(), shown.height());
            painter->fillRect(target, Qt::black);
            painter->drawImage(target, frame);
        }
        if (playing && player->durationMs() > 0) {
            const QRect track(pictureArea.left() + 4, pictureArea.bottom() - 5, pictureArea.width() - 8, 4);
            painter->fillRect(track, QColor(0, 0, 0, 120));
            QRect done = track;
            done.setWidth(int(track.width() * double(player->positionMs()) / double(player->durationMs())));
            painter->fillRect(done, opt.palette.color(QPalette::Highlight));
        }
        const bool hovered = m_view->hoverRow() == row;
        const bool current = (opt.state & QStyle::State_HasFocus) && m_view->hasFocus();
        if (playing || hovered || current) {
            const QRect button = m_view->playButtonRect(opt.rect);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(0, 0, 0, playing ? 110 : 150));
            painter->drawEllipse(button);
            painter->setBrush(Qt::white);
            const QRectF inner = QRectF(button).adjusted(button.width() * 0.32, button.height() * 0.3,
                                                         -button.width() * 0.28, -button.height() * 0.3);
            if (playing) {
                painter->drawRect(QRectF(button).adjusted(button.width() * 0.33, button.height() * 0.33,
                                                          -button.width() * 0.33, -button.height() * 0.33));
            } else {
                const QPointF triangle[3] = {inner.topLeft(), inner.bottomLeft(),
                                             QPointF(inner.right(), inner.center().y())};
                painter->drawPolygon(triangle, 3);
            }
        }
        painter->restore();
    }

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
    // Se selecciona la fila entera, como en la vista detallada: quien cuenta
    // lo seleccionado (barra de estado, copiar, borrar…) cuenta filas.
    setSelectionBehavior(QAbstractItemView::SelectRows);
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
    setMouseTracking(true);   // para saber sobre qué miniatura está el puntero

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

void ThumbnailView::setInlinePlayer(InlinePlayer *player)
{
    if (m_player != nullptr) {
        disconnect(m_player, nullptr, this, nullptr);
    }
    m_player = player;
    if (player != nullptr) {
        player->setMaxFrameSide(512);
        connect(player, &InlinePlayer::changed, viewport(), qOverload<>(&QWidget::update));
    }
    // Al cambiar de carpeta, lo que sonaba se para.
    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, &ThumbnailView::stopPlayback,
            Qt::UniqueConnection);
}

void ThumbnailView::setPlaybackEnabled(bool enabled)
{
    m_playbackEnabled = enabled;
    if (!enabled) {
        stopPlayback();
    }
    viewport()->update();
}

QRect ThumbnailView::playButtonRect(const QRect &itemRect) const
{
    const QRect pictureArea(itemRect.left() + (itemRect.width() - m_size) / 2, itemRect.top() + padding, m_size,
                            m_size);
    const int side = qBound(28, m_size / 3, 56);
    return QRect(pictureArea.center().x() - side / 2 + 1, pictureArea.center().y() - side / 2 + 1, side, side);
}

bool ThumbnailView::isPlayableRow(int row) const
{
    if (m_player == nullptr || row < 0 || row >= m_model->rowCount() || m_model->entryAt(row).isDirectory) {
        return false;
    }
    const thumbs::ThumbnailSource source = thumbs::thumbnailSourceFor(m_model->entryAt(row).name);
    // Video y audio se reconocen por la extensión aunque no haya FFmpeg para
    // las miniaturas: Qt Multimedia los reproduce por su cuenta.
    if (source == thumbs::ThumbnailSource::Video || source == thumbs::ThumbnailSource::Audio) {
        return true;
    }
    static const QStringList media{QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("webm"),
                                   QStringLiteral("avi"), QStringLiteral("mov"), QStringLiteral("m4v"),
                                   QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("ogg"),
                                   QStringLiteral("opus"), QStringLiteral("m4a"), QStringLiteral("wav")};
    return media.contains(QFileInfo(m_model->entryAt(row).name).suffix().toLower());
}

void ThumbnailView::togglePlayback(int row, bool fromKeyboard)
{
    if (!playbackEnabled() || !isPlayableRow(row)) {
        return;
    }
    const QString path = m_model->filePath(row);
    if (m_player->isPlaying() && m_player->path() == path) {
        m_player->stop();
        return;
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool isAudio = thumbs::thumbnailSourceFor(path) == thumbs::ThumbnailSource::Audio
        || QStringList{QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("ogg"), QStringLiteral("opus"),
                       QStringLiteral("m4a"), QStringLiteral("wav")}
               .contains(suffix);
    m_playbackFromKeyboard = fromKeyboard;
    m_player->play(path, isAudio);
}

void ThumbnailView::stopPlayback()
{
    if (m_player != nullptr && m_player->isPlaying()) {
        // Solo lo que se reproduce en esta carpeta: el reproductor es de la ventana.
        const QString playing = m_player->path();
        if (QFileInfo(playing).absolutePath() == QDir::cleanPath(m_model->path())) {
            m_player->stop();
        }
    }
}

void ThumbnailView::setHoverRow(int row)
{
    if (row == m_hoverRow) {
        return;
    }
    m_hoverRow = row;
    viewport()->update();
    // Se para cuando el puntero sale de lo que suena (si lo empezó el ratón).
    if (m_player != nullptr && m_player->isPlaying() && !m_playbackFromKeyboard
        && (row < 0 || m_model->filePath(row) != m_player->path())) {
        stopPlayback();
    }
}

void ThumbnailView::mouseMoveEvent(QMouseEvent *event)
{
    const QModelIndex index = indexAt(event->position().toPoint());
    setHoverRow(index.isValid() ? index.row() : -1);
    QListView::mouseMoveEvent(event);
}

void ThumbnailView::mousePressEvent(QMouseEvent *event)
{
    const QModelIndex index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && index.isValid() && playbackEnabled() && isPlayableRow(index.row())
        && playButtonRect(visualRect(index)).contains(event->position().toPoint())) {
        setHoverRow(index.row());
        togglePlayback(index.row(), false);
        event->accept();
        return;   // el botón no selecciona ni abre
    }
    QListView::mousePressEvent(event);
}

void ThumbnailView::keyPressEvent(QKeyEvent *event)
{
    const QModelIndex current = currentIndex();
    if (event->key() == Qt::Key_Space && event->modifiers() == Qt::NoModifier && current.isValid()
        && playbackEnabled() && isPlayableRow(current.row())) {
        togglePlayback(current.row(), true);
        event->accept();
        return;
    }
    QListView::keyPressEvent(event);
}

bool ThumbnailView::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::Leave) {
        setHoverRow(-1);
    }
    return QListView::viewportEvent(event);
}

void ThumbnailView::currentChanged(const QModelIndex &current, const QModelIndex &previous)
{
    QListView::currentChanged(current, previous);
    // Lo que se empezó con el teclado se para al mover el cursor a otro elemento.
    if (m_player != nullptr && m_player->isPlaying() && m_playbackFromKeyboard
        && (!current.isValid() || m_model->filePath(current.row()) != m_player->path())) {
        stopPlayback();
    }
}

void ThumbnailView::hideEvent(QHideEvent *event)
{
    stopPlayback();   // otra pestaña, otra vista u otra ventana encima: nada suena a escondidas
    QListView::hideEvent(event);
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
    if (m_model->path().isEmpty() || m_model->state() != DirectoryModel::State::Ready
        || isArchiveLocation(m_model->path())) {
        return {};   // dentro de un comprimido no se suelta nada
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
