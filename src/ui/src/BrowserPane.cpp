// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/BrowserPane.h"

#include "eu/fs/DirectoryWatcher.h"
#include "eu/ui/FileClipboard.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QRubberBand>
#include <QStackedLayout>
#include <QStyledItemDelegate>
#include <QTimer>
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
    : QWidget(parent), m_model(new DirectoryModel(scheduler, this)), m_watcher(watcher)
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
    // Renombrar: con la tecla de edición (F2; Intro en macOS) o con un clic
    // sobre el nombre de un elemento ya seleccionado. El doble clic sigue
    // abriendo: Qt distingue los dos casos por el tiempo entre clics (SPEC §8.2).
    m_view->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
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

    m_stack = new QStackedLayout(this);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->addWidget(m_view);
    m_stack->addWidget(m_messagePage);

    connect(m_view, &QTreeView::activated, this, &BrowserPane::onActivated);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &BrowserPane::statusChanged);
    connect(m_model, &DirectoryModel::stateChanged, this, &BrowserPane::onModelStateChanged);
    connect(m_model, &DirectoryModel::contentReplaced, this, &BrowserPane::restoreSelection);
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
    if (!m_watchedPath.isEmpty()) {
        m_watcher.unwatch(m_watchedPath);
    }
}

void BrowserPane::navigateTo(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (clean.isEmpty() || clean == currentPath()) {
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
    changePath(target, parentPathOf(leaving) == target ? QDir(leaving).dirName() : QString());
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
    const QString parent = parentPathOf(currentPath());
    if (parent.isEmpty()) {
        return;
    }
    const QString leaving = currentPath();
    m_backHistory.append(leaving);
    m_forwardHistory.clear();
    changePath(parent, QDir(leaving).dirName());
}

void BrowserPane::changePath(const QString &path, const QString &nameToSelect)
{
    if (!m_watchedPath.isEmpty()) {
        m_watcher.unwatch(m_watchedPath);
        m_watchedPath.clear();
    }
    m_model->setPath(path);
    // setPath vacía el modelo; lo que haya que seleccionar se fija después.
    m_selectedNames = nameToSelect.isEmpty() ? QStringList() : QStringList{nameToSelect};
    m_currentName = nameToSelect;
    m_pendingNames.clear();
    m_editing = false;
    m_refreshDeferred = false;

    emit pathChanged(path);
    emit historyChanged();
    emit statusChanged();
}

void BrowserPane::refresh()
{
    if (m_editing) {
        m_refreshDeferred = true;   // se hará al terminar de escribir el nombre
        return;
    }
    m_model->refresh();
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
    QString text;
    switch (m_model->state()) {
    case DirectoryModel::State::Loading:
        text = (m_model->rowCount() > 0) ? tr("Cargando… %n elemento(s)", nullptr, m_model->rowCount())
                                         : tr("Cargando…");
        break;
    case DirectoryModel::State::Failed:
        return tr("No se pudo leer la carpeta");
    default:
        text = tr("%n elemento(s)", nullptr, m_model->rowCount());
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
