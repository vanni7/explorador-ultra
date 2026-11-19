// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/MainWindow.h"

#include "eu/ui/Locations.h"

#include "eu/archive/Archive.h"
#include "eu/core/SizeFormat.h"
#include "eu/core/Version.h"
#include "eu/ui/BrowserPane.h"
#include "eu/trash/Trash.h"
#include "eu/ui/FileClipboard.h"
#include "eu/thumbs/Isolated.h"
#include "eu/ui/BulkRenameDialog.h"
#include "eu/ui/DuplicatesDialog.h"
#include "eu/ui/SimilarImagesDialog.h"
#include "eu/ui/TagsDialog.h"
#include "eu/ui/OrganizeDialog.h"
#include "eu/ui/ContentIndexDialog.h"
#include "eu/ui/IndexDialog.h"
#include "eu/ui/PathBar.h"
#include "eu/ui/InlinePlayer.h"
#include "eu/ui/PreviewPanel.h"
#include "eu/ui/Sidebar.h"
#include "eu/ui/TrashDialog.h"
#include "eu/ui/VolumesView.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QPointer>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QTreeView>

#include <chrono>
#include <future>

namespace eu::ui {

namespace {

QIcon themedIcon(const QString &name, QStyle::StandardPixmap fallback)
{
    return QIcon::fromTheme(name, QApplication::style()->standardIcon(fallback));
}

/// Dos paneles lado a lado, para cuando el tema de iconos no trae uno.
QIcon dualPaneIcon()
{
    QIcon icon;
    for (const int side : {16, 22, 32, 48}) {
        QPixmap pixmap(side, side);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QApplication::palette().color(QPalette::WindowText), std::max(1.0, side / 16.0)));
        const qreal margin = side / 8.0;
        const qreal gap = side / 10.0;
        const qreal width = (side - 2 * margin - gap) / 2;
        const qreal height = side - 3 * margin;
        painter.drawRect(QRectF(margin, margin * 1.5, width, height));
        painter.drawRect(QRectF(margin + width + gap, margin * 1.5, width, height));
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

} // namespace

MainWindow::MainWindow(tasks::Scheduler &scheduler, std::shared_ptr<journal::Journal> journal, QWidget *parent)
    : QMainWindow(parent), m_scheduler(scheduler), m_deviceMap(scheduler)
{
    // Concurrencia adaptada a cada unidad (SPEC §3.3): el planificador sabe
    // de qué clase es el disco de cada tarea.
    m_deviceMap.setVolumes(m_volumeMonitor.volumes());
    connect(&m_volumeMonitor, &fs::VolumeMonitor::volumesChanged, this,
            [this](const QList<fs::Volume> &volumes) { m_deviceMap.setVolumes(volumes); });
    m_journal = journal;
    m_actions = new FileActions(m_scheduler, std::move(journal), this);
    // Etiquetas (SPEC §17): en una base propia, junto a los demás datos.
    {
        const auto opened = tags::TagStore::open(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                                                 + QStringLiteral("/etiquetas.db"));
        if (opened.ok()) {
            m_tagStore = opened.value();
        }
    }
    // Índices de búsqueda: una base por unidad, junto a los demás datos de la
    // aplicación. La carpeta de la papelera no se indexa (SPEC §10).
    m_indexService = new index::IndexService(
        m_scheduler, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/indice"),
        this);
    m_indexService->setExcludedNames({trash::trashFolderName()});
    // Índices de contenido (SPEC §6.5): el texto de los documentos de las
    // carpetas elegidas, sacado en el proceso auxiliar.
    m_contentIndexService = new index::ContentIndexService(
        m_scheduler, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/indice"),
        this);
    m_contentIndexService->setExcludedNames({trash::trashFolderName()});
    m_contentIndexService->setTextProvider([](const QString &path, const tasks::CancelToken &cancel) {
        return thumbs::extractTextIsolated(path, cancel);
    });
    // Miniaturas: en la carpeta de caché del sistema (se puede borrar sin
    // perder nada), con 256 MB como mucho en memoria (SPEC §5.5).
    // Topes configurables en el archivo de configuración, sección [miniaturas]:
    // memoriaMB (256 de fábrica, SPEC §5.5) y discoMB (1024).
    {
        const QSettings settings;
        const qint64 memoryMb = std::max<qint64>(16, settings.value(QStringLiteral("miniaturas/memoriaMB"), 256).toLongLong());
        const qint64 diskMb = std::max<qint64>(16, settings.value(QStringLiteral("miniaturas/discoMB"), 1024).toLongLong());
        m_thumbnailCache = std::make_shared<thumbs::ThumbnailCache>(
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/miniaturas"),
            memoryMb * 1024 * 1024);
        // La caché en disco no crece sin límite: al arrancar, y en segundo
        // plano, se borra lo más antiguo de lo que sobre.
        m_scheduler.submit(tasks::Category::General, tasks::Priority::Background,
                           [cache = m_thumbnailCache, diskMb](const tasks::CancelToken &) {
            cache->pruneDisk(diskMb * 1024 * 1024);
        });
    }
    m_thumbnailService = new thumbs::ThumbnailService(m_scheduler, m_thumbnailCache, this);
    // Un solo reproductor para toda la ventana: nunca suenan dos a la vez.
    m_inlinePlayer = new InlinePlayer(this);
    setWindowTitle(core::applicationName());
    resize(1180, 720);

    // El centro: uno o dos grupos de pestañas (doble panel). m_tabs es el activo.
    m_leftTabs = makeTabGroup();
    m_tabs = m_leftTabs;
    m_centerSplitter = new QSplitter(Qt::Horizontal, this);
    m_centerSplitter->setChildrenCollapsible(false);
    m_centerSplitter->addWidget(boxFor(m_leftTabs, &m_leftMark));

    m_sidebar = new Sidebar(m_scheduler, m_volumeMonitor, this);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->addWidget(m_sidebar);
    m_splitter->addWidget(m_centerSplitter);
    // Panel derecho: metadatos y vista previa de lo que tiene el cursor.
    m_previewPanel = new PreviewPanel(m_scheduler, this);
    m_previewPanel->setMinimumWidth(180);
    m_splitter->addWidget(m_previewPanel);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setStretchFactor(2, 0);
    m_splitter->setSizes({230, 950});
    setCentralWidget(m_splitter);

    m_statusLabel = new QLabel(this);
    m_unitsLabel = new QLabel(this);
    statusBar()->addWidget(m_statusLabel, 1);
    m_indexLabel = new QLabel(this);
    m_indexLabel->hide();
    statusBar()->addPermanentWidget(m_indexLabel);
    connect(m_indexService, &index::IndexService::changed, this, &MainWindow::updateIndexLabel);
    connect(m_contentIndexService, &index::ContentIndexService::changed, this, &MainWindow::updateIndexLabel);
    statusBar()->addPermanentWidget(m_unitsLabel);

    createActions();
    createEditActions();
    createToolBar();
    createProgressWidgets();

    connect(m_actions, &FileActions::started, this, &MainWindow::onActionStarted);
    connect(m_actions, &FileActions::progressed, this, &MainWindow::onActionProgressed);
    connect(m_actions, &FileActions::finished, this, &MainWindow::onActionFinished);
    connect(m_actions, &FileActions::runningCountChanged, this, &MainWindow::onRunningCountChanged);
    connect(m_actions, &FileActions::interruptedOperationsFound, this, &MainWindow::onInterruptedOperationsFound);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &MainWindow::updateEditActions);

    // El panel activo es el que tiene el foco.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (now == nullptr || !isDualPane()) {
            return;
        }
        if (m_rightTabs->isAncestorOf(now)) {
            setActiveGroup(m_rightTabs);
        } else if (m_leftTabs->isAncestorOf(now)) {
            setActiveGroup(m_leftTabs);
        }
    });
    // Elegir un lugar en el árbol no le quita el teclado: las flechas siguen
    // moviéndose por el árbol, no por la lista de archivos.
    connect(m_sidebar, &Sidebar::placeActivated, this, [this](const QString &path) {
        if (BrowserPane *pane = currentPane()) {
            pane->navigateTo(path);
        }
    });
    connect(m_sidebar, &Sidebar::filesDropped, this, &MainWindow::transfer);
    connect(m_sidebar, &Sidebar::contextMenuRequested, this, &MainWindow::showSidebarMenu);
    // Ctrl+V con el teclado en el árbol pega en la carpeta elegida en él
    // (como "Pegar en esta carpeta" de su menú).
    auto *pasteInTree = new QAction(tr("Pegar en la carpeta del árbol"), m_sidebar);
    pasteInTree->setShortcut(QKeySequence::Paste);
    pasteInTree->setShortcutContext(Qt::WidgetShortcut);
    m_sidebar->addAction(pasteInTree);
    connect(pasteInTree, &QAction::triggered, this, [this] {
        const QString place = m_sidebar->currentPlace();
        if (m_actions->isAvailable() && !place.isEmpty() && QFileInfo(place).isDir()) {
            pasteInto(place);
        }
    });
    m_sidebar->setMoveByDefaultRule([this](const QString &source, const QString &destination) {
        return movesByDefault(source, destination);
    });
    connect(m_pathBar, &PathBar::pathCopied, this, [this] {
        statusBar()->showMessage(tr("Ruta copiada al portapapeles."), 4000);
    });
    connect(m_pathBar, &PathBar::pathRequested, this, &MainWindow::navigateCurrentTo);
}

MainWindow::~MainWindow()
{
    // Los paneles usan m_watcher y m_volumeMonitor. Qt destruiría los widgets
    // hijos *después* de esos miembros, así que se destruyen aquí, antes.
    delete takeCentralWidget();
}

void MainWindow::createActions()
{
    m_backAction = new QAction(themedIcon(QStringLiteral("go-previous"), QStyle::SP_ArrowBack), tr("Atrás"), this);
    m_backAction->setShortcuts({QKeySequence::Back, QKeySequence(Qt::ALT | Qt::Key_Left)});
    connect(m_backAction, &QAction::triggered, this, [this] { if (auto *pane = currentPane()) pane->goBack(); });

    m_forwardAction = new QAction(themedIcon(QStringLiteral("go-next"), QStyle::SP_ArrowForward), tr("Adelante"), this);
    m_forwardAction->setShortcuts({QKeySequence::Forward, QKeySequence(Qt::ALT | Qt::Key_Right)});
    connect(m_forwardAction, &QAction::triggered, this, [this] { if (auto *pane = currentPane()) pane->goForward(); });

    m_upAction = new QAction(themedIcon(QStringLiteral("go-up"), QStyle::SP_ArrowUp), tr("Subir un nivel"), this);
#ifdef Q_OS_MACOS
    m_upAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Up));   // ⌘↑
#else
    m_upAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
#endif
    connect(m_upAction, &QAction::triggered, this, [this] { if (auto *pane = currentPane()) pane->goUp(); });

    m_refreshAction = new QAction(themedIcon(QStringLiteral("view-refresh"), QStyle::SP_BrowserReload), tr("Actualizar"), this);
    m_refreshAction->setShortcuts({QKeySequence::Refresh, QKeySequence(Qt::CTRL | Qt::Key_R)});
    connect(m_refreshAction, &QAction::triggered, this, [this] {
        if (auto *pane = currentPane()) {
            pane->refresh();
            // Actualizar a mano también vuelve a sumar las carpetas.
            if (pane->model()->folderSizesEnabled()) {
                pane->model()->recalculateFolderSizes();
            }
        }
    });

    m_newTabAction = new QAction(themedIcon(QStringLiteral("tab-new"), QStyle::SP_FileDialogNewFolder), tr("Nueva pestaña"), this);
    m_newTabAction->setShortcut(QKeySequence::AddTab);   // Ctrl+T / ⌘T
    connect(m_newTabAction, &QAction::triggered, this, [this] {
        openTab(currentPane() ? currentPane()->currentPath() : QDir::homePath());
    });

    m_closeTabAction = new QAction(tr("Cerrar pestaña"), this);
    m_closeTabAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
    connect(m_closeTabAction, &QAction::triggered, this, [this] { closeTab(m_tabs->currentIndex()); });

    m_duplicateTabAction = new QAction(tr("Duplicar pestaña"), this);
    m_duplicateTabAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K));
    connect(m_duplicateTabAction, &QAction::triggered, this, &MainWindow::duplicateCurrentTab);

    auto *nextTabAction = new QAction(tr("Pestaña siguiente"), this);
    nextTabAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageDown)});
    connect(nextTabAction, &QAction::triggered, this, [this] {
        if (m_tabs->count() > 1) m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
    });
    auto *previousTabAction = new QAction(tr("Pestaña anterior"), this);
    previousTabAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), QKeySequence(Qt::CTRL | Qt::Key_PageUp)});
    connect(previousTabAction, &QAction::triggered, this, [this] {
        if (m_tabs->count() > 1) m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count());
    });

    m_showHiddenAction = new QAction(tr("Mostrar elementos ocultos"), this);
    m_showHiddenAction->setCheckable(true);
    m_showHiddenAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
    connect(m_showHiddenAction, &QAction::toggled, this, [this](bool show) {
        for (BrowserPane *pane : allPanes()) {
            pane->setShowHidden(show);
        }
    });

    m_binaryUnitsAction = new QAction(tr("Tamaños en unidades binarias (KiB, MiB…)"), this);
    m_binaryUnitsAction->setCheckable(true);
    connect(m_binaryUnitsAction, &QAction::toggled, this, [this](bool binary) {
        const core::SizeUnits units = binary ? core::SizeUnits::Binary : core::SizeUnits::Decimal;
        if (m_trashDialog != nullptr) {
            m_trashDialog->setSizeUnits(units);
        }
        for (BrowserPane *pane : allPanes()) {
            pane->setSizeUnits(units);
        }
        m_previewPanel->setSizeUnits(units);
        updateStatus();
    });

    m_folderSizesAction = new QAction(tr("Calcular el tamaño de las carpetas"), this);
    m_folderSizesAction->setCheckable(true);
    m_folderSizesAction->setToolTip(tr("Suma en segundo plano lo que ocupa cada carpeta; se detiene al cambiar de carpeta"));
    connect(m_folderSizesAction, &QAction::toggled, this, [this](bool enabled) {
        for (BrowserPane *pane : allPanes()) {
            pane->setFolderSizesEnabled(enabled);
        }
    });

    m_filterAction = new QAction(themedIcon(QStringLiteral("view-filter"), QStyle::SP_FileDialogContentsView),
                                 tr("Filtrar esta carpeta"), this);
    m_filterAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    m_filterAction->setToolTip(tr("Deja a la vista solo los elementos cuyo nombre coincide con lo que escribas"));
    connect(m_filterAction, &QAction::triggered, this, [this] { if (auto *pane = currentPane()) pane->showFilter(); });

    m_indexAction = new QAction(tr("Índice de búsqueda…"), this);
    m_indexAction->setToolTip(tr("Elegir qué unidades se indexan para buscar en ellas al instante"));
    connect(m_indexAction, &QAction::triggered, this, &MainWindow::showIndexDialog);
    m_contentIndexAction = new QAction(tr("Índice de contenido…"), this);
    m_contentIndexAction->setToolTip(tr("Elegir carpetas de documentos cuyo texto se guarda para buscar dentro "
                                        "de ellos al instante"));
    connect(m_contentIndexAction, &QAction::triggered, this, &MainWindow::showContentIndexDialog);

    m_searchAction = new QAction(themedIcon(QStringLiteral("edit-find"), QStyle::SP_FileDialogContentsView),
                                 tr("Buscar archivos y carpetas"), this);
    m_searchAction->setShortcut(QKeySequence::Find);
    connect(m_searchAction, &QAction::triggered, this, [this] {
        m_searchEdit->setFocus(Qt::ShortcutFocusReason);
        m_searchEdit->selectAll();
    });

    m_inlinePlaybackAction = new QAction(tr("Reproducir video y audio en las miniaturas"), this);
    m_inlinePlaybackAction->setCheckable(true);
    m_inlinePlaybackAction->setChecked(InlinePlayer::isAvailable());
    m_inlinePlaybackAction->setEnabled(InlinePlayer::isAvailable());
    m_inlinePlaybackAction->setToolTip(InlinePlayer::isAvailable()
                                           ? tr("Botón ▶ sobre los videos y audios, o barra espaciadora")
                                           : tr("Esta compilación no incluye Qt Multimedia"));
    connect(m_inlinePlaybackAction, &QAction::toggled, this, [this](bool enabled) {
        for (BrowserPane *pane : allPanes()) {
            pane->setInlinePlaybackEnabled(enabled);
        }
    });

    m_previewAction = new QAction(tr("Panel de previsualización"), this);
    m_previewAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_P));
    m_previewAction->setCheckable(true);
    m_previewAction->setChecked(true);
    m_previewAction->setToolTip(tr("Muestra a la derecha los datos y una vista previa de lo seleccionado"));
    connect(m_previewAction, &QAction::toggled, this, &MainWindow::setPreviewVisible);

    // Vistas de la carpeta (SPEC §5.3). Cada pestaña recuerda la suya.
    auto *viewModes = new QActionGroup(this);
    m_detailsAction = new QAction(themedIcon(QStringLiteral("view-list-details"), QStyle::SP_FileDialogDetailedView),
                                  tr("Detalles"), viewModes);
    m_detailsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1));
    m_detailsAction->setCheckable(true);
    m_detailsAction->setChecked(true);
    m_thumbnailsAction = new QAction(themedIcon(QStringLiteral("view-list-icons"), QStyle::SP_FileDialogListView),
                                     tr("Miniaturas"), viewModes);
    m_thumbnailsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_2));
    m_thumbnailsAction->setCheckable(true);
    m_columnsAction = new QAction(themedIcon(QStringLiteral("view-list-tree"), QStyle::SP_FileDialogContentsView),
                                  tr("Columnas"), viewModes);
    m_columnsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_3));
    m_columnsAction->setCheckable(true);
    m_columnsAction->setToolTip(tr("Columnas: una por cada carpeta de la ruta (← y → para subir y entrar)"));
    connect(m_detailsAction, &QAction::triggered, this, [this] { setViewMode(BrowserPane::ViewMode::Details); });
    connect(m_thumbnailsAction, &QAction::triggered, this, [this] { setViewMode(BrowserPane::ViewMode::Thumbnails); });
    connect(m_columnsAction, &QAction::triggered, this, [this] { setViewMode(BrowserPane::ViewMode::Columns); });

    // Doble panel (SPEC §5.3), con los atajos de Dolphin (F3) y de los
    // exploradores de dos paneles (F6 para cambiar; Mayús+F5/F6 copian y mueven
    // al otro panel, porque F5 es Actualizar).
    m_dualPaneAction = new QAction(QIcon::fromTheme(QStringLiteral("view-split-left-right"), dualPaneIcon()),
                                   tr("Doble panel"), this);
    m_dualPaneAction->setCheckable(true);
    m_dualPaneAction->setShortcut(QKeySequence(Qt::Key_F3));
    m_dualPaneAction->setToolTip(tr("Divide el centro en dos exploradores independientes (F3); F6 pasa de uno a otro"));
    connect(m_dualPaneAction, &QAction::toggled, this, &MainWindow::setDualPane);
    m_switchPaneAction = new QAction(tr("Cambiar de panel"), this);
    m_switchPaneAction->setShortcut(QKeySequence(Qt::Key_F6));
    connect(m_switchPaneAction, &QAction::triggered, this, &MainWindow::switchPane);
    m_copyToOtherAction = new QAction(tr("Copiar al otro panel"), this);
    m_copyToOtherAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
    connect(m_copyToOtherAction, &QAction::triggered, this, [this] { sendSelectionToOtherPane(false); });
    m_moveToOtherAction = new QAction(tr("Mover al otro panel"), this);
    m_moveToOtherAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F6));
    connect(m_moveToOtherAction, &QAction::triggered, this, [this] { sendSelectionToOtherPane(true); });

    m_editPathAction = new QAction(tr("Escribir una ruta"), this);
    m_editPathAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_L), QKeySequence(Qt::ALT | Qt::Key_D)});

    auto *quitAction = new QAction(tr("Salir"), this);
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    QMenu *fileMenu = menuBar()->addMenu(tr("&Archivo"));
    fileMenu->addAction(m_newTabAction);
    fileMenu->addAction(m_duplicateTabAction);
    fileMenu->addAction(m_closeTabAction);
    fileMenu->addSeparator();
    fileMenu->addAction(quitAction);

    QMenu *viewMenu = menuBar()->addMenu(tr("&Ver"));
    viewMenu->setObjectName(QStringLiteral("menuVer"));
    viewMenu->addAction(m_detailsAction);
    viewMenu->addAction(m_thumbnailsAction);
    viewMenu->addAction(m_columnsAction);
    viewMenu->addSeparator();
    viewMenu->addAction(m_dualPaneAction);
    viewMenu->addAction(m_switchPaneAction);
    QMenu *sizeMenu = viewMenu->addMenu(tr("Tamaño de las miniaturas"));
    auto *sizes = new QActionGroup(sizeMenu);
    for (const auto &[size, name] : {std::pair{96, tr("Pequeñas")}, std::pair{160, tr("Medianas")},
                                     std::pair{256, tr("Grandes")}}) {
        QAction *action = sizeMenu->addAction(name);
        action->setCheckable(true);
        sizes->addAction(action);
        m_thumbnailSizeActions.insert(size, action);
        connect(action, &QAction::triggered, this, [this, size = size] { setThumbnailSize(size); });
    }
    viewMenu->addAction(m_previewAction);
    viewMenu->addAction(m_inlinePlaybackAction);
    viewMenu->addSeparator();
    viewMenu->addAction(m_refreshAction);
    viewMenu->addAction(m_filterAction);
    viewMenu->addAction(m_searchAction);
    viewMenu->addAction(m_indexAction);
    viewMenu->addAction(m_contentIndexAction);
    viewMenu->addSeparator();
    viewMenu->addAction(m_showHiddenAction);
    viewMenu->addAction(m_binaryUnitsAction);
    viewMenu->addAction(m_folderSizesAction);

    QMenu *goMenu = menuBar()->addMenu(tr("&Ir"));
    goMenu->addAction(m_backAction);
    goMenu->addAction(m_forwardAction);
    goMenu->addAction(m_upAction);
    goMenu->addSeparator();
    goMenu->addAction(m_editPathAction);
    goMenu->addAction(nextTabAction);
    goMenu->addAction(previousTabAction);
}

void MainWindow::createEditActions()
{
    m_openAction = new QAction(themedIcon(QStringLiteral("document-open"), QStyle::SP_DialogOpenButton), tr("Abrir"), this);
    connect(m_openAction, &QAction::triggered, this, [this] { if (auto *pane = currentPane()) pane->openSelection(); });

    m_newFolderAction = new QAction(themedIcon(QStringLiteral("folder-new"), QStyle::SP_FileDialogNewFolder),
                                    tr("Nueva carpeta"), this);
    m_newFolderAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
    connect(m_newFolderAction, &QAction::triggered, this, &MainWindow::newFolder);

    m_newFileAction = new QAction(themedIcon(QStringLiteral("document-new"), QStyle::SP_FileIcon),
                                  tr("Nuevo archivo de texto"), this);
    connect(m_newFileAction, &QAction::triggered, this, &MainWindow::newFile);

    m_renameAction = new QAction(themedIcon(QStringLiteral("edit-rename"), QStyle::SP_FileDialogDetailedView),
                                 tr("Renombrar"), this);
#ifndef Q_OS_MACOS
    // En macOS se renombra con Intro, y de eso ya se encarga la lista.
    m_renameAction->setShortcut(QKeySequence(Qt::Key_F2));
#endif
    connect(m_renameAction, &QAction::triggered, this, &MainWindow::renameSelection);
    m_bulkRenameAction = new QAction(tr("Renombrar varios…"), this);
    m_bulkRenameAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F2));
    m_bulkRenameAction->setToolTip(tr("Renombrar muchos a la vez con reglas: mayúsculas, sustituir, contador, "
                                      "fecha de la foto, etiquetas de la música…"));
    connect(m_bulkRenameAction, &QAction::triggered, this, &MainWindow::showBulkRename);
    m_organizeAction = new QAction(tr("Organizar esta carpeta…"), this);
    m_organizeAction->setToolTip(tr("Ordenar en subcarpetas por tipo, fecha, tamaño o datos; siempre con "
                                    "simulación antes y se deshace todo junto"));
    connect(m_organizeAction, &QAction::triggered, this, &MainWindow::showOrganize);
    m_duplicatesAction = new QAction(tr("Buscar duplicados…"), this);
    m_duplicatesAction->setToolTip(tr("Archivos con el mismo contenido: cuánto ocupan de más y quitar las copias "
                                      "que elijas (a la papelera)"));
    connect(m_duplicatesAction, &QAction::triggered, this, &MainWindow::showDuplicates);
    m_similarAction = new QAction(tr("Imágenes parecidas…"), this);
    m_similarAction->setToolTip(tr("Fotos casi iguales (recomprimidas, reducidas, retocadas), lado a lado para "
                                   "elegir cuáles quitar"));
    connect(m_similarAction, &QAction::triggered, this, &MainWindow::showSimilarImages);
    m_tagsAction = new QAction(tr("Etiquetas…"), this);
    m_tagsAction->setToolTip(tr("Crear, renombrar y colorear etiquetas; revisar las que perdieron su archivo"));
    connect(m_tagsAction, &QAction::triggered, this, &MainWindow::showTags);

    m_cutAction = new QAction(themedIcon(QStringLiteral("edit-cut"), QStyle::SP_FileIcon), tr("Cortar"), this);
    m_cutAction->setShortcut(QKeySequence::Cut);
    connect(m_cutAction, &QAction::triggered, this, &MainWindow::cutSelection);

    m_copyAction = new QAction(themedIcon(QStringLiteral("edit-copy"), QStyle::SP_FileIcon), tr("Copiar"), this);
    m_copyAction->setShortcut(QKeySequence::Copy);
    connect(m_copyAction, &QAction::triggered, this, &MainWindow::copySelection);

    m_pasteAction = new QAction(themedIcon(QStringLiteral("edit-paste"), QStyle::SP_FileIcon), tr("Pegar"), this);
    m_pasteAction->setShortcut(QKeySequence::Paste);
    connect(m_pasteAction, &QAction::triggered, this, &MainWindow::paste);

    m_trashAction = new QAction(themedIcon(QStringLiteral("user-trash"), QStyle::SP_TrashIcon),
                                tr("Enviar a la papelera"), this);
#ifdef Q_OS_MACOS
    m_trashAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Backspace));   // ⌘⌫
#else
    m_trashAction->setShortcut(QKeySequence(Qt::Key_Delete));
#endif
    connect(m_trashAction, &QAction::triggered, this, &MainWindow::trashSelection);

    m_deleteAction = new QAction(themedIcon(QStringLiteral("edit-delete"), QStyle::SP_DialogDiscardButton),
                                 tr("Eliminar permanentemente…"), this);
#ifdef Q_OS_MACOS
    m_deleteAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Backspace));   // ⌥⌘⌫
#else
    m_deleteAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Delete));
#endif
    connect(m_deleteAction, &QAction::triggered, this, &MainWindow::deleteSelectionPermanently);

    m_undoAction = new QAction(themedIcon(QStringLiteral("edit-undo"), QStyle::SP_ArrowBack), tr("Deshacer"), this);
    m_undoAction->setShortcut(QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::undo);

    m_selectAllAction = new QAction(themedIcon(QStringLiteral("edit-select-all"), QStyle::SP_FileIcon),
                                    tr("Seleccionar todo"), this);
    m_selectAllAction->setShortcut(QKeySequence::SelectAll);
    connect(m_selectAllAction, &QAction::triggered, this, [this] {
        if (auto *pane = currentPane()) pane->selectAll();
    });

    m_showTrashAction = new QAction(themedIcon(QStringLiteral("user-trash-full"), QStyle::SP_TrashIcon),
                                    tr("Papelera"), this);
    m_showTrashAction->setToolTip(tr("Ver la papelera: restaurar o eliminar para siempre"));
    connect(m_showTrashAction, &QAction::triggered, this, &MainWindow::showTrash);
    menuBar()->actions().constLast()->menu()->addSeparator();
    menuBar()->actions().constLast()->menu()->addAction(m_showTrashAction);

    // Estos atajos solo actúan cuando el foco está en la zona de archivos. Así
    // Supr o Ctrl+V no hacen nada inesperado mientras se escribe una ruta o
    // se recorre el panel de lugares.
    const QList<QAction *> scoped{m_newFolderAction, m_renameAction, m_cutAction, m_copyAction, m_pasteAction,
                                  m_trashAction, m_deleteAction, m_undoAction, m_selectAllAction};
    for (QAction *action : scoped) {
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        m_paneActions.append(action);
        m_leftTabs->addAction(action);
        if (m_rightTabs != nullptr) {
            m_rightTabs->addAction(action);
        }
    }

    QMenu *fileMenu = menuBar()->actions().constFirst()->menu();
    QAction *firstFileAction = fileMenu->actions().constFirst();
    fileMenu->insertAction(firstFileAction, m_newFolderAction);
    fileMenu->insertAction(firstFileAction, m_newFileAction);
    fileMenu->insertSeparator(firstFileAction);

    auto *editMenu = new QMenu(tr("&Edición"), this);
    menuBar()->insertMenu(menuBar()->actions().at(1), editMenu);
    editMenu->addAction(m_undoAction);
    editMenu->addSeparator();
    editMenu->addAction(m_cutAction);
    editMenu->addAction(m_copyAction);
    editMenu->addAction(m_pasteAction);
    editMenu->addSeparator();
    editMenu->addAction(m_copyToOtherAction);
    editMenu->addAction(m_moveToOtherAction);
    editMenu->addSeparator();
    editMenu->addAction(m_selectAllAction);
    editMenu->addSeparator();
    editMenu->addAction(m_renameAction);
    editMenu->addAction(m_bulkRenameAction);
    QMenu *toolsMenu = menuBar()->addMenu(tr("&Herramientas"));
    toolsMenu->setObjectName(QStringLiteral("menuHerramientas"));
    toolsMenu->addAction(m_bulkRenameAction);
    toolsMenu->addAction(m_organizeAction);
    toolsMenu->addSeparator();
    toolsMenu->addAction(m_duplicatesAction);
    toolsMenu->addAction(m_similarAction);
    toolsMenu->addSeparator();
    toolsMenu->addAction(m_tagsAction);
    editMenu->addAction(m_trashAction);
    editMenu->addAction(m_deleteAction);

    updateEditActions();
}

void MainWindow::createProgressWidgets()
{
    m_progressBox = new QWidget(this);
    m_progressLabel = new QLabel(m_progressBox);
    m_progressBar = new QProgressBar(m_progressBox);
    m_progressBar->setFixedWidth(160);
    m_progressBar->setTextVisible(false);
    m_progressBar->setAccessibleName(tr("Avance de la operación"));
    m_cancelButton = new QToolButton(m_progressBox);
    m_cancelButton->setText(tr("Cancelar"));
    m_cancelButton->setToolTip(tr("Detener las operaciones en curso"));
    m_detailsButton = new QToolButton(m_progressBox);
    m_detailsButton->setText(tr("Detalles"));
    m_detailsButton->setToolTip(tr("Ver la cola de transferencias: pausar, reintentar, velocidad…"));
    auto *layout = new QHBoxLayout(m_progressBox);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_progressLabel);
    layout->addWidget(m_progressBar);
    layout->addWidget(m_detailsButton);
    layout->addWidget(m_cancelButton);
    statusBar()->addPermanentWidget(m_progressBox);
    m_progressBox->hide();

    connect(m_cancelButton, &QToolButton::clicked, m_actions, &FileActions::cancelAll);

    // Panel de la cola de transferencias, abajo; aparece al pedirlo, cuando
    // hay más de una en la cola o cuando una falla.
    m_transfersPanel = new TransfersPanel(*m_actions, this);
    m_transfersDock = new QDockWidget(tr("Transferencias"), this);
    m_transfersDock->setObjectName(QStringLiteral("transferencias"));
    m_transfersDock->setWidget(m_transfersPanel);
    m_transfersDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, m_transfersDock);
    m_transfersDock->hide();
    QAction *toggle = m_transfersDock->toggleViewAction();
    toggle->setText(tr("Transferencias"));
    toggle->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    for (QAction *menuAction : menuBar()->actions()) {
        if (menuAction->menu() != nullptr && menuAction->menu()->objectName() == QStringLiteral("menuVer")) {
            menuAction->menu()->addSeparator();
            menuAction->menu()->addAction(toggle);
        }
    }
    connect(m_detailsButton, &QToolButton::clicked, m_transfersDock, &QDockWidget::show);
    connect(m_actions, &FileActions::transfersChanged, this, [this] {
        int active = 0;
        bool failed = false;
        for (const TransferInfo &info : m_actions->transfers()) {
            active += info.isActive() ? 1 : 0;
            failed = failed || info.state == TransferInfo::State::Failed;
        }
        if (active > 1 || failed) {
            m_transfersDock->show();
        }
    });
    // Crear una carpeta o renombrar tarda milisegundos: el indicador solo
    // aparece si la acción sigue en marcha pasado un momento.
    m_progressDelay.setSingleShot(true);
    m_progressDelay.setInterval(400);
    connect(&m_progressDelay, &QTimer::timeout, this, [this] {
        if (m_actions->isBusy()) {
            m_progressBox->show();
        }
    });
}

bool MainWindow::movesByDefault(const QString &source, const QString &destination) const
{
    // Soltar sin teclas: dentro de la misma unidad se mueve y entre unidades
    // se copia, como en los exploradores habituales. Se decide con la lista
    // de unidades que ya está en memoria, sin consultar el disco.
    const auto rootOf = [this](const QString &path) {
        QString best;
        const QList<fs::Volume> volumes = m_volumeMonitor.volumes();
        for (const fs::Volume &volume : volumes) {
            const QString root = volume.rootPath;
            const bool inside = path == root
                || path.startsWith(root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/'));
            if (inside && root.size() > best.size()) {
                best = root;
            }
        }
        return best;
    };
    const QString sourceRoot = rootOf(source);
    // Si no se sabe en qué unidad está, se copia: es lo que no quita nada.
    return !sourceRoot.isEmpty() && sourceRoot == rootOf(destination);
}

void MainWindow::setDeleteConfirmation(std::function<bool(const QStringList &)> confirmation)
{
    m_deleteConfirmation = std::move(confirmation);
}

void MainWindow::updateEditActions()
{
    if (m_copyAction == nullptr) {
        return;   // aún se está construyendo la ventana
    }
    const BrowserPane *pane = currentPane();
    // En la lista de unidades no hay carpeta donde crear, pegar ni filtrar.
    // Tampoco en los resultados de una búsqueda, pero ahí sí se puede copiar,
    // cortar o borrar lo encontrado.
    const bool inFolder = pane != nullptr && pane->showsFolder();
    // Dentro de un comprimido se navega y se extrae, pero no se modifica ni
    // se copia (lo copiado serían rutas que no existen en el disco).
    const bool inArchive = pane != nullptr && pane->showsArchive();
    const bool canModify = inFolder && !inArchive && m_actions->isAvailable();
    const bool canRemove = pane != nullptr && !inArchive && m_actions->isAvailable();
    m_filterAction->setEnabled(inFolder);
    m_searchAction->setEnabled(pane != nullptr && !inArchive);
    if (m_searchEdit != nullptr) {
        m_searchEdit->setEnabled(m_searchAction->isEnabled());
    }
    const int selected = pane ? pane->selectedCount() : 0;
    const bool canPaste = !fileSelectionFrom(QGuiApplication::clipboard()->mimeData()).isEmpty();

    m_openAction->setEnabled(selected > 0);
    m_copyAction->setEnabled(selected > 0 && !inArchive);
    m_cutAction->setEnabled(canRemove && selected > 0);
    m_pasteAction->setEnabled(canModify && canPaste);
    const BrowserPane *other = otherPane();
    const bool otherIsFolder = other != nullptr && other->showsFolder() && !other->showsArchive();
    m_copyToOtherAction->setEnabled(otherIsFolder && selected > 0 && !inArchive);
    m_moveToOtherAction->setEnabled(otherIsFolder && selected > 0 && canModify);
    // F2 con varios seleccionados abre el renombrado masivo.
    m_renameAction->setEnabled(canModify && selected >= 1);
    m_bulkRenameAction->setEnabled(canModify);
    m_organizeAction->setEnabled(canModify);
    m_trashAction->setEnabled(canRemove && selected > 0);
    m_deleteAction->setEnabled(canRemove && selected > 0);
    m_newFolderAction->setEnabled(canModify);
    m_newFileAction->setEnabled(canModify);
    m_undoAction->setEnabled(m_actions->isAvailable());
    m_selectAllAction->setEnabled(pane != nullptr);
}

void MainWindow::showContextMenu(BrowserPane *pane, const QPoint &globalPosition)
{
    QMenu menu(this);
    if (pane->showsSearch()) {
        // Sobre los resultados: lo que tiene sentido hacer con lo encontrado.
        if (pane->selectedCount() == 0) {
            return;
        }
        menu.addAction(m_openAction);
        QAction *openFolder = menu.addAction(tr("Abrir la carpeta que lo contiene"));
        connect(openFolder, &QAction::triggered, pane, &BrowserPane::openContainingFolder);
        menu.addSeparator();
        menu.addAction(m_cutAction);
        menu.addAction(m_copyAction);
        menu.addSeparator();
        menu.addAction(m_trashAction);
        menu.addAction(m_deleteAction);
        menu.exec(globalPosition);
        return;
    }
    if (pane->showsArchive()) {
        // Dentro de un comprimido: abrir y sacar, nada más (SPEC §15).
        const QString location = pane->currentPath();
        const QString archivePath = archivePathOf(location);
        const QString inner = archiveInnerOf(location);
        if (pane->selectedCount() > 0) {
            menu.addAction(m_openAction);
            QStringList innerPaths;
            for (const QString &path : pane->selectedPaths()) {
                innerPaths.append(archiveInnerOf(path));
            }
            QAction *selected = menu.addAction(tr("Extraer lo seleccionado junto al comprimido"));
            connect(selected, &QAction::triggered, this, [this, archivePath, innerPaths, inner] {
                extractArchive(archivePath, innerPaths, inner);
            });
        }
        QAction *all = menu.addAction(tr("Extraer todo junto al comprimido"));
        connect(all, &QAction::triggered, this, [this, archivePath] { extractArchive(archivePath, {}, QString()); });
        all->setEnabled(m_actions->isAvailable());
        menu.exec(globalPosition);
        return;
    }
    if (pane->selectedCount() > 0) {
        menu.addAction(m_openAction);
        const QStringList selected = pane->selectedPaths();
        if (archive::isAvailable() && m_actions->isAvailable()) {
            if (selected.size() == 1 && archive::isArchiveName(selected.first()) && QFileInfo(selected.first()).isFile()) {
                const QString archivePath = selected.first();
                QAction *here = menu.addAction(tr("Extraer aquí"));
                here->setToolTip(tr("En esta misma carpeta; lo que ya exista no se sobrescribe"));
                connect(here, &QAction::triggered, this,
                        [this, archivePath] { extractArchive(archivePath, {}, QString(), false); });
                QAction *newFolder = menu.addAction(tr("Extraer aquí (en una carpeta nueva)"));
                connect(newFolder, &QAction::triggered, this,
                        [this, archivePath] { extractArchive(archivePath, {}, QString()); });
            }
            QMenu *compress = menu.addMenu(tr("Comprimir"));
            for (const auto &[format, name] : {std::pair{archive::Format::Zip, tr("En ZIP")},
                                               std::pair{archive::Format::SevenZip, tr("En 7z")},
                                               std::pair{archive::Format::TarGz, tr("En TAR.GZ")}}) {
                QAction *action = compress->addAction(name);
                connect(action, &QAction::triggered, this,
                        [this, pane, format = format] { compressSelection(pane, format); });
            }
        }
        // Etiquetas: marcadas las que tienen todos los seleccionados.
        if (m_tagStore && !isVirtualLocation(pane->currentPath())) {
            QMenu *tagMenu = menu.addMenu(tr("Etiquetas"));
            const QList<tags::Tag> allTags = m_tagStore->tags();
            for (const tags::Tag &tag : allTags) {
                bool everyone = true;
                for (const QString &path : selected) {
                    bool has = false;
                    for (const tags::Tag &own : pane->model()->tagsOf(QFileInfo(path).fileName())) {
                        has = has || own.id == tag.id;
                    }
                    everyone = everyone && has;
                }
                QAction *action = tagMenu->addAction(tagIcon(tag.color), tag.name);
                action->setCheckable(true);
                action->setChecked(everyone);
                connect(action, &QAction::toggled, this,
                        [this, selected, id = tag.id](bool on) { setTag(selected, id, on); });
            }
            if (!allTags.isEmpty()) {
                tagMenu->addSeparator();
            }
            QAction *create = tagMenu->addAction(tr("Nueva etiqueta…"));
            connect(create, &QAction::triggered, this, [this, selected] {
                const QString name = QInputDialog::getText(this, tr("Nueva etiqueta"), tr("Nombre:"));
                if (name.trimmed().isEmpty()) {
                    return;
                }
                const QStringList colors = tags::defaultColors();
                const auto created = m_tagStore->createTag(name, colors.value(int(m_tagStore->tags().size()) % colors.size()));
                if (!created.ok()) {
                    report(created.error().message, true);
                    return;
                }
                setTag(selected, created.value().id, true);
            });
            tagMenu->addAction(m_tagsAction);
        }
        menu.addSeparator();
        menu.addAction(m_cutAction);
        menu.addAction(m_copyAction);
        menu.addAction(m_pasteAction);
        menu.addSeparator();
        menu.addAction(m_renameAction);
        menu.addAction(m_trashAction);
        menu.addAction(m_deleteAction);
    } else {
        menu.addAction(m_newFolderAction);
        menu.addAction(m_newFileAction);
        menu.addSeparator();
        menu.addAction(m_pasteAction);
        menu.addSeparator();
        menu.addAction(m_selectAllAction);
        menu.addAction(m_refreshAction);
    }
    menu.exec(globalPosition);
}

QString MainWindow::freeName(const QString &folder, const QString &stem, const QString &extension)
{
    QString name = stem + extension;
    for (int i = 2; QFileInfo::exists(folder + QLatin1Char('/') + name) || QFileInfo(folder + QLatin1Char('/') + name).isSymLink();
         ++i) {
        name = QStringLiteral("%1 (%2)%3").arg(stem).arg(i).arg(extension);
    }
    return name;
}

void MainWindow::extractArchive(const QString &archivePath, const QStringList &innerPaths, const QString &innerFolder,
                                bool intoNewFolder)
{
    if (!m_actions->isAvailable()) {
        return;
    }
    // La carpeta nueva se llama como el comprimido, sin su extensión
    // ("fotos.tar.gz" -> "fotos").
    const QFileInfo info(archivePath);
    QString stem = info.fileName();
    const QString lower = stem.toLower();
    bool stripped = false;
    for (const char *double_ : {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst"}) {
        if (lower.endsWith(QLatin1String(double_))) {
            stem.chop(qsizetype(qstrlen(double_)));
            stripped = true;
            break;
        }
    }
    if (!stripped) {
        stem = info.completeBaseName();
    }
    const QString folder = info.path();
    const quint64 id = m_actions->extract(archivePath, innerPaths, innerFolder, folder,
                                          intoNewFolder ? freeName(folder, stem, QString()) : QString());
    m_requestedCounts.insert(id, 1);
    if (!intoNewFolder) {
        m_extractHereIds.append(id);
    }
}

void MainWindow::compressSelection(BrowserPane *pane, archive::Format format)
{
    const QStringList sources = pane->selectedPaths();
    if (sources.isEmpty() || !pane->showsFolder() || pane->showsArchive() || !m_actions->isAvailable()) {
        return;
    }
    // Un elemento: con su nombre; varios: "Comprimido".
    const QFileInfo first(sources.first());
    const QString stem = sources.size() == 1 ? (first.isDir() ? first.fileName() : first.completeBaseName())
                                             : tr("Comprimido");
    const QString folder = pane->currentPath();
    const QString output = folder + QLatin1Char('/') + freeName(folder, stem, archive::extensionOf(format));
    m_requestedCounts.insert(m_actions->compress(sources, output, format), 1);
}

void MainWindow::newFolder()
{
    BrowserPane *pane = currentPane();
    if (pane == nullptr || !pane->showsFolder() || !m_actions->isAvailable()) {
        return;
    }
    // Se crea ya con un nombre libre y se deja listo para escribir el definitivo.
    const quint64 id = m_actions->createFolder(pane->currentPath(), pane->unusedName(tr("Nueva carpeta")));
    m_requestedCounts.insert(id, 1);
    m_renameAfterCreateId = id;
}

void MainWindow::newFile()
{
    BrowserPane *pane = currentPane();
    if (pane == nullptr || !pane->showsFolder() || !m_actions->isAvailable()) {
        return;
    }
    const quint64 id = m_actions->createFile(pane->currentPath(), pane->unusedName(tr("Nuevo archivo.txt")));
    m_requestedCounts.insert(id, 1);
    m_renameAfterCreateId = id;
}

void MainWindow::renameSelection()
{
    BrowserPane *pane = currentPane();
    if (pane == nullptr) {
        return;
    }
    if (pane->selectedCount() > 1) {
        showBulkRename();
        return;
    }
    pane->startRename();
}

void MainWindow::showBulkRename()
{
    BrowserPane *pane = currentPane();
    if (pane == nullptr || !m_actions->isAvailable() || isVirtualLocation(pane->currentPath())) {
        return;
    }
    const QString folder = pane->currentPath();
    BulkRenameDialog dialog(m_scheduler, pane->selectedPaths(), folder, this);
    const bool accepted = m_bulkRenameDriver ? m_bulkRenameDriver(dialog) : dialog.exec() == QDialog::Accepted;
    const QList<QPair<QString, QString>> renames = dialog.renames();
    if (!accepted || renames.isEmpty()) {
        return;
    }
    m_requestedCounts.insert(m_actions->renameMany(renames, folder), int(renames.size()));
}

void MainWindow::showOrganize()
{
    BrowserPane *pane = currentPane();
    if (pane == nullptr || !m_actions->isAvailable() || isVirtualLocation(pane->currentPath())) {
        return;
    }
    OrganizeDialog dialog(m_scheduler, pane->currentPath(), this);
    const bool accepted = m_organizeDriver ? m_organizeDriver(dialog) : dialog.exec() == QDialog::Accepted;
    if (!accepted || dialog.plan().moves.isEmpty()) {
        return;
    }
    QList<QPair<QString, QString>> moves;
    for (const organize::Move &move : dialog.plan().moves) {
        moves.append({move.source, move.target});
    }
    m_requestedCounts.insert(m_actions->organize(moves, dialog.plan().foldersToCreate, dialog.folder()),
                             int(moves.size()));
}

void MainWindow::showDuplicates()
{
    BrowserPane *pane = currentPane();
    if (pane == nullptr || isVirtualLocation(pane->currentPath())) {
        return;
    }
    const QString folder = pane->currentPath();
    const auto [volume, roots] = volumesFor(folder);
    DuplicatesDialog dialog(m_scheduler, folder, volume, roots, this);
    const bool accepted = m_duplicatesDriver ? m_duplicatesDriver(dialog) : dialog.exec() == QDialog::Accepted;
    const QStringList marked = dialog.marked();
    if (!accepted || marked.isEmpty() || !m_actions->isAvailable()) {
        return;
    }
    // A la papelera propia: se recupera con Deshacer (SPEC §14.1).
    m_requestedCounts.insert(m_actions->trash(marked), int(marked.size()));
}

void MainWindow::setTag(const QStringList &paths, qint64 tagId, bool on)
{
    if (!m_tagStore || paths.isEmpty()) {
        return;
    }
    const QPointer<MainWindow> self(this);
    m_scheduler.submit(tasks::Category::Metadata, tasks::Priority::UserInteraction,
                       [self, store = m_tagStore, paths, tagId, on](const tasks::CancelToken &) {
        const core::Status status = on ? store->assign(paths, tagId) : store->unassign(paths, tagId);
        QMetaObject::invokeMethod(qApp, [self, status] {
            if (!self) {
                return;
            }
            if (!status.ok()) {
                self->report(status.error().message, true);
            }
            self->refreshTagsInPanes();
        }, Qt::QueuedConnection);
    });
}

void MainWindow::refreshTagsInPanes()
{
    for (BrowserPane *pane : allPanes()) {
        pane->model()->refreshTags();
    }
}

void MainWindow::followTags(const ActionResult &result)
{
    static const QSet<ActionKind> moving{ActionKind::Move,         ActionKind::Rename,  ActionKind::BulkRename,
                                         ActionKind::Organize,     ActionKind::Trash,   ActionKind::RestoreTrash,
                                         ActionKind::Undo,         ActionKind::Revert};
    if (!m_tagStore || !m_journal || result.outcome.operationId == 0 || !moving.contains(result.kind)) {
        return;
    }
    const QPointer<MainWindow> self(this);
    const bool reverse = result.kind == ActionKind::Undo || result.kind == ActionKind::Revert;
    m_scheduler.submit(tasks::Category::Metadata, tasks::Priority::Background,
                       [self, store = m_tagStore, journal = m_journal, id = result.outcome.operationId,
                        reverse](const tasks::CancelToken &) {
        const auto steps = journal->steps(id);
        if (!steps.ok()) {
            return;
        }
        QList<journal::Step> applied;
        if (reverse) {
            // Deshacer: los pasos deshechos, al revés y de vuelta.
            for (qsizetype i = steps.value().size() - 1; i >= 0; --i) {
                journal::Step step = steps.value()[i];
                if (step.state != journal::StepState::Undone) {
                    continue;
                }
                std::swap(step.source, step.target);
                step.state = journal::StepState::Done;
                applied.append(step);
            }
        } else {
            applied = steps.value();
        }
        store->followSteps(applied);
        QMetaObject::invokeMethod(qApp, [self] {
            if (self) {
                self->refreshTagsInPanes();
            }
        }, Qt::QueuedConnection);
    });
}

void MainWindow::showTags()
{
    if (!m_tagStore) {
        report(tr("Las etiquetas no están disponibles (no se pudo abrir su base)."), true);
        return;
    }
    QStringList roots;
    for (const fs::Volume &volume : searchVolumes()) {
        roots.append(volume.rootPath);
    }
    TagsDialog dialog(m_scheduler, m_tagStore, roots, this);
    connect(&dialog, &TagsDialog::tagsChanged, this, &MainWindow::refreshTagsInPanes);
    dialog.exec();
    refreshTagsInPanes();
}

QPair<QString, QStringList> MainWindow::volumesFor(const QString &folder)
{
    QStringList roots;
    QString volume;
    for (const fs::Volume &candidate : searchVolumes()) {
        roots.append(candidate.rootPath);
        const QString prefix = candidate.rootPath.endsWith(QLatin1Char('/')) ? candidate.rootPath
                                                                             : candidate.rootPath + QLatin1Char('/');
        if ((folder == candidate.rootPath || folder.startsWith(prefix)) && candidate.rootPath.size() > volume.size()) {
            volume = candidate.rootPath;
        }
    }
    return {volume, roots};
}

void MainWindow::showSimilarImages()
{
    BrowserPane *pane = currentPane();
    if (pane == nullptr || isVirtualLocation(pane->currentPath())) {
        return;
    }
    const QString folder = pane->currentPath();
    const auto [volume, roots] = volumesFor(folder);
    SimilarImagesDialog dialog(m_scheduler, folder, volume, roots, this);
    const bool accepted = m_similarDriver ? m_similarDriver(dialog) : dialog.exec() == QDialog::Accepted;
    const QStringList marked = dialog.marked();
    if (!accepted || marked.isEmpty() || !m_actions->isAvailable()) {
        return;
    }
    m_requestedCounts.insert(m_actions->trash(marked), int(marked.size()));
}

void MainWindow::copyToClipboard(bool cut)
{
    const BrowserPane *pane = currentPane();
    const QStringList paths = pane ? pane->selectedPaths() : QStringList();
    if (paths.isEmpty()) {
        return;
    }
    QGuiApplication::clipboard()->setMimeData(makeFileMimeData(paths, cut));
    report(cut ? tr("%n elemento(s) listo(s) para mover: pega en la carpeta de destino.", nullptr, int(paths.size()))
               : tr("%n elemento(s) copiado(s) al portapapeles.", nullptr, int(paths.size())),
           false);
}

void MainWindow::copySelection()
{
    copyToClipboard(false);
}

void MainWindow::cutSelection()
{
    copyToClipboard(true);
}

void MainWindow::paste()
{
    const BrowserPane *pane = currentPane();
    if (pane != nullptr && pane->showsFolder()) {
        pasteInto(pane->currentPath());
    }
}

void MainWindow::pasteInto(const QString &destination)
{
    const FileSelection selection = fileSelectionFrom(QGuiApplication::clipboard()->mimeData());
    if (selection.isEmpty() || destination.isEmpty()) {
        return;
    }
    transfer(selection.paths, destination, selection.cut);
    if (selection.cut) {
        // Lo cortado se pega una sola vez.
        QGuiApplication::clipboard()->clear();
    }
}

void MainWindow::showSidebarMenu(const QString &path, const QPoint &globalPosition)
{
    QMenu menu(this);
    QAction *open = menu.addAction(m_openAction->icon(), tr("Abrir"));
    QAction *openInTab = menu.addAction(m_newTabAction->icon(), tr("Abrir en una pestaña nueva"));
    menu.addSeparator();
    // No se reutiliza la acción "Pegar" del menú Edición: aquella pega en la
    // carpeta de la pestaña; esta, en la carpeta del árbol sobre la que se pulsó.
    QAction *pasteHere = menu.addAction(m_pasteAction->icon(), tr("Pegar en esta carpeta"));
    pasteHere->setEnabled(m_actions->isAvailable()
                          && !fileSelectionFrom(QGuiApplication::clipboard()->mimeData()).isEmpty());
    menu.addSeparator();
    QAction *copyPath = menu.addAction(tr("Copiar la ruta"));

    const QAction *chosen = menu.exec(globalPosition);
    if (chosen == open) {
        navigateCurrentTo(path);
    } else if (chosen == openInTab) {
        openTab(path);
    } else if (chosen == pasteHere) {
        pasteInto(path);
    } else if (chosen == copyPath) {
        QGuiApplication::clipboard()->setText(QDir::toNativeSeparators(path));
        statusBar()->showMessage(tr("Ruta copiada al portapapeles."), 4000);
    }
}

void MainWindow::transfer(const QStringList &paths, const QString &destination, bool move)
{
    if (!m_actions->isAvailable()) {
        report(tr("El historial de operaciones no está disponible; no se modifica nada."), true);
        return;
    }
    if (!move) {
        // Copiar sobre la misma carpeta crea "nombre (2)"; en otra, si algo
        // ya existe, se pregunta qué hacer.
        m_requestedCounts.insert(m_actions->copy(paths, destination, transferOptions(false)), int(paths.size()));
        return;
    }
    // Mover algo a la carpeta donde ya está no es ninguna operación.
    QStringList toMove;
    for (const QString &path : paths) {
        if (parentPathOf(path) != QDir::cleanPath(destination)) {
            toMove.append(path);
        }
    }
    if (toMove.isEmpty()) {
        report(tr("Los elementos ya están en esta carpeta."), false);
        return;
    }
    m_requestedCounts.insert(m_actions->move(toMove, destination, transferOptions(true)), int(toMove.size()));
}

ops::TransferOptions MainWindow::transferOptions(bool move)
{
    ops::TransferOptions options(ops::CollisionPolicy::Ask);
    options.copy = TransfersPanel::copyOptionsFromSettings();
    // La pregunta llega desde el hilo de la transferencia: se muestra aquí y
    // ese hilo espera la respuesta (o a que se cancele la operación).
    const QPointer<MainWindow> self(this);
    options.ask = [self, move](const ops::Conflict &conflict, const tasks::CancelToken &cancel) {
        auto answer = std::make_shared<std::promise<ops::CollisionDecision>>();
        std::future<ops::CollisionDecision> future = answer->get_future();
        QMetaObject::invokeMethod(qApp, [self, conflict, move, answer, cancel] {
            ops::CollisionDecision decision;
            decision.cancel = true;
            if (self && !cancel.isCancelled()) {
                if (self->m_testCollisionAnswer) {
                    decision = self->m_testCollisionAnswer(conflict, cancel);
                } else {
                    CollisionDialog dialog(conflict, move, self);
                    dialog.exec();
                    decision = dialog.decision();
                }
            }
            answer->set_value(decision);
        }, Qt::QueuedConnection);
        while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
            if (cancel.isCancelled()) {
                return ops::CollisionDecision{ops::CollisionPolicy::Skip, false, true};
            }
        }
        return future.get();
    };
    return options;
}

void MainWindow::trashSelection()
{
    const BrowserPane *pane = currentPane();
    const QStringList paths = pane ? pane->selectedPaths() : QStringList();
    if (paths.isEmpty() || !m_actions->isAvailable()) {
        return;
    }
    // Sin pregunta: enviar a la papelera se puede deshacer.
    m_requestedCounts.insert(m_actions->trash(paths), int(paths.size()));
}

void MainWindow::deleteSelectionPermanently()
{
    const BrowserPane *pane = currentPane();
    const QStringList paths = pane ? pane->selectedPaths() : QStringList();
    if (paths.isEmpty() || !m_actions->isAvailable()) {
        return;
    }
    if (askPermanentDelete(paths, {})) {
        m_requestedCounts.insert(m_actions->deletePermanently(paths), int(paths.size()));
    }
}

TrashDialog *MainWindow::showTrash()
{
    if (m_trashDialog == nullptr) {
        m_trashDialog = new TrashDialog(m_actions, this);
        m_trashDialog->setSizeUnits(m_binaryUnitsAction->isChecked() ? core::SizeUnits::Binary
                                                                     : core::SizeUnits::Decimal);
        connect(m_trashDialog, &TrashDialog::restoreRequested, this, [this](const QList<trash::TrashItem> &items) {
            m_requestedCounts.insert(m_actions->restoreFromTrash(items), int(items.size()));
        });
        connect(m_trashDialog, &TrashDialog::purgeRequested, this,
                [this](const QList<trash::TrashItem> &items, bool everything) {
            QStringList paths;
            for (const trash::TrashItem &item : items) {
                paths.append(item.originalPath);
            }
            const QString reason = everything ? tr("Se va a vaciar la papelera de todas las unidades conectadas.")
                                              : tr("Estos elementos están en la papelera.");
            if (askPermanentDelete(paths, reason)) {
                m_requestedCounts.insert(m_actions->purgeFromTrash(items), int(items.size()));
            }
        });
    }
    m_trashDialog->show();
    m_trashDialog->raise();
    m_trashDialog->activateWindow();
    m_trashDialog->reload();
    return m_trashDialog;
}

void MainWindow::undo()
{
    if (m_actions->isAvailable()) {
        m_actions->undoLast();
    }
}

bool MainWindow::askPermanentDelete(const QStringList &paths, const QString &reason)
{
    if (m_deleteConfirmation) {
        return m_deleteConfirmation(paths);
    }
    // Confirmación reforzada (SPEC §8.1): el botón de eliminar no se puede
    // pulsar hasta marcar la casilla, y el botón por defecto es Cancelar.
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Eliminar permanentemente"));
    box.setText(tr("¿Eliminar permanentemente %n elemento(s)?", nullptr, int(paths.size())));

    QStringList lines;
    if (!reason.isEmpty()) {
        lines.append(reason);
        lines.append(QString());
    }
    const qsizetype shown = qMin<qsizetype>(paths.size(), 5);
    for (qsizetype i = 0; i < shown; ++i) {
        lines.append(QDir::toNativeSeparators(paths[i]));
    }
    if (paths.size() > shown) {
        lines.append(tr("…y %n más", nullptr, int(paths.size() - shown)));
    }
    lines.append(QString());
    lines.append(tr("No pasarán por la papelera y no se podrán recuperar."));
    box.setInformativeText(lines.join(QLatin1Char('\n')));

    auto *understood = new QCheckBox(tr("Entiendo que esto no se puede deshacer"), &box);
    box.setCheckBox(understood);
    QPushButton *deleteButton = box.addButton(tr("Eliminar permanentemente"), QMessageBox::DestructiveRole);
    QPushButton *cancelButton = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancelButton);
    box.setEscapeButton(cancelButton);
    deleteButton->setEnabled(false);
    connect(understood, &QCheckBox::toggled, deleteButton, &QPushButton::setEnabled);
    box.exec();
    return box.clickedButton() == deleteButton;
}

void MainWindow::setRecoveryChoice(std::function<RecoveryChoice(const QList<journal::Operation> &)> choice)
{
    m_recoveryChoice = std::move(choice);
}

void MainWindow::checkInterruptedOperations()
{
    m_actions->checkJournalAtStartup();
}

void MainWindow::onInterruptedOperationsFound(const QList<journal::Operation> &operations)
{
    switch (askRecovery(operations)) {
    case RecoveryChoice::Revert:
        for (const journal::Operation &operation : operations) {
            m_actions->revertInterrupted(operation);
        }
        break;
    case RecoveryChoice::Discard:
        for (const journal::Operation &operation : operations) {
            m_actions->discardInterrupted(operation);
        }
        break;
    case RecoveryChoice::Resume:
        for (const journal::Operation &operation : operations) {
            m_actions->resumeInterrupted(operation, transferOptions(operation.kind == journal::OperationKind::Move));
        }
        break;
    case RecoveryChoice::Later:
        report(tr("Las operaciones interrumpidas quedan pendientes; se volverá a preguntar al abrir."), false);
        break;
    }
}

MainWindow::RecoveryChoice MainWindow::askRecovery(const QList<journal::Operation> &operations)
{
    if (m_recoveryChoice) {
        return m_recoveryChoice(operations);
    }
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Operaciones interrumpidas"));
    box.setText(tr("La última vez la aplicación se cerró sin terminar %n operación(es).", nullptr,
                   int(operations.size())));

    QStringList lines;
    const qsizetype shown = qMin<qsizetype>(operations.size(), 8);
    for (qsizetype i = 0; i < shown; ++i) {
        const journal::Operation &operation = operations[i];
        lines.append(QStringLiteral("• %1 (%2)")
                         .arg(operation.description,
                              QLocale().toString(QDateTime::fromMSecsSinceEpoch(operation.startedMs),
                                                 QLocale::ShortFormat)));
    }
    if (operations.size() > shown) {
        lines.append(tr("…y %n más", nullptr, int(operations.size() - shown)));
    }
    lines.append(QString());
    lines.append(tr("No se perdió ningún archivo: los originales solo se quitan cuando su copia está completa.\n\n"
                    "• Revertir: deshace lo que se alcanzó a hacer y deja todo como estaba antes.\n"
                    "• Descartar: conserva lo ya hecho y borra solo los archivos a medio escribir.\n"
                    "• Reanudar (copiar y mover): termina lo que faltaba; lo ya copiado no se repite y un "
                    "archivo grande cortado sigue desde donde se quedó."));
    box.setInformativeText(lines.join(QLatin1Char('\n')));

    QPushButton *revertButton = box.addButton(tr("Revertir"), QMessageBox::AcceptRole);
    QPushButton *discardButton = box.addButton(tr("Descartar"), QMessageBox::DestructiveRole);
    QPushButton *resumeButton = box.addButton(tr("Reanudar"), QMessageBox::ActionRole);
    // Solo copiar y mover saben reanudarse.
    bool allResumable = true;
    for (const journal::Operation &operation : operations) {
        allResumable = allResumable
            && (operation.kind == journal::OperationKind::Copy || operation.kind == journal::OperationKind::Move);
    }
    resumeButton->setEnabled(allResumable);
    if (!allResumable) {
        resumeButton->setToolTip(tr("Solo se pueden reanudar las copias y los movimientos"));
    }
    QPushButton *laterButton = box.addButton(tr("Decidir más tarde"), QMessageBox::RejectRole);
    box.setDefaultButton(allResumable ? resumeButton : revertButton);
    box.setEscapeButton(laterButton);
    box.exec();
    if (box.clickedButton() == revertButton) {
        return RecoveryChoice::Revert;
    }
    if (box.clickedButton() == discardButton) {
        return RecoveryChoice::Discard;
    }
    if (box.clickedButton() == resumeButton) {
        return RecoveryChoice::Resume;
    }
    return RecoveryChoice::Later;
}

void MainWindow::onActionStarted(quint64 id, ActionKind kind)
{
    m_runningKinds.insert(id, kind);
}

void MainWindow::onRunningCountChanged(int count)
{
    if (count == 0) {
        m_progressDelay.stop();
        m_progressBox->hide();
        return;
    }
    if (!m_progressBox->isVisible() && !m_progressDelay.isActive()) {
        m_progressLabel->setText(tr("Trabajando…"));
        m_progressBar->setRange(0, 0);   // sin datos aún: barra de "ocupado"
        m_progressDelay.start();
    }
}

void MainWindow::onActionProgressed(quint64 id, const ops::Progress &progress)
{
    QString verb;
    switch (m_runningKinds.value(id, ActionKind::Copy)) {
    case ActionKind::Copy:   verb = tr("Copiando"); break;
    case ActionKind::Move:   verb = tr("Moviendo"); break;
    case ActionKind::Delete: verb = tr("Eliminando"); break;
    case ActionKind::Undo:   verb = tr("Deshaciendo"); break;
    case ActionKind::Revert: verb = tr("Revirtiendo"); break;
    case ActionKind::Extract: verb = tr("Extrayendo"); break;
    case ActionKind::Compress: verb = tr("Comprimiendo"); break;
    default:                 verb = tr("Trabajando"); break;
    }
    QString text = progress.itemsTotal > 0
        ? tr("%1… %2 de %3").arg(verb).arg(progress.itemsDone).arg(progress.itemsTotal)
        : tr("%1…").arg(verb);
    const ActionKind kind = m_runningKinds.value(id, ActionKind::Copy);
    if (kind == ActionKind::Copy || kind == ActionKind::Move) {
        const TransferInfo total = m_actions->overallTransfer();
        if (total.bytesPerSecond > 0) {
            text += tr(" · %1/s").arg(core::formatSize(qint64(total.bytesPerSecond)));
        }
        if (total.secondsLeft >= 0) {
            text += tr(" · quedan %1").arg(formatDuration(total.secondsLeft));
        }
    }
    if (m_actions->runningCount() > 1) {
        text += tr(" (%n operación(es) en curso)", nullptr, m_actions->runningCount());
    }
    m_progressLabel->setText(text);

    if (progress.bytesTotal > 0) {
        m_progressBar->setRange(0, 1000);
        m_progressBar->setValue(int(progress.bytesDone * 1000 / progress.bytesTotal));
    } else if (progress.itemsTotal > 0) {
        m_progressBar->setRange(0, progress.itemsTotal);
        m_progressBar->setValue(progress.itemsDone);
    } else {
        m_progressBar->setRange(0, 0);
    }
}

void MainWindow::onActionFinished(const ActionResult &result)
{
    m_runningKinds.remove(result.id);
    followTags(result);
    const int requested = m_requestedCounts.take(result.id);
    const ops::Outcome &outcome = result.outcome;

    // 1. Poner a la vista lo que cambió.
    QStringList createdNames;
    const QString directory = QDir::cleanPath(result.directory);
    if (!result.directory.isEmpty()) {
        for (const QString &path : outcome.resultPaths) {
            if (parentPathOf(path) == directory) {
                createdNames.append(path.mid(path.lastIndexOf(QLatin1Char('/')) + 1));
            }
        }
    }
    const bool editName = result.id == m_renameAfterCreateId && outcome.ok();
    for (BrowserPane *pane : allPanes()) {
        if (!createdNames.isEmpty() && pane->currentPath() == directory) {
            pane->selectWhenAvailable(createdNames, editName && pane == currentPane());
        } else {
            // La vigilancia de carpetas suele avisar sola; esto cubre los
            // sistemas de archivos donde no avisa (red, algunos USB).
            pane->contentMayHaveChanged();
        }
    }
    if (result.id == m_renameAfterCreateId) {
        m_renameAfterCreateId = 0;
    }
    // El árbol: la carpeta de la operación y las que quedaron entre ella y lo
    // creado (al organizar aparecen "Documentos" y "Documentos/PDF").
    if (!directory.isEmpty()) {
        QStringList changed{directory};
        for (const QString &path : outcome.resultPaths) {
            for (QString folder = parentPathOf(path); changed.size() < 200 && folder.size() > directory.size()
                                                      && folder.startsWith(directory) && !changed.contains(folder);
                 folder = parentPathOf(folder)) {
                changed.append(folder);
            }
        }
        for (const QString &folder : std::as_const(changed)) {
            m_sidebar->refreshFolder(folder);
        }
    }

    // 2. Enviar a la papelera en un volumen sin papelera: se ofrece la
    //    eliminación permanente, con la confirmación reforzada (SPEC §10.2).
    if (result.kind == ActionKind::Trash && !outcome.errors.isEmpty()) {
        QStringList withoutTrash;
        for (const core::Error &error : outcome.errors) {
            if (error.code == core::ErrorCode::NotSupported && !error.path.isEmpty()) {
                withoutTrash.append(error.path);
            }
        }
        if (withoutTrash.size() == outcome.errors.size()) {
            if (askPermanentDelete(withoutTrash, tr("Esta ubicación no admite papelera."))) {
                m_requestedCounts.insert(m_actions->deletePermanently(withoutTrash), int(withoutTrash.size()));
            } else {
                report(tr("No se eliminó nada: esta ubicación no admite papelera."), false);
            }
            return;
        }
    }

    // 3. Contar qué pasó.
    const int failed = int(outcome.errors.size());
    QString message;
    if (result.kind == ActionKind::Undo && result.nothingToUndo) {
        message = tr("No hay nada que deshacer.");
    } else if (outcome.cancelled) {
        message = tr("Operación cancelada. Lo que ya se había hecho se conserva; se puede quitar con Deshacer.");
    } else {
        switch (result.kind) {
        case ActionKind::Copy:
            message = tr("Se copiaron %n elemento(s).", nullptr, int(outcome.resultPaths.size()));
            break;
        case ActionKind::Move:
            message = tr("Se movieron %n elemento(s).", nullptr, int(outcome.resultPaths.size()));
            break;
        case ActionKind::Trash:
            message = tr("%n elemento(s) enviado(s) a la papelera.", nullptr, qMax(0, requested - failed));
            message += QLatin1Char(' ')
                + tr("Se recupera con Deshacer (%1).").arg(m_undoAction->shortcut().toString(QKeySequence::NativeText));
            break;
        case ActionKind::Delete:
        case ActionKind::PurgeTrash:
            message = tr("%n elemento(s) eliminado(s) permanentemente.", nullptr, qMax(0, requested - failed));
            break;
        case ActionKind::RestoreTrash:
            message = tr("Se restauraron %n elemento(s) a su ubicación original.", nullptr,
                         int(outcome.resultPaths.size()));
            break;
        case ActionKind::Undo:
            if (outcome.ok()) {
                message = tr("Se deshizo: %1").arg(result.undoneDescription);
            }
            break;
        case ActionKind::Revert:
            if (outcome.ok()) {
                message = tr("Se revirtió la operación interrumpida: %1").arg(result.undoneDescription);
            }
            break;
        case ActionKind::Discard:
            message = tr("Se descartó la operación interrumpida: %1").arg(result.undoneDescription);
            break;
        case ActionKind::Extract:
            if (m_extractHereIds.removeAll(result.id) > 0) {
                message = tr("Extraído en «%1»: %n elemento(s) nuevo(s).", nullptr, int(outcome.resultPaths.size()))
                              .arg(QFileInfo(directory).fileName().isEmpty() ? QDir::toNativeSeparators(directory)
                                                                              : QFileInfo(directory).fileName());
                if (!outcome.skipped.isEmpty()) {
                    message += tr(" %n elemento(s) no se extrajeron porque ya existían en la carpeta (no se "
                                  "sobrescribe nada) o no se podían extraer; detalle en el registro.",
                                  nullptr, int(outcome.skipped.size()));
                }
                break;
            }
            if (outcome.ok() && !outcome.resultPaths.isEmpty()) {
                message = tr("Extraído en «%1».").arg(QFileInfo(outcome.resultPaths.first()).fileName());
            }
            if (!outcome.skipped.isEmpty()) {
                // Aquí lo omitido son rutas peligrosas, enlaces que salen o
                // cifrados, no solo nombres repetidos.
                message += tr(" %n elemento(s) omitido(s) por seguridad o porque no se podían extraer (detalle en el "
                              "registro).",
                              nullptr, int(outcome.skipped.size()));
            }
            break;
        case ActionKind::Organize:
            message = tr("Se organizaron %n archivo(s).", nullptr, int(outcome.resultPaths.size()));
            message += QLatin1Char(' ')
                + tr("Se deshace todo junto con Deshacer (%1).").arg(m_undoAction->shortcut().toString(QKeySequence::NativeText));
            break;
        case ActionKind::BulkRename:
            message = tr("Se renombraron %n elemento(s).", nullptr, int(outcome.resultPaths.size()));
            message += QLatin1Char(' ')
                + tr("Se deshace todo junto con Deshacer (%1).").arg(m_undoAction->shortcut().toString(QKeySequence::NativeText));
            break;
        case ActionKind::Compress:
            if (outcome.ok() && !outcome.resultPaths.isEmpty()) {
                message = tr("Creado «%1».").arg(QFileInfo(outcome.resultPaths.first()).fileName());
            }
            break;
        case ActionKind::CreateFolder:
        case ActionKind::CreateFile:
        case ActionKind::Rename:
            break;   // el resultado está a la vista; no hace falta decir nada
        }
    }
    if (!outcome.skipped.isEmpty() && result.kind != ActionKind::Extract) {
        const int identical = int(outcome.identical.size());
        const int others = int(outcome.skipped.size()) - identical;
        if (identical > 0) {
            message += tr(" %n omitido(s) por ser idéntico(s) a lo que ya había.", nullptr, identical);
        }
        if (others > 0) {
            message += tr(" %n omitido(s) porque ya existía(n) en el destino.", nullptr, others);
        }
    }
    if (!outcome.replaced.isEmpty()) {
        message += tr(" %n reemplazado(s); lo anterior está en la papelera.", nullptr, int(outcome.replaced.size()));
    }
    if (outcome.verified > 0) {
        message += tr(" %n comprobado(s) con su hash.", nullptr, outcome.verified);
    }

    if (failed == 0) {
        report(message.trimmed(), false);
        return;
    }
    QStringList details;
    for (const core::Error &error : outcome.errors) {
        details.append(error.path.isEmpty() ? error.message
                                            : QStringLiteral("%1\n    %2").arg(error.message, QDir::toNativeSeparators(error.path)));
    }
    if (!message.trimmed().isEmpty()) {
        details.prepend(message.trimmed());
    }
    if (result.kind == ActionKind::Undo || result.kind == ActionKind::Revert) {
        details.prepend(tr("Al deshacer «%1»:").arg(result.undoneDescription));
    }
    const core::Error &first = outcome.errors.first();
    report(first.path.isEmpty() ? first.message
                                : tr("%1 (%2)").arg(first.message, QDir::toNativeSeparators(first.path)),
           true, details);
}

void MainWindow::report(const QString &message, bool isError, const QStringList &details)
{
    if (!isError) {
        if (!message.isEmpty()) {
            statusBar()->showMessage(message, 8000);
        }
        emit actionReported(message, false);
        return;
    }
    // Los errores no se pierden en la barra de estado: se muestran en un
    // aviso que no bloquea la ventana (se puede seguir trabajando).
    auto *box = new QMessageBox(QMessageBox::Warning, tr("No se pudo completar"), message, QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    if (details.size() > 1) {
        box->setInformativeText(tr("Hubo %n problema(s). El detalle está abajo.", nullptr, int(details.size())));
        box->setDetailedText(details.join(QStringLiteral("\n\n")));
    }
    box->setModal(false);
    box->show();
    statusBar()->showMessage(message, 15000);
    emit actionReported(message, true);
}

void MainWindow::createToolBar()
{
    QToolBar *toolBar = addToolBar(tr("Navegación"));
    toolBar->setObjectName(QStringLiteral("navigationToolBar"));
    toolBar->setMovable(false);
    toolBar->addAction(m_backAction);
    toolBar->addAction(m_forwardAction);
    toolBar->addAction(m_upAction);
    toolBar->addAction(m_refreshAction);

    m_pathBar = new PathBar(toolBar);
    toolBar->addWidget(m_pathBar);
    connect(m_editPathAction, &QAction::triggered, m_pathBar, &PathBar::startEditing);

    // Buscador: busca en la carpeta de la pestaña y en sus subcarpetas, con
    // la misma sintaxis que el filtro (docs/SEARCH_SYNTAX.md).
    m_searchEdit = new QLineEdit(toolBar);
    m_searchEdit->setToolTip(tr("Escribe y pulsa Intro. Ejemplos: factura 2025 · *.pdf · ext:jpg size:>5MB"));
    m_searchEdit->setAccessibleName(tr("Buscar"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedWidth(260);
    m_searchEdit->addAction(m_searchAction->icon(), QLineEdit::LeadingPosition);
    // Flecha al final del buscador (o tecla Abajo): las últimas búsquedas.
    auto *recent = new QAction(themedIcon(QStringLiteral("go-down"), QStyle::SP_ArrowDown),
                               tr("Búsquedas recientes (flecha abajo)"), m_searchEdit);
    recent->setShortcut(QKeySequence(Qt::Key_Down));
    recent->setShortcutContext(Qt::WidgetShortcut);
    m_searchEdit->addAction(recent, QLineEdit::TrailingPosition);
    connect(recent, &QAction::triggered, this, &MainWindow::showRecentSearches);
    toolBar->addWidget(m_searchEdit);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &MainWindow::runSearch);
    m_searchOptionsButton = new QToolButton(toolBar);
    m_searchOptionsButton->setText(tr("Opciones"));
    m_searchOptionsButton->setToolTip(tr("Opciones de búsqueda: dónde buscar, en el nombre o en el contenido, "
                                         "y si se distinguen las mayúsculas"));
    m_searchOptionsButton->setAccessibleName(tr("Opciones de búsqueda"));
    m_searchOptionsButton->setPopupMode(QToolButton::InstantPopup);
    toolBar->addWidget(m_searchOptionsButton);
    buildSearchOptions();
    // Esc devuelve el teclado a la lista.
    auto *leaveSearch = new QAction(m_searchEdit);
    leaveSearch->setShortcut(QKeySequence(Qt::Key_Escape));
    leaveSearch->setShortcutContext(Qt::WidgetShortcut);
    m_searchEdit->addAction(leaveSearch);
    connect(leaveSearch, &QAction::triggered, this, [this] {
        if (auto *pane = currentPane()) pane->focusContent();
    });

    toolBar->addSeparator();
    toolBar->addAction(m_detailsAction);
    toolBar->addAction(m_thumbnailsAction);
    toolBar->addAction(m_columnsAction);
    toolBar->addAction(m_dualPaneAction);
    toolBar->addAction(m_newTabAction);
    toolBar->addAction(m_showTrashAction);
}

BrowserPane *MainWindow::openTab(const QString &path)
{
    return openTabIn(m_tabs, path, true);
}

BrowserPane *MainWindow::openTabIn(QTabWidget *group, const QString &path, bool activate)
{
    auto *pane = new BrowserPane(m_scheduler, m_watcher, group);
    pane->setShowHidden(m_showHiddenAction->isChecked());
    pane->setFolderSizesEnabled(m_folderSizesAction->isChecked());
    pane->setSizeUnits(m_binaryUnitsAction->isChecked() ? core::SizeUnits::Binary
                                                                : core::SizeUnits::Decimal);

    pane->setIndexResolver([this](const QString &folder) { return m_indexService->indexFor(folder); });
    pane->setTagStore(m_tagStore);
    pane->setContentCacheResolver(
        [this](const QStringList &roots) { return m_contentIndexService->cacheFactory(roots); });
    pane->setThumbnailService(m_thumbnailService);
    pane->setThumbnailSize(m_thumbnailSize);
    pane->setInlinePlayer(m_inlinePlayer);
    pane->setInlinePlaybackEnabled(m_inlinePlaybackAction->isChecked());
    // Una pestaña nueva se abre con la vista que se está usando.
    if (BrowserPane *current = currentPane()) {
        pane->setViewMode(current->viewMode());
    } else if (m_thumbnailsAction->isChecked()) {
        pane->setViewMode(BrowserPane::ViewMode::Thumbnails);
    } else if (m_columnsAction->isChecked()) {
        pane->setViewMode(BrowserPane::ViewMode::Columns);
    }

    connect(pane, &BrowserPane::pathChanged, this, [this, pane] {
        updateTabTitle(pane);
        if (pane == currentPane()) {
            m_pathBar->setPath(pane->currentPath());
            m_sidebar->setCurrentPath(pane->currentPath());
            syncSearchBox();
            updateEditActions();
        } else if (pane == otherPane()) {
            updateEditActions();   // «Copiar al otro panel» depende de dónde está el otro
        }
    });
    connect(pane, &BrowserPane::historyChanged, this, &MainWindow::updateNavigationActions);
    connect(pane, &BrowserPane::currentItemChanged, this, [this, pane] {
        if (pane == currentPane()) {
            updatePreview();
        }
    });
    connect(pane, &BrowserPane::statusChanged, this, [this, pane] {
        if (pane == currentPane()) {
            updateStatus();
            updateEditActions();
        }
    });
    // Cada vez que se lee la carpeta a la vista, el árbol la pone al día (carpetas
    // creadas o borradas desde aquí o desde otro programa).
    connect(pane->model(), &DirectoryModel::stateChanged, this, [this, pane](DirectoryModel::State state) {
        if (state == DirectoryModel::State::Ready && pane->showsFolder() && !pane->showsArchive()) {
            m_sidebar->refreshFolder(pane->currentPath());
        }
    });
    connect(pane, &BrowserPane::bulkRenameRequested, this, &MainWindow::showBulkRename);
    connect(pane, &BrowserPane::renameRequested, this, [this](const QString &renamedPath, const QString &newName) {
        m_requestedCounts.insert(m_actions->rename(renamedPath, newName), 1);
    });
    connect(pane, &BrowserPane::contextMenuRequested, this, [this, pane](const QPoint &globalPosition) {
        showContextMenu(pane, globalPosition);
    });
    connect(pane, &BrowserPane::filesDropped, this, &MainWindow::transfer);
    pane->setMoveByDefaultRule([this](const QString &source, const QString &destination) {
        return movesByDefault(source, destination);
    });
    connect(pane, &BrowserPane::openFailed, this, [this](const QString &failedPath) {
        statusBar()->showMessage(tr("No se pudo abrir %1").arg(QDir::toNativeSeparators(failedPath)), 6000);
    });

    const int index = group->addTab(pane, QString());
    pane->navigateTo(path);
    group->setCurrentIndex(index);
    if (activate) {
        setActiveGroup(group);
        pane->focusContent();
    }
    return pane;
}

void MainWindow::closeTab(int index)
{
    closeTabIn(m_tabs, index);
}

void MainWindow::closeTabIn(QTabWidget *group, int index)
{
    if (group == nullptr || index < 0 || index >= group->count()) {
        return;
    }
    // La ✕ de una pestaña con resultados de búsqueda sale primero de la
    // búsqueda (vuelve a la carpeta donde se buscó); otra ✕ cierra la pestaña.
    // Así salir de una búsqueda nunca cierra la ventana sin querer.
    if (auto *searching = qobject_cast<BrowserPane *>(group->widget(index)); searching && searching->showsSearch()) {
        searching->goUp();
        return;
    }
    if (group->count() == 1) {
        if (group == m_rightTabs) {
            setDualPane(false);   // la última del panel derecho cierra el doble panel
            return;
        }
        if (!isDualPane()) {
            // Siempre queda al menos una pestaña: cerrar la última cierra la ventana.
            close();
            return;
        }
        // La última del izquierdo con doble panel: las del derecho pasan a la
        // izquierda y queda un solo panel.
        QWidget *closing = group->widget(index);
        while (m_rightTabs->count() > 0) {
            auto *moved = static_cast<BrowserPane *>(m_rightTabs->widget(0));
            m_rightTabs->removeTab(0);
            m_leftTabs->addTab(moved, QString());
            updateTabTitle(moved);
        }
        m_leftTabs->removeTab(m_leftTabs->indexOf(closing));
        closing->deleteLater();
        setDualPane(false);
        return;
    }
    QWidget *pane = group->widget(index);
    group->removeTab(index);
    pane->deleteLater();
}

QTabWidget *MainWindow::makeTabGroup()
{
    auto *group = new QTabWidget(this);
    group->setDocumentMode(true);
    group->setTabsClosable(true);
    group->setMovable(true);   // reordenar pestañas arrastrando
    for (QAction *action : std::as_const(m_paneActions)) {
        group->addAction(action);
    }
    connect(group, &QTabWidget::currentChanged, this, [this, group] {
        if (group == m_tabs) {
            onCurrentTabChanged();
        }
    });
    connect(group, &QTabWidget::tabBarClicked, this, [this, group] { setActiveGroup(group); });
    connect(group, &QTabWidget::tabCloseRequested, this, [this, group](int index) { closeTabIn(group, index); });
    return group;
}

QWidget *MainWindow::boxFor(QTabWidget *group, QWidget **mark)
{
    auto *box = new QWidget(this);
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    *mark = new QWidget(box);
    (*mark)->setFixedHeight(3);
    (*mark)->setAutoFillBackground(true);
    (*mark)->hide();   // solo con doble panel
    layout->addWidget(*mark);
    layout->addWidget(group, 1);
    return box;
}

QTabWidget *MainWindow::groupOf(const BrowserPane *pane) const
{
    if (m_rightTabs != nullptr && m_rightTabs->indexOf(const_cast<BrowserPane *>(pane)) >= 0) {
        return m_rightTabs;
    }
    return m_leftTabs;
}

QList<BrowserPane *> MainWindow::allPanes() const
{
    QList<BrowserPane *> panes;
    for (QTabWidget *group : {m_leftTabs, m_rightTabs}) {
        if (group == nullptr) {
            continue;
        }
        for (int i = 0; i < group->count(); ++i) {
            if (auto *pane = qobject_cast<BrowserPane *>(group->widget(i))) {
                panes.append(pane);
            }
        }
    }
    return panes;
}

void MainWindow::setActiveGroup(QTabWidget *group)
{
    if (group == nullptr || group == m_tabs) {
        return;
    }
    m_tabs = group;
    updatePaneMarks();
    onCurrentTabChanged();
}

void MainWindow::updatePaneMarks()
{
    // Una franja del color de la selección encima del panel activo.
    const bool dual = isDualPane();
    for (QWidget *mark : {m_leftMark, m_rightMark}) {
        if (mark == nullptr) {
            continue;
        }
        mark->setVisible(dual);
        const bool active = (mark == m_leftMark && m_tabs == m_leftTabs) || (mark == m_rightMark && m_tabs == m_rightTabs);
        QPalette palette = mark->palette();
        palette.setColor(QPalette::Window, active ? palette.color(QPalette::Highlight) : palette.color(QPalette::Mid));
        mark->setPalette(palette);
    }
}

void MainWindow::setDualPane(bool on)
{
    if (on == isDualPane()) {
        if (m_dualPaneAction != nullptr) {
            m_dualPaneAction->setChecked(on);
        }
        return;
    }
    if (on) {
        m_rightTabs = makeTabGroup();
        m_rightBox = boxFor(m_rightTabs, &m_rightMark);
        m_centerSplitter->addWidget(m_rightBox);
        const int half = std::max(m_centerSplitter->width(), 400) / 2;
        m_centerSplitter->setSizes({half, half});
        // El panel nuevo empieza en la misma carpeta; el teclado sigue en el actual.
        const BrowserPane *pane = currentPane();
        const QString path = pane == nullptr ? QDir::homePath()
            : isSearchLocation(pane->currentPath()) ? searchRootOf(pane->currentPath())
                                                    : pane->currentPath();
        openTabIn(m_rightTabs, path, false);
    } else {
        if (m_tabs == m_rightTabs) {
            setActiveGroup(m_leftTabs);
            if (BrowserPane *pane = currentPane()) {
                pane->focusContent();
            }
        }
        QTabWidget *closing = m_rightTabs;
        m_rightTabs = nullptr;   // ya no cuenta: sus avisos se ignoran mientras se cierra
        while (closing->count() > 0) {
            QWidget *pane = closing->widget(0);
            closing->removeTab(0);
            pane->deleteLater();
        }
        m_rightBox->deleteLater();
        m_rightBox = nullptr;
        m_rightMark = nullptr;
    }
    if (m_dualPaneAction != nullptr) {
        m_dualPaneAction->setChecked(on);
    }
    updatePaneMarks();
    updateEditActions();
}

void MainWindow::switchPane()
{
    if (!isDualPane()) {
        return;
    }
    setActiveGroup(m_tabs == m_leftTabs ? m_rightTabs : m_leftTabs);
    if (BrowserPane *pane = currentPane()) {
        pane->focusContent();
    }
}

BrowserPane *MainWindow::otherPane() const
{
    if (!isDualPane()) {
        return nullptr;
    }
    QTabWidget *other = m_tabs == m_leftTabs ? m_rightTabs : m_leftTabs;
    return static_cast<BrowserPane *>(other->currentWidget());
}

void MainWindow::sendSelectionToOtherPane(bool move)
{
    BrowserPane *pane = currentPane();
    BrowserPane *other = otherPane();
    if (pane == nullptr || other == nullptr) {
        return;
    }
    if (!other->showsFolder() || other->showsArchive()) {
        statusBar()->showMessage(tr("El otro panel no muestra una carpeta donde dejar los archivos"), 6000);
        return;
    }
    const QStringList paths = pane->selectedPaths();
    if (!paths.isEmpty()) {
        transfer(paths, other->currentPath(), move);
    }
}

void MainWindow::duplicateCurrentTab()
{
    if (BrowserPane *pane = currentPane()) {
        openTab(pane->currentPath());
    }
}

BrowserPane *MainWindow::currentPane() const
{
    return static_cast<BrowserPane *>(m_tabs->currentWidget());
}

int MainWindow::tabCount() const
{
    return m_tabs->count();
}

void MainWindow::navigateCurrentTo(const QString &path)
{
    if (BrowserPane *pane = currentPane()) {
        pane->navigateTo(path);
        pane->focusContent();
    }
}

void MainWindow::buildSearchOptions()
{
    auto *menu = new QMenu(m_searchOptionsButton);
    m_searchOptionsButton->setMenu(menu);

    menu->addSection(tr("Dónde buscar"));
    auto *scopes = new QActionGroup(menu);
    const auto addScope = [&](SearchScope scope, const QString &text) {
        QAction *action = menu->addAction(text);
        action->setCheckable(true);
        action->setChecked(scope == m_searchScope);
        scopes->addAction(action);
        m_scopeActions.insert(int(scope), action);
        connect(action, &QAction::triggered, this, [this, scope] { setSearchScope(scope); });
    };
    addScope(SearchScope::Folder, tr("Solo en esta carpeta"));
    addScope(SearchScope::Subfolders, tr("En esta carpeta y sus subcarpetas"));
    addScope(SearchScope::Volume, tr("En toda esta unidad"));
    addScope(SearchScope::AllVolumes, tr("En todas las unidades"));
    addScope(SearchScope::ChosenVolumes, tr("En las unidades que elija…"));
    // Elegir unidades pide cuáles; si se cancela, se queda el alcance anterior.
    disconnect(m_scopeActions.value(int(SearchScope::ChosenVolumes)), &QAction::triggered, this, nullptr);
    connect(m_scopeActions.value(int(SearchScope::ChosenVolumes)), &QAction::triggered, this, [this] {
        if (chooseSearchVolumes()) {
            setSearchScope(SearchScope::ChosenVolumes);
        } else {
            m_scopeActions.value(int(m_searchScope))->setChecked(true);
        }
    });

    menu->addSection(tr("Qué se compara"));
    auto *targets = new QActionGroup(menu);
    m_searchInNameAction = menu->addAction(tr("El nombre"));
    m_searchInNameAction->setCheckable(true);
    m_searchInNameAction->setChecked(true);
    targets->addAction(m_searchInNameAction);
    m_searchInContentAction = menu->addAction(tr("El contenido (texto, PDF y documentos)"));
    m_searchInContentAction->setCheckable(true);
    m_searchInContentAction->setToolTip(tr("Busca las palabras dentro de los archivos: texto y código, PDF, Word, "
                                           "Excel, PowerPoint, OpenDocument y RTF. Lee cada archivo: en carpetas "
                                           "grandes tarda."));
    targets->addAction(m_searchInContentAction);
    connect(targets, &QActionGroup::triggered, this, &MainWindow::updateSearchHints);

    menu->addSeparator();
    m_searchCaseAction = menu->addAction(tr("Distinguir mayúsculas y minúsculas"));
    menu->addSeparator();
    menu->addAction(m_indexAction);
    menu->addAction(m_contentIndexAction);
    m_searchCaseAction->setCheckable(true);
    connect(m_searchCaseAction, &QAction::toggled, this, &MainWindow::updateSearchHints);
    menu->setToolTipsVisible(true);
    updateSearchHints();
}

void MainWindow::startIndexing(bool mayAsk)
{
    QSettings settings;
    const QStringList roots = settings.value(QStringLiteral("index/roots")).toStringList();
    for (const QString &root : roots) {
        m_indexService->addRoot(root);
    }
    for (const QString &root : settings.value(QStringLiteral("contenido/carpetas")).toStringList()) {
        m_contentIndexService->addRoot(root);
    }
    if (mayAsk && !settings.value(QStringLiteral("index/asked"), false).toBool()) {
        // Con la ventana ya a la vista, no antes.
        QTimer::singleShot(0, this, &MainWindow::runIndexWizard);
    }
}

void MainWindow::saveIndexRoots()
{
    QSettings().setValue(QStringLiteral("index/roots"), m_indexService->roots());
}

void MainWindow::runIndexWizard()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("¿Crear un índice para buscar al instante?"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(
        tr("Explorador Ultra puede anotar los nombres de los archivos de tus unidades para que las búsquedas "
           "en una unidad entera tarden décimas de segundo en vez de minutos.\n\n"
           "El índice se crea en segundo plano, se puede pausar, se guarda solo en este equipo y no incluye el "
           "contenido de los archivos. Es opcional: sin él todo funciona igual, solo que más despacio.\n\n"
           "Marca las unidades que quieras indexar:"), &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *list = new QListWidget(&dialog);
    for (const fs::Volume &volume : searchVolumes()) {
        auto *item = new QListWidgetItem(
            iconForVolume(volume),
            tr("%1 — %2 (%3)").arg(volume.displayName, QDir::toNativeSeparators(volume.rootPath),
                                   core::formatSize(volume.totalBytes)), list);
        item->setData(Qt::UserRole, volume.rootPath);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        // De fábrica, los discos fijos. Lo extraíble, de red u óptico, solo si se pide.
        const bool fixedDisk = volume.kind == fs::DeviceKind::Ssd || volume.kind == fs::DeviceKind::Hdd
            || volume.kind == fs::DeviceKind::Unknown;
        item->setCheckState(fixedDisk ? Qt::Checked : Qt::Unchecked);
    }
    layout->addWidget(list);
    auto *later = new QLabel(tr("Puedes cambiarlo cuando quieras en Ver → Índice de búsqueda."), &dialog);
    later->setWordWrap(true);
    layout->addWidget(later);
    auto *buttons = new QDialogButtonBox(&dialog);
    QPushButton *accept = buttons->addButton(tr("Indexar las marcadas"), QDialogButtonBox::AcceptRole);
    QPushButton *postpone = buttons->addButton(tr("Ahora no"), QDialogButtonBox::RejectRole);
    QPushButton *never = buttons->addButton(tr("No, y no volver a preguntar"), QDialogButtonBox::DestructiveRole);
    accept->setDefault(true);
    layout->addWidget(buttons);
    enum Answer { Postpone, Accept, Never };
    Answer answer = Postpone;
    connect(accept, &QPushButton::clicked, &dialog, [&] { answer = Accept; dialog.accept(); });
    connect(postpone, &QPushButton::clicked, &dialog, [&] { answer = Postpone; dialog.reject(); });
    connect(never, &QPushButton::clicked, &dialog, [&] { answer = Never; dialog.reject(); });
    dialog.resize(560, 420);
    dialog.exec();

    if (answer == Postpone) {
        return;   // se volverá a proponer en el próximo arranque
    }
    QSettings().setValue(QStringLiteral("index/asked"), true);
    if (answer == Accept) {
        for (int row = 0; row < list->count(); ++row) {
            if (list->item(row)->checkState() == Qt::Checked) {
                m_indexService->addRoot(list->item(row)->data(Qt::UserRole).toString());
            }
        }
        saveIndexRoots();
    }
}

void MainWindow::saveContentIndexRoots()
{
    QSettings().setValue(QStringLiteral("contenido/carpetas"), m_contentIndexService->roots());
}

void MainWindow::showContentIndexDialog()
{
    BrowserPane *pane = currentPane();
    // La carpeta a la vista; si es una búsqueda, la carpeta donde se buscó.
    QString folder = pane != nullptr ? pane->currentPath() : QString();
    if (isSearchLocation(folder)) {
        folder = searchSpecOf(folder).origin;
    }
    if (isVirtualLocation(folder)) {
        folder.clear();
    }
    ContentIndexDialog dialog(*m_contentIndexService, folder, this);
    connect(&dialog, &ContentIndexDialog::rootsChanged, this, &MainWindow::saveContentIndexRoots);
    dialog.exec();
}

void MainWindow::showIndexDialog()
{
    IndexDialog dialog(*m_indexService, [this] { return searchVolumes(); }, this);
    connect(&dialog, &IndexDialog::rootsChanged, this, &MainWindow::saveIndexRoots);
    dialog.exec();
    // Quien abre esta ventana ya ha decidido: el asistente no hace falta.
    QSettings().setValue(QStringLiteral("index/asked"), true);
}

void MainWindow::updateIndexLabel()
{
    QStringList working;
    for (const index::RootStatus &status : m_indexService->status()) {
        if (status.state == index::RootStatus::State::Building
            || status.state == index::RootStatus::State::Updating) {
            working.append(tr("%1: %2").arg(QDir::toNativeSeparators(status.root), indexStateText(status)));
        }
    }
    for (const index::ContentRootStatus &status : m_contentIndexService->status()) {
        if (status.state == index::ContentRootStatus::State::Building
            || status.state == index::ContentRootStatus::State::Updating) {
            working.append(tr("contenido de %1: %2")
                               .arg(QDir::toNativeSeparators(status.root), contentIndexStateText(status)));
        }
    }
    if (m_indexService->isPaused() && m_indexService->isBusy()) {
        m_indexLabel->setText(tr("Índice en pausa"));
    } else {
        m_indexLabel->setText(working.isEmpty() ? QString() : tr("Índice — %1").arg(working.first()));
    }
    m_indexLabel->setToolTip(working.join(QLatin1Char('\n')));
    m_indexLabel->setVisible(!m_indexLabel->text().isEmpty());
}

void MainWindow::rememberSearch(const QString &text)
{
    m_recentSearches.removeAll(text);
    m_recentSearches.prepend(text);
    while (m_recentSearches.size() > 15) {
        m_recentSearches.removeLast();
    }
}

void MainWindow::showRecentSearches()
{
    QMenu menu(this);
    if (m_recentSearches.isEmpty()) {
        menu.addAction(tr("Aún no hay búsquedas recientes"))->setEnabled(false);
    }
    for (const QString &text : std::as_const(m_recentSearches)) {
        QAction *action = menu.addAction(QString(text).replace(QLatin1Char('&'), QStringLiteral("&&")));
        connect(action, &QAction::triggered, this, [this, text] {
            m_searchEdit->setText(text);
            runSearch();   // con las opciones y en la carpeta de ahora
        });
    }
    if (!m_recentSearches.isEmpty()) {
        menu.addSeparator();
        connect(menu.addAction(tr("Borrar las búsquedas recientes")), &QAction::triggered, this,
                [this] { m_recentSearches.clear(); });
    }
    menu.setMinimumWidth(m_searchEdit->width());
    menu.exec(m_searchEdit->mapToGlobal(QPoint(0, m_searchEdit->height())));
}

void MainWindow::setSearchScope(SearchScope scope)
{
    m_searchScope = scope;
    if (QAction *action = m_scopeActions.value(int(scope))) {
        action->setChecked(true);
    }
    updateSearchHints();
}

void MainWindow::setSearchInContent(bool inContent)
{
    (inContent ? m_searchInContentAction : m_searchInNameAction)->setChecked(true);
    updateSearchHints();
}

void MainWindow::setSearchCaseSensitive(bool caseSensitive)
{
    m_searchCaseAction->setChecked(caseSensitive);
}

void MainWindow::updateSearchHints()
{
    if (m_searchEdit == nullptr || m_searchCaseAction == nullptr) {
        return;
    }
    // El propio buscador dice qué va a hacer, para no tener que abrir el menú.
    QString where;
    switch (m_searchScope) {
    case SearchScope::Folder:        where = tr("Buscar solo en esta carpeta"); break;
    case SearchScope::Subfolders:    where = tr("Buscar aquí y en subcarpetas"); break;
    case SearchScope::Volume:        where = tr("Buscar en toda la unidad"); break;
    case SearchScope::AllVolumes:    where = tr("Buscar en todas las unidades"); break;
    case SearchScope::ChosenVolumes: where = tr("Buscar en las unidades elegidas"); break;
    }
    if (m_searchInContentAction->isChecked()) {
        where += tr(", en el contenido");
    }
    m_searchEdit->setPlaceholderText(where + tr(" (Ctrl+F)"));
    const bool changed = m_searchScope != SearchScope::Subfolders || m_searchInContentAction->isChecked()
        || m_searchCaseAction->isChecked();
    // Con opciones distintas de las habituales, el botón lo avisa.
    m_searchOptionsButton->setText(changed ? tr("Opciones •") : tr("Opciones"));
}

QList<fs::Volume> MainWindow::searchVolumes() const
{
    return m_searchVolumeSource ? m_searchVolumeSource() : m_volumeMonitor.volumes();
}

bool MainWindow::chooseSearchVolumes()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Unidades donde buscar"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Marca las unidades en las que quieres buscar:"), &dialog));
    auto *list = new QListWidget(&dialog);
    for (const fs::Volume &volume : searchVolumes()) {
        auto *item = new QListWidgetItem(
            tr("%1 — %2").arg(volume.displayName, QDir::toNativeSeparators(volume.rootPath)), list);
        item->setData(Qt::UserRole, volume.rootPath);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(m_chosenVolumes.contains(volume.rootPath) ? Qt::Checked : Qt::Unchecked);
    }
    layout->addWidget(list);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Aceptar"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancelar"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    QStringList chosen;
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->checkState() == Qt::Checked) {
            chosen.append(list->item(row)->data(Qt::UserRole).toString());
        }
    }
    if (chosen.isEmpty()) {
        return false;
    }
    m_chosenVolumes = chosen;
    return true;
}

void MainWindow::runSearch()
{
    BrowserPane *pane = currentPane();
    const QString text = m_searchEdit->text().trimmed();
    if (pane == nullptr || text.isEmpty()) {
        return;
    }
    SearchSpec spec;
    spec.query = text;
    spec.caseSensitive = m_searchCaseAction->isChecked();
    spec.inContent = m_searchInContentAction->isChecked();
    // Desde unos resultados se busca donde se buscó la primera vez.
    spec.origin = pane->showsSearch() ? searchRootOf(pane->currentPath()) : pane->currentPath();
    spec.scope = m_searchScope;
    // En la lista de unidades no hay carpeta: se busca en las unidades.
    if (isVolumesLocation(spec.origin) && spec.scope != SearchScope::ChosenVolumes) {
        spec.scope = SearchScope::AllVolumes;
    }

    const QList<fs::Volume> volumes = searchVolumes();
    switch (spec.scope) {
    case SearchScope::Folder:
    case SearchScope::Subfolders:
        spec.roots = QStringList{spec.origin};
        break;
    case SearchScope::Volume: {
        // La unidad de la carpeta: el punto de montaje más largo que la contiene.
        QString best;
        for (const fs::Volume &volume : volumes) {
            const QString root = volume.rootPath;
            const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
            if ((spec.origin == root || spec.origin.startsWith(prefix)) && root.size() > best.size()) {
                best = root;
            }
        }
        if (!best.isEmpty()) {
            spec.roots = QStringList{best};
        }
        break;
    }
    case SearchScope::AllVolumes:
        for (const fs::Volume &volume : volumes) {
            spec.roots.append(volume.rootPath);
        }
        break;
    case SearchScope::ChosenVolumes:
        for (const fs::Volume &volume : volumes) {
            if (m_chosenVolumes.contains(volume.rootPath)) {
                spec.roots.append(volume.rootPath);   // solo las que siguen conectadas
            }
        }
        break;
    }
    if (spec.roots.isEmpty()) {
        statusBar()->showMessage(tr("No se puede buscar: no hay ninguna unidad conectada para ese alcance."), 10000);
        return;
    }

    // Una consulta mal escrita se explica aquí, sin salir de la carpeta.
    search::QueryOptions queryOptions;
    queryOptions.caseSensitive = spec.caseSensitive;
    queryOptions.termsInContent = spec.inContent;
    const core::Result<search::Query> parsedQuery = search::Query::parse(text, queryOptions);
    if (!parsedQuery.ok()) {
        statusBar()->showMessage(tr("No se puede buscar: %1").arg(parsedQuery.error().message), 10000);
        return;
    }
    rememberSearch(text);
    pane->search(spec);
    pane->focusContent();
}

void MainWindow::syncSearchBox()
{
    // El buscador enseña la consulta de la pestaña que se está viendo; fuera
    // de unos resultados queda vacío.
    const BrowserPane *pane = currentPane();
    if (m_searchEdit == nullptr || m_searchEdit->hasFocus()) {
        return;
    }
    // Al pasar a una carpeta el texto se queda: lo normal es querer repetir
    // la misma búsqueda en otro sitio. Se borra con la X del buscador.
    if (pane != nullptr && pane->showsSearch()) {
        m_searchEdit->setText(searchQueryOf(pane->currentPath()));
    }
}

void MainWindow::setViewMode(BrowserPane::ViewMode mode)
{
    if (BrowserPane *pane = currentPane()) {
        pane->setViewMode(mode);
    }
    syncViewActions();
}

void MainWindow::setThumbnailSize(int size)
{
    m_thumbnailSize = size;
    for (BrowserPane *pane : allPanes()) {
        pane->setThumbnailSize(size);
    }
    syncViewActions();
}

void MainWindow::syncViewActions()
{
    const BrowserPane *pane = currentPane();
    const BrowserPane::ViewMode mode = pane != nullptr ? pane->viewMode() : BrowserPane::ViewMode::Details;
    (mode == BrowserPane::ViewMode::Thumbnails ? m_thumbnailsAction
     : mode == BrowserPane::ViewMode::Columns  ? m_columnsAction
                                               : m_detailsAction)
        ->setChecked(true);
    if (QAction *size = m_thumbnailSizeActions.value(m_thumbnailSize)) {
        size->setChecked(true);
    }
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (m_firstShowDone) {
        return;
    }
    m_firstShowDone = true;
    // Anchos de fábrica de los tres paneles, ya con el tamaño real de la
    // ventana (antes de mostrarla no se conoce y el panel derecho quedaba a 0).
    if (!m_splitterRestored) {
        const int total = std::max(m_splitter->width(), 900);
        m_splitter->setSizes({220, total - 220 - 300, 300});
    }
}

void MainWindow::setPreviewVisible(bool visible)
{
    if (m_previewAction->isChecked() != visible) {
        m_previewAction->setChecked(visible);   // vuelve aquí por la señal
        return;
    }
    m_previewPanel->setVisible(visible);
    updatePreview();
}

void MainWindow::updatePreview()
{
    if (m_previewPanel == nullptr || !m_previewPanel->isVisibleTo(this)) {
        return;   // oculto: no se lee nada del disco
    }
    const BrowserPane *pane = currentPane();
    const BrowserPane::CurrentItem item = pane != nullptr ? pane->currentItem() : BrowserPane::CurrentItem();
    if (!item.valid) {
        m_previewPanel->clear(item.selectedCount > 1 ? tr("%1 elementos seleccionados").arg(item.selectedCount)
                                                     : QString());
        return;
    }
    PreviewItem preview;
    preview.path = item.path;
    preview.entry = item.entry;
    preview.typeText = item.typeText;
    preview.selectedCount = item.selectedCount;
    m_previewPanel->showItem(preview);
}

void MainWindow::onCurrentTabChanged()
{
    syncViewActions();
    updatePreview();
    if (BrowserPane *pane = currentPane()) {
        m_pathBar->setPath(pane->currentPath());
        m_sidebar->setCurrentPath(pane->currentPath());
    }
    syncSearchBox();
    updateNavigationActions();
    updateStatus();
    updateEditActions();
}

void MainWindow::updateNavigationActions()
{
    const BrowserPane *pane = currentPane();
    m_backAction->setEnabled(pane && pane->canGoBack());
    m_forwardAction->setEnabled(pane && pane->canGoForward());
    m_upAction->setEnabled(pane && pane->canGoUp());
}

void MainWindow::updateStatus()
{
    const BrowserPane *pane = currentPane();
    m_statusLabel->setText(pane ? pane->statusText() : QString());
    // La convención de tamaños en uso siempre está a la vista (SPEC §5.4).
    m_unitsLabel->setText(m_binaryUnitsAction->isChecked() ? tr("Unidades binarias (1 KiB = 1024 B)")
                                                           : tr("Unidades decimales (1 kB = 1000 B)"));
}

void MainWindow::updateTabTitle(BrowserPane *pane)
{
    const int index = groupOf(pane)->indexOf(pane);
    if (index < 0) {
        return;
    }
    const QString path = pane->currentPath();
    QString title = pane->showsVolumes() ? tr("Unidades")
        : pane->showsSearch()            ? tr("Buscar: %1").arg(searchQueryOf(path))
        : pane->showsArchive()
            ? (archiveInnerOf(path).isEmpty() ? QFileInfo(archivePathOf(path)).fileName()
                                              : archiveInnerOf(path).section(QLatin1Char('/'), -1))
            : QDir(path).dirName();
    if (title.isEmpty()) {
        title = QDir::toNativeSeparators(path);   // raíces: "/" o "C:\"
    }
    groupOf(pane)->setTabText(index, title);
    groupOf(pane)->setTabToolTip(index, pane->showsVolumes() ? title
                                 : pane->showsSearch()
                                     ? tr("%1 en %2").arg(title, pane->searchWhere())
                                     : QDir::toNativeSeparators(path));
    if (pane == currentPane()) {
        setWindowTitle(QStringLiteral("%1 — %2").arg(title, core::applicationName()));
    }
}

void MainWindow::restoreSession()
{
    loadPreferences();
    QSettings settings;
    const QStringList paths = settings.value(QStringLiteral("session/tabs")).toStringList();
    for (const QString &path : paths) {
        // Si la carpeta ya no existe, la pestaña lo indica y ofrece reintentar;
        // no se bloquea el arranque (SPEC §5.7).
        openTab(path);
    }
    if (m_tabs->count() == 0) {
        openTab(QDir::homePath());
    } else {
        m_tabs->setCurrentIndex(qBound(0, settings.value(QStringLiteral("session/currentTab"), 0).toInt(),
                                       m_tabs->count() - 1));
    }
    // El doble panel, con sus pestañas y el panel que estaba activo.
    const QStringList rightPaths = settings.value(QStringLiteral("session/rightTabs")).toStringList();
    if (settings.value(QStringLiteral("session/dualPane"), false).toBool() && !rightPaths.isEmpty()) {
        setDualPane(true);
        // setDualPane abrió una pestaña en la carpeta actual: se sustituye por las guardadas.
        QWidget *initial = m_rightTabs->widget(0);
        for (const QString &path : rightPaths) {
            openTabIn(m_rightTabs, path, false);
        }
        m_rightTabs->removeTab(m_rightTabs->indexOf(initial));
        initial->deleteLater();
        m_rightTabs->setCurrentIndex(qBound(0, settings.value(QStringLiteral("session/rightCurrentTab"), 0).toInt(),
                                            m_rightTabs->count() - 1));
        const QByteArray center = settings.value(QStringLiteral("window/center")).toByteArray();
        if (!center.isEmpty()) {
            m_centerSplitter->restoreState(center);
        }
        if (settings.value(QStringLiteral("session/rightActive"), false).toBool()) {
            setActiveGroup(m_rightTabs);
        }
    }
    updateStatus();
}

void MainWindow::loadPreferences()
{
    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
    // "splitter3": la disposición con el panel de previsualización. La antigua,
    // de dos paneles, dejaría el nuevo con ancho cero; se ignora.
    m_splitterRestored = m_splitter->restoreState(settings.value(QStringLiteral("window/splitter3")).toByteArray());
    m_showHiddenAction->setChecked(settings.value(QStringLiteral("view/showHidden"), false).toBool());
    m_binaryUnitsAction->setChecked(settings.value(QStringLiteral("view/binaryUnits"), false).toBool());
    m_folderSizesAction->setChecked(settings.value(QStringLiteral("view/folderSizes"), false).toBool());
    setThumbnailSize(settings.value(QStringLiteral("view/thumbnailSize"), 160).toInt());
    m_previewAction->setChecked(settings.value(QStringLiteral("view/preview"), true).toBool());
    if (InlinePlayer::isAvailable()) {
        m_inlinePlaybackAction->setChecked(settings.value(QStringLiteral("view/inlinePlayback"), true).toBool());
    }

    const QString mode = settings.value(QStringLiteral("view/mode")).toString();
    (mode == QLatin1String("miniaturas") ? m_thumbnailsAction
     : mode == QLatin1String("columnas") ? m_columnsAction
                                         : m_detailsAction)
        ->setChecked(true);
    // El alcance y "en el contenido" no se guardan: una búsqueda en todos los
    // discos no debe heredarse sin querer de la sesión anterior.
    m_searchCaseAction->setChecked(settings.value(QStringLiteral("search/caseSensitive"), false).toBool());
    m_recentSearches = settings.value(QStringLiteral("search/recent")).toStringList();
}

void MainWindow::saveSession()
{
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/splitter3"), m_splitter->saveState());
    settings.setValue(QStringLiteral("view/showHidden"), m_showHiddenAction->isChecked());
    settings.setValue(QStringLiteral("view/binaryUnits"), m_binaryUnitsAction->isChecked());
    settings.setValue(QStringLiteral("view/folderSizes"), m_folderSizesAction->isChecked());
    settings.setValue(QStringLiteral("view/thumbnailSize"), m_thumbnailSize);
    settings.setValue(QStringLiteral("view/preview"), m_previewAction->isChecked());
    if (InlinePlayer::isAvailable()) {
        settings.setValue(QStringLiteral("view/inlinePlayback"), m_inlinePlaybackAction->isChecked());
    }
    settings.setValue(QStringLiteral("view/mode"), m_thumbnailsAction->isChecked() ? QStringLiteral("miniaturas")
                                                   : m_columnsAction->isChecked()  ? QStringLiteral("columnas")
                                                                                   : QStringLiteral("detalles"));
    settings.setValue(QStringLiteral("search/caseSensitive"), m_searchCaseAction->isChecked());
    settings.setValue(QStringLiteral("search/recent"), m_recentSearches);

    // Unos resultados no se guardan: al volver se abre la carpeta donde se buscó.
    const auto pathsOf = [](const QTabWidget *group) {
        QStringList paths;
        for (int i = 0; group != nullptr && i < group->count(); ++i) {
            const QString path = static_cast<BrowserPane *>(group->widget(i))->currentPath();
            paths.append(isSearchLocation(path) ? searchRootOf(path) : path);
        }
        return paths;
    };
    settings.setValue(QStringLiteral("session/tabs"), pathsOf(m_leftTabs));
    settings.setValue(QStringLiteral("session/currentTab"), m_leftTabs->currentIndex());
    settings.setValue(QStringLiteral("session/dualPane"), isDualPane());
    settings.setValue(QStringLiteral("session/rightTabs"), pathsOf(m_rightTabs));
    settings.setValue(QStringLiteral("session/rightCurrentTab"), isDualPane() ? m_rightTabs->currentIndex() : 0);
    settings.setValue(QStringLiteral("session/rightActive"), isDualPane() && m_tabs == m_rightTabs);
    if (isDualPane()) {
        settings.setValue(QStringLiteral("window/center"), m_centerSplitter->saveState());
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_actions->isBusy()) {
        // Cerrar a mitad de una copia no daña nada (la operación se detiene de
        // forma ordenada y queda en el historial), pero se pregunta antes.
        QMessageBox box(QMessageBox::Question, tr("Operaciones en curso"),
                        tr("Hay operaciones de archivos sin terminar."), QMessageBox::NoButton, this);
        box.setInformativeText(tr("Si sales ahora se detienen. Lo que ya se hizo se conserva y se puede deshacer."));
        QPushButton *stopButton = box.addButton(tr("Detener y salir"), QMessageBox::DestructiveRole);
        QPushButton *waitButton = box.addButton(tr("Seguir trabajando"), QMessageBox::RejectRole);
        box.setDefaultButton(waitButton);
        box.setEscapeButton(waitButton);
        box.exec();
        if (box.clickedButton() != stopButton) {
            event->ignore();
            return;
        }
        m_actions->cancelAll();
    }
    saveSession();
    QMainWindow::closeEvent(event);
}

} // namespace eu::ui
