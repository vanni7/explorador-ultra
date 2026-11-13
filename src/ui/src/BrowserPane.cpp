// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/BrowserPane.h"

#include "eu/fs/DirectoryWatcher.h"
#include "eu/ui/FileClipboard.h"
#include "eu/ui/Locations.h"
#include "eu/ui/SearchResultsModel.h"
#include "eu/ui/VolumesView.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QAction>
#include <QDragEnterEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRubberBand>
#include <QSignalBlocker>
#include <QStackedLayout>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace eu::ui {

QString parentPathOf(const QString &path);

namespace {

/// Cuántos caracteres del nombre se dejan seleccionados al empezar a
/// renombrar: todo menos la extensión, para escribir encima sin perderla.
/// "informe.pdf" -> 7, "copia.tar.gz" -> 5, ".bashrc" -> todo.
qsizetype stemLength(const QString &name)
{
    qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0) {
        return name.size();
    }
    if (name.left(dot).endsWith(QStringLiteral(".tar"), Qt::CaseInsensitive) && dot > 4) {
        dot -= 4;
    }
    return dot;
}

/// Editor del nombre en la lista. Solo añade dos cosas al de Qt: selecciona
/// el nombre sin la extensión y avisa de cuándo empieza la edición.
class NameDelegate : public QStyledItemDelegate
{
public:
    NameDelegate(DirectoryModel *model, QObject *parent) : QStyledItemDelegate(parent), m_model(model) {}

    std::function<void()> onEditingStarted;

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override
    {
        QWidget *editor = QStyledItemDelegate::createEditor(parent, option, index);
        if (onEditingStarted) {
            onEditingStarted();
        }
        if (auto *lineEdit = qobject_cast<QLineEdit *>(editor)) {
            const bool isDirectory = m_model->entryAt(index.row()).isDirectory;
            const QString name = m_model->entryAt(index.row()).name;
            const qsizetype length = isDirectory ? name.size() : stemLength(name);
            // La vista selecciona todo el texto justo después de crear el
            // editor; por eso la selección parcial se aplica un instante después.
            QTimer::singleShot(0, lineEdit, [lineEdit, length] { lineEdit->setSelection(0, int(length)); });
        }
        return editor;
    }

private:
    DirectoryModel *m_model;
};

/// La lista de archivos, con arrastrar y soltar y selección con recuadro.
///
/// Soltar sobre una carpeta deja los archivos dentro de ella; soltar en
/// cualquier otro punto, en la carpeta que se está viendo. Con Ctrl se copia,
/// con Mayús se mueve, y sin teclas decide la regla que pone la ventana.
///
/// Arrastrar desde el nombre o el icono de un elemento lo arrastra. Arrastrar
/// desde cualquier otro punto (a la derecha del nombre, las otras columnas o
/// el espacio vacío) dibuja un recuadro que selecciona las filas que toca,
/// como en los exploradores habituales.
class FileView : public QTreeView
{
public:
    FileView(DirectoryModel *model, QWidget *parent) : QTreeView(parent), m_model(model)
    {
        setDragEnabled(true);
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDropIndicatorShown(false);   // el destino se marca aquí, con un recuadro
    }

    MoveByDefaultRule moveByDefault;
    std::function<void(const QStringList &, const QString &, bool)> onDrop;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        m_bandCandidate = false;
        m_bandActive = false;
        if (event->button() == Qt::LeftButton && !isOverName(event->position().toPoint())) {
            // Posible inicio de un recuadro: se recuerda dónde, en coordenadas
            // del contenido (para que el desplazamiento no lo mueva).
            m_bandCandidate = true;
            m_bandOrigin = event->position().toPoint() + QPoint(horizontalOffset(), verticalOffset());
            m_bandKeepsSelection = event->modifiers().testFlag(Qt::ControlModifier);
            m_selectionBeforeBand = selectionModel()->selection();
        }
        QTreeView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_bandCandidate || !(event->buttons() & Qt::LeftButton)) {
            QTreeView::mouseMoveEvent(event);
            return;
        }
        const QPoint position = event->position().toPoint();
        const QPoint origin = m_bandOrigin - QPoint(horizontalOffset(), verticalOffset());
        if (!m_bandActive && (position - origin).manhattanLength() < QApplication::startDragDistance()) {
            return;
        }
        m_bandActive = true;
        if (m_band == nullptr) {
            m_band = new QRubberBand(QRubberBand::Rectangle, viewport());
        }
        m_band->setGeometry(QRect(origin, position).normalized());
        m_band->show();
        selectRowsBetween(m_bandOrigin.y(), position.y() + verticalOffset());
        // No se llama a la clase base: así no empieza un arrastre de archivos.
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const bool wasBand = m_bandActive;
        m_bandCandidate = false;
        m_bandActive = false;
        if (m_band != nullptr) {
            m_band->hide();
        }
        if (wasBand) {
            setState(QAbstractItemView::NoState);
            event->accept();
            return;   // la selección ya quedó hecha; no es un clic
        }
        QTreeView::mouseReleaseEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (fileSelectionFrom(event->mimeData()).isEmpty()) {
            event->ignore();
            return;
        }
        event->acceptProposedAction();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        int targetRow = -1;
        const DropPlan plan = planFor(event, &targetRow);
        setDropTarget(plan.valid, targetRow);
        if (!plan.valid) {
            event->ignore();
            return;
        }
        // El cursor muestra lo que va a pasar: mover o copiar.
        event->setDropAction(plan.move ? Qt::MoveAction : Qt::CopyAction);
        event->accept();
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        setDropTarget(false, -1);
        event->accept();
    }

    void dropEvent(QDropEvent *event) override
    {
        int targetRow = -1;
        const DropPlan plan = planFor(event, &targetRow);
        setDropTarget(false, -1);
        if (!plan.valid) {
            event->ignore();
            return;
        }
        // A la aplicación de origen siempre se le contesta "copiado", aunque
        // aquí se vaya a mover. Si se le dijera "movido", algunos programas
        // borrarían ellos mismos el original, incluso si nuestro movimiento
        // falla o se omite por un nombre repetido. El original solo lo quita
        // el motor de operaciones, después de comprobar el destino.
        event->setDropAction(Qt::CopyAction);
        event->accept();
        if (onDrop) {
            onDrop(plan.paths, plan.target, plan.move);
        }
    }

    void paintEvent(QPaintEvent *event) override
    {
        QTreeView::paintEvent(event);
        if (!m_dropActive) {
            return;
        }
        QRect frame = viewport()->rect();
        if (m_dropRow >= 0 && m_dropRow < m_model->rowCount()) {
            frame = visualRect(m_model->index(m_dropRow, 0));
            frame.setLeft(0);
            frame.setRight(viewport()->width() - 1);
        }
        QPainter painter(viewport());
        painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
        painter.drawRect(frame.adjusted(1, 1, -2, -2));
    }

private:
    /// true si el punto cae sobre el icono o el texto del nombre de una fila.
    bool isOverName(const QPoint &position) const
    {
        const QModelIndex index = indexAt(position);
        if (!index.isValid() || index.column() != int(Column::Name)) {
            return false;
        }
        const QRect cell = visualRect(index);
        const int textWidth = fontMetrics().horizontalAdvance(m_model->entryAt(index.row()).name);
        const int nameRight = cell.left() + iconSize().width() + textWidth + 16;   // icono, texto y márgenes
        return position.x() <= qMin(nameRight, cell.right());
    }

    /// Selecciona las filas entre dos alturas (en coordenadas del contenido).
    void selectRowsBetween(int contentY1, int contentY2)
    {
        const int rows = m_model->rowCount();
        const int rowHeight = rows > 0 ? visualRect(m_model->index(0, 0)).height() : 0;
        QItemSelection selection = m_bandKeepsSelection ? m_selectionBeforeBand : QItemSelection();
        if (rowHeight > 0) {
            const int first = qMax(0, qMin(contentY1, contentY2) / rowHeight);
            const int last = qMin(rows - 1, qMax(contentY1, contentY2) / rowHeight);
            if (first <= last) {
                selection.merge(QItemSelection(m_model->index(first, 0), m_model->index(last, int(Column::Count) - 1)),
                                QItemSelectionModel::Select);
            }
        }
        selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    }

    DropPlan planFor(const QDropEvent *event, int *targetRow) const
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

    void setDropTarget(bool active, int row)
    {
        if (m_dropActive != active || m_dropRow != row) {
            m_dropActive = active;
            m_dropRow = row;
            viewport()->update();
        }
    }

    DirectoryModel *m_model;
    bool m_dropActive = false;
    int m_dropRow = -1;

    QRubberBand *m_band = nullptr;
    bool m_bandCandidate = false;
    bool m_bandActive = false;
    bool m_bandKeepsSelection = false;
    QPoint m_bandOrigin;
    QItemSelection m_selectionBeforeBand;
};

} // namespace

QString parentPathOf(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (clean.isEmpty() || QDir(clean).isRoot()) {
        return {};
    }
    const qsizetype slash = clean.lastIndexOf(QLatin1Char('/'));
    if (slash < 0) {
        return {};
    }
    if (slash == 0) {
        return QStringLiteral("/");
    }
    QString parent = clean.left(slash);
    // "C:/Users" -> "C:/" (y no "C:", que en Windows significa otra cosa).
    if (parent.size() == 2 && parent.at(1) == QLatin1Char(':')) {
        parent += QLatin1Char('/');
    }
    return parent;
}

BrowserPane::BrowserPane(tasks::Scheduler &scheduler, fs::DirectoryWatcher &watcher, QWidget *parent)
    : QWidget(parent), m_scheduler(scheduler), m_model(new DirectoryModel(scheduler, this)), m_watcher(watcher),
      m_volumeSource(&fs::listVolumes)
{
    auto *fileView = new FileView(m_model, this);
    fileView->onDrop = [this](const QStringList &paths, const QString &destination, bool move) {
        emit filesDropped(paths, destination, move);
    };
    m_view = fileView;
    m_view->setModel(m_model);
    m_view->setRootIsDecorated(false);
    m_view->setUniformRowHeights(true);   // imprescindible para que 100 000 filas vayan fluidas
    m_view->setItemsExpandable(false);
    m_view->setAllColumnsShowFocus(true);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    // Renombrar: con la tecla de edición (F2; Intro en macOS) o desde el menú.
    // El "clic lento" sobre un nombre ya seleccionado queda desactivado, como
    // se acordó en la Fase 0: se confunde con el doble clic y con el arrastre.
    m_view->setEditTriggers(QAbstractItemView::EditKeyPressed);
    auto *nameDelegate = new NameDelegate(m_model, this);
    nameDelegate->onEditingStarted = [this] { m_editing = true; };
    m_view->setItemDelegateForColumn(int(Column::Name), nameDelegate);
    connect(nameDelegate, &QAbstractItemDelegate::closeEditor, this, &BrowserPane::onEditingFinished);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        emit contextMenuRequested(m_view->viewport()->mapToGlobal(position));
    });
    m_view->setSortingEnabled(true);
    m_view->sortByColumn(int(Column::Name), Qt::AscendingOrder);
    m_view->setAlternatingRowColors(true);
    m_view->setAccessibleName(tr("Contenido de la carpeta"));
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setSectionResizeMode(int(Column::Name), QHeaderView::Stretch);
    m_view->header()->resizeSection(int(Column::Size), 100);
    m_view->header()->resizeSection(int(Column::Type), 190);
    m_view->header()->resizeSection(int(Column::Modified), 150);

    // Página de mensajes: carpeta vacía, error de lectura…
    m_messagePage = new QWidget(this);
    m_messageLabel = new QLabel(m_messagePage);
    m_messageLabel->setAlignment(Qt::AlignCenter);
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_retryButton = new QPushButton(tr("Reintentar"), m_messagePage);
    auto *messageLayout = new QVBoxLayout(m_messagePage);
    messageLayout->addStretch();
    messageLayout->addWidget(m_messageLabel);
    messageLayout->addWidget(m_retryButton, 0, Qt::AlignHCenter);
    messageLayout->addStretch();

    auto *stackHost = new QWidget(this);
    m_stack = new QStackedLayout(stackHost);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->addWidget(m_view);
    m_stack->addWidget(m_messagePage);

    // Lista de unidades: lo que se ve en la ubicación "Unidades".
    m_volumesView = new VolumesView(this);
    m_stack->addWidget(m_volumesView);
    connect(m_volumesView, &VolumesView::volumeActivated, this, &BrowserPane::navigateTo);
    connect(m_volumesView, &QTreeWidget::itemSelectionChanged, this, &BrowserPane::statusChanged);
    // El espacio libre cambia solo: se vuelve a consultar cada pocos segundos,
    // pero solo mientras la lista está en pantalla.
    m_volumesTimer.setInterval(3000);
    connect(&m_volumesTimer, &QTimer::timeout, this, [this] {
        if (isVisible()) {
            reloadVolumes();
        }
    });

    // Resultados de búsqueda: una franja con el avance y los botones, y la lista.
    m_resultsModel = new SearchResultsModel(scheduler, this);
    m_searchPage = new QWidget(this);
    auto *searchBar = new QWidget(m_searchPage);
    searchBar->setAutoFillBackground(true);
    searchBar->setBackgroundRole(QPalette::AlternateBase);
    m_searchInfo = new QLabel(searchBar);
    // Un texto largo (rutas) no debe ensanchar la ventana.
    m_searchInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_searchPauseButton = new QPushButton(tr("Pausar"), searchBar);
    m_searchStopButton = new QPushButton(tr("Detener"), searchBar);
    m_searchAgainButton = new QPushButton(tr("Buscar de nuevo"), searchBar);
    auto *searchBarLayout = new QHBoxLayout(searchBar);
    searchBarLayout->setContentsMargins(6, 3, 6, 3);
    searchBarLayout->addWidget(m_searchInfo, 1);
    searchBarLayout->addWidget(m_searchPauseButton);
    searchBarLayout->addWidget(m_searchStopButton);
    searchBarLayout->addWidget(m_searchAgainButton);

    m_resultsView = new QTreeView(m_searchPage);
    m_resultsView->setModel(m_resultsModel);
    m_resultsView->setRootIsDecorated(false);
    m_resultsView->setUniformRowHeights(true);
    m_resultsView->setItemsExpandable(false);
    m_resultsView->setAllColumnsShowFocus(true);
    m_resultsView->setAlternatingRowColors(true);
    m_resultsView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_resultsView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultsView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultsView->setDragEnabled(true);
    m_resultsView->setDragDropMode(QAbstractItemView::DragOnly);
    m_resultsView->setAccessibleName(tr("Resultados de la búsqueda"));
    m_resultsView->setContextMenuPolicy(Qt::CustomContextMenu);
    // Sin orden elegido se ven en el orden en que aparecen: primero lo más
    // cercano a la carpeta donde se busca.
    m_resultsView->header()->setSortIndicator(-1, Qt::AscendingOrder);
    m_resultsView->setSortingEnabled(true);
    m_resultsView->header()->setStretchLastSection(false);
    m_resultsView->header()->resizeSection(SearchResultsModel::NameColumn, 280);
    m_resultsView->header()->setSectionResizeMode(SearchResultsModel::FolderColumn, QHeaderView::Stretch);
    m_resultsView->header()->resizeSection(SearchResultsModel::SizeColumn, 100);
    m_resultsView->header()->resizeSection(SearchResultsModel::TypeColumn, 170);
    m_resultsView->header()->resizeSection(SearchResultsModel::ModifiedColumn, 150);

    auto *searchLayout = new QVBoxLayout(m_searchPage);
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(0);
    searchLayout->addWidget(searchBar);
    searchLayout->addWidget(m_resultsView, 1);
    m_stack->addWidget(m_searchPage);

    connect(m_resultsView, &QTreeView::activated, this, &BrowserPane::onResultActivated);
    connect(m_resultsView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &BrowserPane::statusChanged);
    connect(m_resultsView, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        emit contextMenuRequested(m_resultsView->viewport()->mapToGlobal(position));
    });
    connect(m_resultsModel, &SearchResultsModel::contentReplaced, this, &BrowserPane::statusChanged);
    connect(m_searchPauseButton, &QPushButton::clicked, this, [this] {
        setSearchPaused(m_searchState == SearchState::Running);
    });
    connect(m_searchStopButton, &QPushButton::clicked, this, &BrowserPane::stopSearch);
    connect(m_searchAgainButton, &QPushButton::clicked, this, &BrowserPane::refresh);

    // Barra de filtro: aparece bajo la lista, con otro color de fondo, para
    // que no se confunda con la búsqueda global de la barra superior.
    m_filterBar = new QWidget(this);
    m_filterBar->setAutoFillBackground(true);
    m_filterBar->setBackgroundRole(QPalette::AlternateBase);
    auto *filterLabel = new QLabel(tr("Filtrar esta carpeta:"), m_filterBar);
    m_filterEdit = new QLineEdit(m_filterBar);
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setPlaceholderText(tr("nombre, *.pdf, ext:jpg, size:>1MB, modified:2025…"));
    m_filterEdit->setAccessibleName(tr("Filtro de la carpeta"));
    filterLabel->setBuddy(m_filterEdit);
    m_filterInfo = new QLabel(m_filterBar);
    auto *closeFilter = new QToolButton(m_filterBar);
    closeFilter->setText(tr("Quitar filtro"));
    closeFilter->setToolTip(tr("Quitar el filtro (Esc)"));
    auto *filterLayout = new QHBoxLayout(m_filterBar);
    filterLayout->setContentsMargins(6, 3, 6, 3);
    filterLayout->addWidget(filterLabel);
    filterLayout->addWidget(m_filterEdit, 1);
    filterLayout->addWidget(m_filterInfo);
    filterLayout->addWidget(closeFilter);
    m_filterBar->hide();

    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);
    outerLayout->addWidget(stackHost, 1);
    outerLayout->addWidget(m_filterBar);

    connect(m_filterEdit, &QLineEdit::textChanged, this, &BrowserPane::applyFilterText);
    connect(closeFilter, &QToolButton::clicked, this, &BrowserPane::hideFilter);
    // Intro o flecha abajo: pasar a la lista filtrada. Esc: quitar el filtro.
    connect(m_filterEdit, &QLineEdit::returnPressed, this, [this] { m_view->setFocus(); });
    auto *escape = new QAction(m_filterEdit);
    escape->setShortcut(QKeySequence(Qt::Key_Escape));
    escape->setShortcutContext(Qt::WidgetShortcut);
    m_filterEdit->addAction(escape);
    connect(escape, &QAction::triggered, this, &BrowserPane::hideFilter);

    connect(m_view, &QTreeView::activated, this, &BrowserPane::onActivated);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &BrowserPane::statusChanged);
    connect(m_model, &DirectoryModel::stateChanged, this, &BrowserPane::onModelStateChanged);
    connect(m_model, &DirectoryModel::contentReplaced, this, &BrowserPane::restoreSelection);
    connect(m_model, &DirectoryModel::contentReplaced, this, &BrowserPane::updateFilterInfo);
    connect(m_model, &DirectoryModel::renameRequested, this, &BrowserPane::renameRequested);
    connect(m_model, &DirectoryModel::folderSizesChanged, this, &BrowserPane::statusChanged);
    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, &BrowserPane::rememberSelection);
    connect(m_model, &QAbstractItemModel::rowsInserted, this, &BrowserPane::statusChanged);
    connect(m_retryButton, &QPushButton::clicked, this, &BrowserPane::refresh);
    connect(&m_watcher, &fs::DirectoryWatcher::directoryChanged, this, [this](const QString &directory) {
        if (directory == m_watchedPath) {
            refresh();
        }
    });
}

void BrowserPane::setMoveByDefaultRule(MoveByDefaultRule rule)
{
    static_cast<FileView *>(m_view)->moveByDefault = std::move(rule);
}

BrowserPane::~BrowserPane()
{
    // La tarea de búsqueda no toca este objeto: basta pedirle que pare.
    cancelSearch();
    if (!m_watchedPath.isEmpty()) {
        m_watcher.unwatch(m_watchedPath);
    }
}

void BrowserPane::navigateTo(const QString &path)
{
    // Las ubicaciones virtuales no son rutas: no se normalizan.
    const QString clean = isVolumesLocation(path) ? volumesLocation()
        : isSearchLocation(path) ? path : QDir::cleanPath(path);
    if (clean.isEmpty()) {
        return;
    }
    if (clean == currentPath()) {
        if (showsSearch()) {
            refresh();   // repetir la misma búsqueda es volver a buscar
        }
        return;
    }
    if (!currentPath().isEmpty()) {
        m_backHistory.append(currentPath());
    }
    m_forwardHistory.clear();
    changePath(clean, {});
}

void BrowserPane::goBack()
{
    if (!canGoBack()) {
        return;
    }
    const QString leaving = currentPath();
    m_forwardHistory.append(leaving);
    // Al volver se deja seleccionada la carpeta de la que se viene, si es hija.
    const QString target = m_backHistory.takeLast();
    if (isVolumesLocation(target)) {
        changePath(target, leaving);   // queda seleccionada la unidad de la que se viene
        return;
    }
    const bool cameFromChild = !isVirtualLocation(leaving) && !isVirtualLocation(target)
        && parentPathOf(leaving) == target;
    changePath(target, cameFromChild ? QDir(leaving).dirName() : QString());
}

void BrowserPane::goForward()
{
    if (!canGoForward()) {
        return;
    }
    m_backHistory.append(currentPath());
    changePath(m_forwardHistory.takeLast(), {});
}

void BrowserPane::goUp()
{
    if (!canGoUp()) {
        return;
    }
    const QString leaving = currentPath();
    m_backHistory.append(leaving);
    m_forwardHistory.clear();
    if (showsSearch()) {
        // Por encima de unos resultados está la carpeta donde se buscó.
        changePath(searchRootOf(leaving), {});
        return;
    }
    const QString parent = parentPathOf(leaving);
    if (parent.isEmpty()) {
        // Por encima de la raíz de una unidad está la lista de unidades.
        changePath(volumesLocation(), leaving);
    } else {
        changePath(parent, QDir(leaving).dirName());
    }
}

void BrowserPane::changePath(const QString &path, const QString &nameToSelect)
{
    if (!m_watchedPath.isEmpty()) {
        m_watcher.unwatch(m_watchedPath);
        m_watchedPath.clear();
    }
    m_location = path;
    if (!showsSearch() && m_searchState != SearchState::Idle) {
        // Se sale de unos resultados: la búsqueda se detiene y se libera la lista.
        cancelSearch();
        m_resultsModel->clear();
        m_searchState = SearchState::Idle;
    }
    if (showsSearch()) {
        m_volumesTimer.stop();
        m_model->clear();
        m_selectedNames.clear();
        m_currentName.clear();
        m_stack->setCurrentWidget(m_searchPage);
        startSearch();
    } else if (showsVolumes()) {
        // No hay carpeta que leer: el modelo queda vacío y se enseña la lista
        // de unidades. `nameToSelect` es aquí la raíz de la unidad a seleccionar.
        m_model->clear();
        m_selectedNames.clear();
        m_currentName.clear();
        m_volumeToSelect = nameToSelect;
        m_stack->setCurrentWidget(m_volumesView);
        reloadVolumes();
        m_volumesTimer.start();
    } else {
        m_volumesTimer.stop();
        m_stack->setCurrentWidget(m_view);
        m_model->setPath(path);
        // setPath vacía el modelo; lo que haya que seleccionar se fija después.
        m_selectedNames = nameToSelect.isEmpty() ? QStringList() : QStringList{nameToSelect};
        m_currentName = nameToSelect;
    }
    m_pendingNames.clear();
    m_editing = false;
    m_refreshDeferred = false;
    // El filtro era de la carpeta anterior (el modelo ya lo quitó en setPath).
    if (m_filterBar != nullptr && !m_filterBar->isHidden()) {
        const QSignalBlocker blocker(m_filterEdit);
        m_filterEdit->clear();
        m_filterProblem.clear();
        m_filterBar->hide();
        updateFilterInfo();
    }

    emit pathChanged(path);
    emit historyChanged();
    emit statusChanged();
}

void BrowserPane::refresh()
{
    if (showsVolumes()) {
        reloadVolumes();
        return;
    }
    if (showsSearch()) {
        startSearch();
        return;
    }
    if (m_editing) {
        m_refreshDeferred = true;   // se hará al terminar de escribir el nombre
        return;
    }
    m_model->refresh();
}

bool BrowserPane::showsVolumes() const
{
    return isVolumesLocation(m_location);
}

void BrowserPane::setVolumeSource(std::function<QList<fs::Volume>()> source)
{
    m_volumeSource = std::move(source);
    if (showsVolumes()) {
        reloadVolumes();
    }
}

void BrowserPane::setSizeUnits(core::SizeUnits units)
{
    m_model->setSizeUnits(units);
    m_volumesView->setSizeUnits(units);
    m_resultsModel->setSizeUnits(units);
}

void BrowserPane::focusContent()
{
    if (showsVolumes()) {
        m_volumesView->setFocus();
    } else if (showsSearch()) {
        m_resultsView->setFocus();
    } else {
        m_view->setFocus();
    }
}

int BrowserPane::selectedCount() const
{
    if (showsVolumes()) {
        return 0;   // una unidad no es un elemento sobre el que operar
    }
    const QTreeView *view = showsSearch() ? m_resultsView : m_view;
    return int(view->selectionModel()->selectedRows().size());
}

void BrowserPane::selectAll()
{
    if (showsSearch()) {
        m_resultsView->selectAll();
    } else if (showsFolder()) {
        m_view->selectAll();
    }
}

void BrowserPane::contentMayHaveChanged()
{
    if (showsSearch()) {
        // Volver a buscar por cada operación sería lento y perdería la
        // selección: basta quitar lo que ya no está.
        m_resultsModel->pruneMissing();
    } else {
        refresh();
    }
}

// ---------------------------------------------------------------- búsqueda

bool BrowserPane::showsSearch() const
{
    return isSearchLocation(m_location);
}

void BrowserPane::search(const QString &queryText)
{
    const QString text = queryText.trimmed();
    if (text.isEmpty() || m_location.isEmpty() || showsVolumes()) {
        return;
    }
    // Desde unos resultados se repite con el mismo alcance y las mismas opciones.
    SearchSpec spec;
    if (showsSearch()) {
        spec = searchSpecOf(m_location);
    } else {
        spec.origin = m_location;
    }
    spec.query = text;
    search(spec);
}

void BrowserPane::search(const SearchSpec &spec)
{
    if (spec.query.trimmed().isEmpty() || spec.origin.isEmpty()) {
        return;
    }
    navigateTo(searchLocation(spec));
}

QString BrowserPane::searchWhere() const
{
    if (!showsSearch()) {
        return {};
    }
    const SearchSpec spec = searchSpecOf(m_location);
    switch (spec.scope) {
    case SearchScope::Folder:
        return tr("%1 (sin subcarpetas)").arg(QDir::toNativeSeparators(spec.origin));
    case SearchScope::Subfolders:
        return QDir::toNativeSeparators(spec.origin);
    case SearchScope::Volume:
        return tr("la unidad %1").arg(QDir::toNativeSeparators(spec.roots.value(0)));
    case SearchScope::AllVolumes:
        return tr("todas las unidades (%1)").arg(spec.roots.size());
    case SearchScope::ChosenVolumes:
        break;
    }
    QStringList names;
    for (const QString &root : spec.roots) {
        names.append(QDir::toNativeSeparators(root));
    }
    return tr("las unidades %1").arg(names.join(QStringLiteral(", ")));
}

void BrowserPane::cancelSearch()
{
    ++m_searchGeneration;
    if (m_searchTask.isValid()) {
        m_searchTask.cancel();
        m_searchTask = tasks::TaskHandle();
    }
    m_searchGate.reset();
}

void BrowserPane::startSearch()
{
    cancelSearch();
    const SearchSpec spec = searchSpecOf(m_location);
    const bool withinFolder = spec.scope == SearchScope::Folder || spec.scope == SearchScope::Subfolders;
    m_resultsModel->clear();
    // Buscando en una carpeta, la columna Carpeta es relativa a ella; en
    // unidades enteras se muestra la ruta completa.
    m_resultsModel->setRoot(withinFolder ? spec.origin : QString());
    m_searchProgress = {};
    m_searchTruncated = false;
    m_searchError.clear();

    search::QueryOptions queryOptions;
    queryOptions.caseSensitive = spec.caseSensitive;
    queryOptions.termsInContent = spec.inContent;
    const core::Result<search::Query> parsedQuery = search::Query::parse(spec.query, queryOptions);
    if (!parsedQuery.ok()) {
        m_searchError = parsedQuery.error().message;
        m_searchState = SearchState::Invalid;
        updateSearchInfo();
        emit statusChanged();
        return;
    }
    const QStringList unavailable = parsedQuery.value().unavailableFilters();
    if (!unavailable.isEmpty()) {
        m_searchError = tr("Aún no disponible: %1").arg(unavailable.join(QStringLiteral(":, ")) + QLatin1Char(':'));
    }

    m_searchGate = std::make_shared<search::PauseGate>();
    m_searchState = SearchState::Running;
    search::SearchOptions options;
    options.includeHidden = m_model->showHidden();
    options.recursive = spec.scope != SearchScope::Folder;
    m_searchReadsContent = parsedQuery.value().usesContent();

    // La tarea no toca este objeto: trabaja con copias y devuelve los lotes al
    // hilo de interfaz, donde se comprueba que la pestaña sigue viva y que
    // los resultados son de la búsqueda vigente.
    const QPointer<BrowserPane> self(this);
    const quint64 generation = m_searchGeneration;
    m_searchTask = m_scheduler.submit(
        tasks::Category::Indexing, tasks::Priority::UserInteraction,
        [self, generation, roots = spec.roots, query = parsedQuery.value(), options,
         gate = m_searchGate](const tasks::CancelToken &cancel) {
            const search::SearchSummary summary = search::searchTree(
                roots, query, options, cancel, gate.get(),
                [&](std::vector<search::Hit> &&hits, const search::SearchProgress &progress) {
                    QMetaObject::invokeMethod(qApp, [self, generation, hits = std::move(hits), progress]() mutable {
                        if (self) {
                            self->onSearchHits(generation, std::move(hits), progress);
                        }
                    }, Qt::QueuedConnection);
                });
            QMetaObject::invokeMethod(qApp, [self, generation, summary] {
                if (self) {
                    self->onSearchFinished(generation, summary);
                }
            }, Qt::QueuedConnection);
        });
    updateSearchInfo();
    emit statusChanged();
}

void BrowserPane::onSearchHits(quint64 generation, std::vector<search::Hit> hits,
                               const search::SearchProgress &progress)
{
    if (generation != m_searchGeneration) {
        return;
    }
    m_searchProgress = progress;
    m_resultsModel->append(std::move(hits));
    updateSearchInfo();
    emit statusChanged();
}

void BrowserPane::onSearchFinished(quint64 generation, const search::SearchSummary &summary)
{
    if (generation != m_searchGeneration) {
        return;
    }
    m_searchProgress = summary.progress;
    m_searchTruncated = summary.truncated;
    m_searchState = summary.cancelled ? SearchState::Stopped : SearchState::Finished;
    m_searchTask = tasks::TaskHandle();
    m_resultsModel->resort();   // si hay un orden elegido, que incluya lo último
    updateSearchInfo();
    emit statusChanged();
}

void BrowserPane::setSearchPaused(bool paused)
{
    if (m_searchState != SearchState::Running && m_searchState != SearchState::Paused) {
        return;
    }
    m_searchGate->setPaused(paused);
    m_searchState = paused ? SearchState::Paused : SearchState::Running;
    updateSearchInfo();
    emit statusChanged();
}

void BrowserPane::stopSearch()
{
    if (m_searchState != SearchState::Running && m_searchState != SearchState::Paused) {
        return;
    }
    // Lo encontrado se queda a la vista; lo que la tarea entregue a partir
    // de ahora se descarta.
    cancelSearch();
    m_searchState = SearchState::Stopped;
    m_resultsModel->resort();
    updateSearchInfo();
    emit statusChanged();
}

QString BrowserPane::searchInfo() const
{
    return m_searchInfo->text();
}

void BrowserPane::updateSearchInfo()
{
    const bool active = m_searchState == SearchState::Running || m_searchState == SearchState::Paused;
    m_searchPauseButton->setVisible(active);
    m_searchPauseButton->setText(m_searchState == SearchState::Paused ? tr("Reanudar") : tr("Pausar"));
    m_searchStopButton->setVisible(active);
    m_searchAgainButton->setVisible(m_searchState == SearchState::Finished || m_searchState == SearchState::Stopped);

    QPalette palette;
    if (m_searchState == SearchState::Invalid) {
        palette.setColor(QPalette::WindowText, QColor(0xb0, 0x2a, 0x1e));
        m_searchInfo->setPalette(palette);
        m_searchInfo->setText(tr("No se puede buscar: %1").arg(m_searchError));
        return;
    }
    m_searchInfo->setPalette(palette);

    QString pattern;
    switch (m_searchState) {
    case SearchState::Running:  pattern = tr("Buscando «%1» en %2 — carpetas revisadas: %3"); break;
    case SearchState::Paused:   pattern = tr("En pausa. «%1» en %2 — carpetas revisadas: %3"); break;
    case SearchState::Stopped:  pattern = tr("Detenida antes de terminar. «%1» en %2 — carpetas revisadas: %3"); break;
    default:                    pattern = tr("Búsqueda terminada. «%1» en %2 — carpetas revisadas: %3"); break;
    }
    const SearchSpec spec = searchSpecOf(m_location);
    QString text = pattern.arg(spec.query, searchWhere(), QLocale().toString(m_searchProgress.foldersScanned));
    if (m_searchReadsContent) {
        text += tr(" · archivos leídos: %1 (solo texto, hasta 20 MB)")
                    .arg(QLocale().toString(m_searchProgress.filesRead));
    }
    if (spec.caseSensitive) {
        text += tr(" · distingue mayúsculas");
    }
    if (m_searchProgress.unreadableFolders > 0) {
        text += tr(" · no se pudieron leer: %1").arg(QLocale().toString(m_searchProgress.unreadableFolders));
    }
    if (m_searchTruncated) {
        text += tr(" · hay más resultados de los que se muestran: afina la búsqueda");
    }
    if (!m_searchError.isEmpty()) {
        text += QStringLiteral(" · ") + m_searchError;
    }
    m_searchInfo->setText(text);
    m_searchInfo->setToolTip(text);
}

void BrowserPane::onResultActivated(const QModelIndex &index)
{
    if (!index.isValid() || index.row() >= m_resultsModel->count()) {
        return;
    }
    // Copias: navegar vacía la lista de resultados.
    const QString path = m_resultsModel->filePath(index.row());
    const bool isDirectory = m_resultsModel->hitAt(index.row()).entry.isDirectory;
    if (isDirectory) {
        navigateTo(path);
    } else if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        emit openFailed(path);
    }
}

void BrowserPane::openContainingFolder()
{
    if (!showsSearch()) {
        return;
    }
    const QModelIndexList rows = m_resultsView->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return;
    }
    const search::Hit hit = m_resultsModel->hitAt(rows.first().row());
    m_backHistory.append(currentPath());
    m_forwardHistory.clear();
    changePath(hit.directory, hit.entry.name);
}

void BrowserPane::reloadVolumes()
{
    if (m_volumesLoading) {
        return;   // una unidad lenta no debe acumular consultas
    }
    m_volumesLoading = true;
    // Consultar el espacio puede tardar (unidades de red, discos dormidos):
    // se hace en una tarea y el resultado vuelve al hilo de interfaz.
    const QPointer<BrowserPane> self(this);
    m_scheduler.submit(tasks::Category::Listing, tasks::Priority::VisibleNavigation,
                       [self, source = m_volumeSource](const tasks::CancelToken &) {
        const QList<fs::Volume> volumes = source();
        QMetaObject::invokeMethod(qApp, [self, volumes] {
            if (!self) {
                return;
            }
            self->m_volumesLoading = false;
            if (!self->showsVolumes()) {
                return;
            }
            self->m_volumesView->setVolumes(volumes);
            if (!self->m_volumeToSelect.isEmpty()) {
                self->m_volumesView->selectRoot(self->m_volumeToSelect);
                self->m_volumeToSelect.clear();
            }
            emit self->statusChanged();
        }, Qt::QueuedConnection);
    });
}

void BrowserPane::showFilter()
{
    if (!showsFolder()) {
        return;   // el filtro es de una carpeta
    }
    m_filterBar->show();
    m_filterEdit->setFocus(Qt::ShortcutFocusReason);
    m_filterEdit->selectAll();
}

void BrowserPane::hideFilter()
{
    const bool hadFocus = m_filterEdit->hasFocus();
    m_filterEdit->clear();   // quita el filtro (applyFilterText con texto vacío)
    m_filterBar->hide();
    if (hadFocus) {
        m_view->setFocus();
    }
}

bool BrowserPane::isFilterVisible() const
{
    return !m_filterBar->isHidden();
}

QString BrowserPane::filterMessage() const
{
    return m_filterInfo->text();
}

void BrowserPane::applyFilterText(const QString &text)
{
    const core::Result<search::Query> parsedQuery = search::Query::parse(text);
    if (!parsedQuery.ok()) {
        // Consulta a medio escribir o mal escrita: se explica y la lista se
        // queda como estaba, en vez de vaciarse a cada tecla.
        m_filterProblem = parsedQuery.error().message;
        updateFilterInfo();
        return;
    }
    m_filterProblem.clear();
    const QStringList unavailable = parsedQuery.value().unavailableFilters();
    if (!unavailable.isEmpty()) {
        m_filterProblem = tr("Aún no disponible: %1").arg(unavailable.join(QStringLiteral(":, ")) + QLatin1Char(':'));
    } else if (parsedQuery.value().usesContent()) {
        // El filtro no abre archivos: se dice, en vez de dejar la lista vacía sin más.
        m_filterProblem = tr("content: solo funciona en el buscador (Ctrl+F)");
    }
    m_model->setFilter(parsedQuery.value());
    updateFilterInfo();
    emit statusChanged();
}

void BrowserPane::updateFilterInfo()
{
    QPalette palette = m_filterInfo->palette();
    if (!m_filterProblem.isEmpty()) {
        palette.setColor(QPalette::WindowText, QColor(0xb0, 0x2a, 0x1e));
        m_filterInfo->setText(m_filterProblem);
    } else {
        palette = QPalette();
        m_filterInfo->setText(m_model->filter().isEmpty()
                                  ? QString()
                                  : tr("%1 de %2").arg(m_model->rowCount()).arg(m_model->unfilteredCount()));
    }
    m_filterInfo->setPalette(palette);
}

void BrowserPane::onEditingFinished()
{
    m_editing = false;
    if (m_refreshDeferred) {
        m_refreshDeferred = false;
        m_model->refresh();
    }
}

void BrowserPane::startRename()
{
    const QModelIndex current = m_view->currentIndex();
    if (!current.isValid() || m_model->state() != DirectoryModel::State::Ready) {
        return;
    }
    const QModelIndex nameIndex = m_model->index(current.row(), int(Column::Name));
    m_view->setCurrentIndex(nameIndex);
    m_view->edit(nameIndex);
}

void BrowserPane::selectWhenAvailable(const QStringList &names, bool editFirst)
{
    m_pendingNames = names;
    m_pendingEdit = editFirst;
    m_pendingAttempts = 2;   // si tras dos lecturas no aparecen, se deja de esperar
    if (names.isEmpty()) {
        return;
    }
    // Lo normal es que la vista aún no los tenga: se vuelve a leer la carpeta
    // y se seleccionan cuando llegue el contenido nuevo (restoreSelection).
    if (m_model->state() != DirectoryModel::State::Ready || !applyPendingSelection()) {
        refresh();
    }
}

bool BrowserPane::applyPendingSelection()
{
    QList<int> rows;
    for (const QString &name : std::as_const(m_pendingNames)) {
        const int row = m_model->rowOfName(name);
        if (row >= 0) {
            rows.append(row);
        }
    }
    if (rows.isEmpty()) {
        return false;
    }
    QItemSelectionModel *selection = m_view->selectionModel();
    selection->clearSelection();
    for (const int row : std::as_const(rows)) {
        selection->select(m_model->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    const QModelIndex first = m_model->index(rows.first(), int(Column::Name));
    selection->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
    m_view->scrollTo(first);
    const bool edit = m_pendingEdit;
    m_pendingNames.clear();
    m_pendingEdit = false;
    if (edit) {
        m_view->edit(first);
    }
    emit statusChanged();
    return true;
}

QString BrowserPane::unusedName(const QString &wanted) const
{
    if (m_model->rowOfName(wanted) < 0) {
        return wanted;
    }
    // "Nuevo archivo.txt" -> "Nuevo archivo (2).txt": el número va antes de la extensión.
    const qsizetype dot = wanted.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot > 0 ? wanted.left(dot) : wanted;
    const QString suffix = dot > 0 ? wanted.mid(dot) : QString();
    for (int number = 2;; ++number) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(stem).arg(number).arg(suffix);
        if (m_model->rowOfName(candidate) < 0) {
            return candidate;
        }
    }
}

void BrowserPane::setShowHidden(bool show)
{
    m_model->setShowHidden(show);
}

void BrowserPane::setFolderSizesEnabled(bool enabled)
{
    m_model->setFolderSizesEnabled(enabled);
}

void BrowserPane::onModelStateChanged(DirectoryModel::State state)
{
    if (!showsFolder()) {
        emit statusChanged();   // sigue a la vista la lista de unidades o de resultados
        return;
    }
    switch (state) {
    case DirectoryModel::State::Failed:
        m_messageLabel->setText(tr("No se puede mostrar esta carpeta.\n%1\n\n%2")
                                    .arg(m_model->lastError().message, QDir::toNativeSeparators(m_model->path())));
        m_retryButton->setVisible(true);
        m_stack->setCurrentWidget(m_messagePage);
        break;
    case DirectoryModel::State::Ready:
        // Solo se vigila una carpeta que se pudo leer.
        if (m_watchedPath != m_model->path()) {
            if (!m_watchedPath.isEmpty()) {
                m_watcher.unwatch(m_watchedPath);
            }
            m_watchedPath = m_watcher.watch(m_model->path()) ? m_model->path() : QString();
        }
        m_stack->setCurrentWidget(m_view);
        break;
    case DirectoryModel::State::Loading:
    case DirectoryModel::State::Empty:
        m_stack->setCurrentWidget(m_view);
        break;
    }
    emit statusChanged();
}

void BrowserPane::rememberSelection()
{
    // Durante una carga nueva el modelo está vacío: no hay nada que recordar
    // y no se debe pisar lo que changePath dejó pendiente de seleccionar.
    if (m_model->rowCount() == 0) {
        return;
    }
    m_selectedNames.clear();
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    for (const QModelIndex &index : rows) {
        m_selectedNames.append(m_model->entryAt(index.row()).name);
    }
    const QModelIndex current = m_view->currentIndex();
    m_currentName = current.isValid() ? m_model->entryAt(current.row()).name : QString();
}

void BrowserPane::restoreSelection()
{
    if (!m_pendingNames.isEmpty()) {
        if (applyPendingSelection()) {
            return;
        }
        if (--m_pendingAttempts <= 0) {
            m_pendingNames.clear();
        }
    }
    QItemSelectionModel *selection = m_view->selectionModel();
    for (const QString &name : std::as_const(m_selectedNames)) {
        const int row = m_model->rowOfName(name);
        if (row >= 0) {
            selection->select(m_model->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
    const int currentRow = m_currentName.isEmpty() ? -1 : m_model->rowOfName(m_currentName);
    if (currentRow >= 0) {
        selection->setCurrentIndex(m_model->index(currentRow, 0), QItemSelectionModel::NoUpdate);
        m_view->scrollTo(m_model->index(currentRow, 0));
    }
    emit statusChanged();
}

QStringList BrowserPane::selectedPaths() const
{
    QStringList paths;
    if (showsVolumes()) {
        return paths;   // una unidad no se copia, corta ni borra como un archivo
    }
    if (showsSearch()) {
        const QModelIndexList found = m_resultsView->selectionModel()->selectedRows();
        for (const QModelIndex &index : found) {
            paths.append(m_resultsModel->filePath(index.row()));
        }
        return paths;
    }
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    for (const QModelIndex &index : rows) {
        paths.append(m_model->filePath(index.row()));
    }
    return paths;
}

void BrowserPane::onActivated(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }
    const fs::Entry &entry = m_model->entryAt(index.row());
    const QString path = m_model->filePath(index.row());
    if (entry.isDirectory) {
        navigateTo(path);
    } else if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        emit openFailed(path);
    }
}

void BrowserPane::openSelection()
{
    if (showsSearch()) {
        const QModelIndexList found = m_resultsView->selectionModel()->selectedRows();
        // Igual que en una carpeta: si hay una carpeta se entra en ella; si no,
        // se abren todos los archivos.
        for (const QModelIndex &index : found) {
            if (m_resultsModel->hitAt(index.row()).entry.isDirectory) {
                onResultActivated(index);
                return;
            }
        }
        QStringList files;
        for (const QModelIndex &index : found) {
            files.append(m_resultsModel->filePath(index.row()));
        }
        for (const QString &file : std::as_const(files)) {
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(file))) {
                emit openFailed(file);
            }
        }
        return;
    }
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    // Con varias carpetas seleccionadas solo se entra en la primera; los
    // archivos se abren todos.
    bool navigated = false;
    for (const QModelIndex &index : rows) {
        if (m_model->entryAt(index.row()).isDirectory) {
            if (!navigated) {
                navigated = true;
                onActivated(index);
                return;
            }
        }
    }
    for (const QModelIndex &index : rows) {
        onActivated(index);
    }
}

QString BrowserPane::statusText() const
{
    if (showsVolumes()) {
        const QList<fs::Volume> volumes = m_volumesView->volumes();
        QString summary = tr("%n unidad(es)", nullptr, int(volumes.size()));
        const QString selected = m_volumesView->selectedRoot();
        for (const fs::Volume &volume : volumes) {
            if (volume.rootPath == selected && volume.totalBytes > 0) {
                summary += tr(" · %1: %2 libres de %3")
                               .arg(volume.displayName, core::formatSize(volume.freeBytes, m_model->sizeUnits()),
                                    core::formatSize(volume.totalBytes, m_model->sizeUnits()));
            }
        }
        return summary;
    }
    if (showsSearch()) {
        const int found = m_resultsModel->count();
        QString summary;
        switch (m_searchState) {
        case SearchState::Running: summary = tr("Buscando… %n resultado(s)", nullptr, found); break;
        case SearchState::Paused:  summary = tr("Búsqueda en pausa: %n resultado(s)", nullptr, found); break;
        case SearchState::Stopped: summary = tr("Búsqueda detenida: %n resultado(s)", nullptr, found); break;
        case SearchState::Invalid: return tr("La búsqueda tiene un error");
        default:                   summary = tr("%n resultado(s)", nullptr, found); break;
        }
        const QModelIndexList chosen = m_resultsView->selectionModel()->selectedRows();
        if (!chosen.isEmpty()) {
            qint64 bytes = 0;
            bool onlyFiles = true;
            for (const QModelIndex &index : chosen) {
                const fs::Entry &entry = m_resultsModel->hitAt(index.row()).entry;
                onlyFiles = onlyFiles && !entry.isDirectory;
                bytes += entry.isDirectory ? 0 : entry.size;
            }
            summary += tr(" · %n seleccionado(s)", nullptr, int(chosen.size()));
            if (onlyFiles) {
                summary += QStringLiteral(" (%1)").arg(core::formatSize(bytes, m_model->sizeUnits()));
            }
        }
        return summary;
    }
    QString text;
    switch (m_model->state()) {
    case DirectoryModel::State::Loading:
        text = (m_model->rowCount() > 0) ? tr("Cargando… %n elemento(s)", nullptr, m_model->rowCount())
                                         : tr("Cargando…");
        break;
    case DirectoryModel::State::Failed:
        return tr("No se pudo leer la carpeta");
    default:
        text = m_model->filter().isEmpty()
            ? tr("%n elemento(s)", nullptr, m_model->rowCount())
            : tr("Filtro: %1 de %n elemento(s)", nullptr, m_model->unfilteredCount()).arg(m_model->rowCount());
        break;
    }
    if (m_model->hiddenCount() > 0) {
        text += tr(" (%n oculto(s))", nullptr, m_model->hiddenCount());
    }
    if (m_model->isMeasuringFolders()) {
        text += tr(" · calculando el tamaño de las carpetas…");
    }

    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    if (!rows.isEmpty()) {
        qint64 bytes = 0;
        bool onlyFiles = true;
        for (const QModelIndex &index : rows) {
            const fs::Entry &entry = m_model->entryAt(index.row());
            if (entry.isDirectory) {
                onlyFiles = false;
            } else {
                bytes += entry.size;
            }
        }
        text += tr(" · %n seleccionado(s)", nullptr, int(rows.size()));
        if (onlyFiles) {
            text += QStringLiteral(" (%1)").arg(core::formatSize(bytes, m_model->sizeUnits()));
        }
    }
    return text;
}

} // namespace eu::ui
