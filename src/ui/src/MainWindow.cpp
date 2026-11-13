// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/MainWindow.h"

#include "eu/ui/Locations.h"

#include "eu/core/Version.h"
#include "eu/ui/BrowserPane.h"
#include "eu/ui/FileClipboard.h"
#include "eu/ui/PathBar.h"
#include "eu/ui/Sidebar.h"
#include "eu/ui/TrashDialog.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>

namespace eu::ui {

namespace {

QIcon themedIcon(const QString &name, QStyle::StandardPixmap fallback)
{
    return QIcon::fromTheme(name, QApplication::style()->standardIcon(fallback));
}

} // namespace

MainWindow::MainWindow(tasks::Scheduler &scheduler, std::shared_ptr<journal::Journal> journal, QWidget *parent)
    : QMainWindow(parent), m_scheduler(scheduler)
{
    m_actions = new FileActions(m_scheduler, std::move(journal), this);
    setWindowTitle(core::applicationName());
    resize(1180, 720);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);   // reordenar pestañas arrastrando

    m_sidebar = new Sidebar(m_scheduler, m_volumeMonitor, this);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->addWidget(m_sidebar);
    m_splitter->addWidget(m_tabs);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({230, 950});
    setCentralWidget(m_splitter);

    m_statusLabel = new QLabel(this);
    m_unitsLabel = new QLabel(this);
    statusBar()->addWidget(m_statusLabel, 1);
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

    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    // Elegir un lugar en el árbol no le quita el teclado: las flechas siguen
    // moviéndose por el árbol, no por la lista de archivos.
    connect(m_sidebar, &Sidebar::placeActivated, this, [this](const QString &path) {
        if (BrowserPane *pane = currentPane()) {
            pane->navigateTo(path);
        }
    });
    connect(m_sidebar, &Sidebar::filesDropped, this, &MainWindow::transfer);
    connect(m_sidebar, &Sidebar::contextMenuRequested, this, &MainWindow::showSidebarMenu);
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
        for (int i = 0; i < m_tabs->count(); ++i) {
            static_cast<BrowserPane *>(m_tabs->widget(i))->setShowHidden(show);
        }
    });

    m_binaryUnitsAction = new QAction(tr("Tamaños en unidades binarias (KiB, MiB…)"), this);
    m_binaryUnitsAction->setCheckable(true);
    connect(m_binaryUnitsAction, &QAction::toggled, this, [this](bool binary) {
        const core::SizeUnits units = binary ? core::SizeUnits::Binary : core::SizeUnits::Decimal;
        if (m_trashDialog != nullptr) {
            m_trashDialog->setSizeUnits(units);
        }
        for (int i = 0; i < m_tabs->count(); ++i) {
            static_cast<BrowserPane *>(m_tabs->widget(i))->setSizeUnits(units);
        }
        updateStatus();
    });

    m_folderSizesAction = new QAction(tr("Calcular el tamaño de las carpetas"), this);
    m_folderSizesAction->setCheckable(true);
    m_folderSizesAction->setToolTip(tr("Suma en segundo plano lo que ocupa cada carpeta; se detiene al cambiar de carpeta"));
    connect(m_folderSizesAction, &QAction::toggled, this, [this](bool enabled) {
        for (int i = 0; i < m_tabs->count(); ++i) {
            static_cast<BrowserPane *>(m_tabs->widget(i))->setFolderSizesEnabled(enabled);
        }
    });

    m_filterAction = new QAction(themedIcon(QStringLiteral("view-filter"), QStyle::SP_FileDialogContentsView),
                                 tr("Filtrar esta carpeta"), this);
    m_filterAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    m_filterAction->setToolTip(tr("Deja a la vista solo los elementos cuyo nombre coincide con lo que escribas"));
    connect(m_filterAction, &QAction::triggered, this, [this] { if (auto *pane = currentPane()) pane->showFilter(); });

    m_searchAction = new QAction(themedIcon(QStringLiteral("edit-find"), QStyle::SP_FileDialogContentsView),
                                 tr("Buscar en esta carpeta y sus subcarpetas"), this);
    m_searchAction->setShortcut(QKeySequence::Find);
    connect(m_searchAction, &QAction::triggered, this, [this] {
        m_searchEdit->setFocus(Qt::ShortcutFocusReason);
        m_searchEdit->selectAll();
    });

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
    viewMenu->addAction(m_refreshAction);
    viewMenu->addAction(m_filterAction);
    viewMenu->addAction(m_searchAction);
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
        m_tabs->addAction(action);
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
    editMenu->addAction(m_selectAllAction);
    editMenu->addSeparator();
    editMenu->addAction(m_renameAction);
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
    auto *layout = new QHBoxLayout(m_progressBox);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_progressLabel);
    layout->addWidget(m_progressBar);
    layout->addWidget(m_cancelButton);
    statusBar()->addPermanentWidget(m_progressBox);
    m_progressBox->hide();

    connect(m_cancelButton, &QToolButton::clicked, m_actions, &FileActions::cancelAll);
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
    const bool canModify = inFolder && m_actions->isAvailable();
    const bool canRemove = pane != nullptr && m_actions->isAvailable();
    m_filterAction->setEnabled(inFolder);
    m_searchAction->setEnabled(pane != nullptr && !pane->showsVolumes());
    if (m_searchEdit != nullptr) {
        m_searchEdit->setEnabled(m_searchAction->isEnabled());
    }
    const int selected = pane ? pane->selectedCount() : 0;
    const bool canPaste = !fileSelectionFrom(QGuiApplication::clipboard()->mimeData()).isEmpty();

    m_openAction->setEnabled(selected > 0);
    m_copyAction->setEnabled(selected > 0);
    m_cutAction->setEnabled(canRemove && selected > 0);
    m_pasteAction->setEnabled(canModify && canPaste);
    m_renameAction->setEnabled(canModify && selected == 1);
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
    if (pane->selectedCount() > 0) {
        menu.addAction(m_openAction);
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
    if (BrowserPane *pane = currentPane()) {
        pane->startRename();
    }
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
        // Copiar sobre la misma carpeta crea "nombre (2)": nunca se sobrescribe.
        m_requestedCounts.insert(m_actions->copy(paths, destination, ops::CollisionPolicy::AutoRename),
                                 int(paths.size()));
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
    m_requestedCounts.insert(m_actions->move(toMove, destination, ops::CollisionPolicy::Skip), int(toMove.size()));
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
                    "• Reanudar donde se quedó aún no está disponible (próximamente)."));
    box.setInformativeText(lines.join(QLatin1Char('\n')));

    QPushButton *revertButton = box.addButton(tr("Revertir"), QMessageBox::AcceptRole);
    QPushButton *discardButton = box.addButton(tr("Descartar"), QMessageBox::DestructiveRole);
    // Se muestra deshabilitado y rotulado: no aparenta funcionar (SPEC §25.1).
    QPushButton *resumeButton = box.addButton(tr("Reanudar (próximamente)"), QMessageBox::ActionRole);
    resumeButton->setEnabled(false);
    QPushButton *laterButton = box.addButton(tr("Decidir más tarde"), QMessageBox::RejectRole);
    box.setDefaultButton(revertButton);
    box.setEscapeButton(laterButton);
    box.exec();
    if (box.clickedButton() == revertButton) {
        return RecoveryChoice::Revert;
    }
    if (box.clickedButton() == discardButton) {
        return RecoveryChoice::Discard;
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
    default:                 verb = tr("Trabajando"); break;
    }
    QString text = progress.itemsTotal > 0
        ? tr("%1… %2 de %3").arg(verb).arg(progress.itemsDone).arg(progress.itemsTotal)
        : tr("%1…").arg(verb);
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
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *pane = static_cast<BrowserPane *>(m_tabs->widget(i));
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
        case ActionKind::CreateFolder:
        case ActionKind::CreateFile:
        case ActionKind::Rename:
            break;   // el resultado está a la vista; no hace falta decir nada
        }
    }
    if (!outcome.skipped.isEmpty()) {
        message += tr(" %n omitido(s) porque ya existía(n) en el destino.", nullptr, int(outcome.skipped.size()));
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
    m_searchEdit->setPlaceholderText(tr("Buscar aquí y en subcarpetas (Ctrl+F)"));
    m_searchEdit->setToolTip(tr("Escribe y pulsa Intro. Ejemplos: factura 2025 · *.pdf · ext:jpg size:>5MB"));
    m_searchEdit->setAccessibleName(tr("Buscar"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedWidth(260);
    m_searchEdit->addAction(m_searchAction->icon(), QLineEdit::LeadingPosition);
    toolBar->addWidget(m_searchEdit);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &MainWindow::runSearch);
    // Esc devuelve el teclado a la lista.
    auto *leaveSearch = new QAction(m_searchEdit);
    leaveSearch->setShortcut(QKeySequence(Qt::Key_Escape));
    leaveSearch->setShortcutContext(Qt::WidgetShortcut);
    m_searchEdit->addAction(leaveSearch);
    connect(leaveSearch, &QAction::triggered, this, [this] {
        if (auto *pane = currentPane()) pane->focusContent();
    });

    toolBar->addAction(m_newTabAction);
    toolBar->addAction(m_showTrashAction);
}

BrowserPane *MainWindow::openTab(const QString &path)
{
    auto *pane = new BrowserPane(m_scheduler, m_watcher, m_tabs);
    pane->setShowHidden(m_showHiddenAction->isChecked());
    pane->setFolderSizesEnabled(m_folderSizesAction->isChecked());
    pane->setSizeUnits(m_binaryUnitsAction->isChecked() ? core::SizeUnits::Binary
                                                                : core::SizeUnits::Decimal);

    connect(pane, &BrowserPane::pathChanged, this, [this, pane] {
        updateTabTitle(pane);
        if (pane == currentPane()) {
            m_pathBar->setPath(pane->currentPath());
            m_sidebar->setCurrentPath(pane->currentPath());
            syncSearchBox();
            updateEditActions();
        }
    });
    connect(pane, &BrowserPane::historyChanged, this, &MainWindow::updateNavigationActions);
    connect(pane, &BrowserPane::statusChanged, this, [this, pane] {
        if (pane == currentPane()) {
            updateStatus();
            updateEditActions();
        }
    });
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

    const int index = m_tabs->addTab(pane, QString());
    pane->navigateTo(path);
    m_tabs->setCurrentIndex(index);
    pane->focusContent();
    return pane;
}

void MainWindow::closeTab(int index)
{
    if (index < 0) {
        return;
    }
    // Siempre queda al menos una pestaña: cerrar la última cierra la ventana.
    if (m_tabs->count() == 1) {
        close();
        return;
    }
    QWidget *pane = m_tabs->widget(index);
    m_tabs->removeTab(index);
    pane->deleteLater();
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

void MainWindow::runSearch()
{
    BrowserPane *pane = currentPane();
    const QString text = m_searchEdit->text().trimmed();
    if (pane == nullptr || text.isEmpty() || pane->showsVolumes()) {
        return;
    }
    // Una consulta mal escrita se explica aquí, sin salir de la carpeta.
    const core::Result<search::Query> parsedQuery = search::Query::parse(text);
    if (!parsedQuery.ok()) {
        statusBar()->showMessage(tr("No se puede buscar: %1").arg(parsedQuery.error().message), 10000);
        return;
    }
    pane->search(text);
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
    m_searchEdit->setText(pane != nullptr && pane->showsSearch() ? searchQueryOf(pane->currentPath()) : QString());
}

void MainWindow::onCurrentTabChanged()
{
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
    const int index = m_tabs->indexOf(pane);
    if (index < 0) {
        return;
    }
    const QString path = pane->currentPath();
    QString title = pane->showsVolumes() ? tr("Unidades")
        : pane->showsSearch() ? tr("Buscar: %1").arg(searchQueryOf(path)) : QDir(path).dirName();
    if (title.isEmpty()) {
        title = QDir::toNativeSeparators(path);   // raíces: "/" o "C:\"
    }
    m_tabs->setTabText(index, title);
    m_tabs->setTabToolTip(index, pane->showsVolumes() ? title
                                 : pane->showsSearch()
                                     ? tr("%1 en %2").arg(title, QDir::toNativeSeparators(searchRootOf(path)))
                                     : QDir::toNativeSeparators(path));
    if (pane == currentPane()) {
        setWindowTitle(QStringLiteral("%1 — %2").arg(title, core::applicationName()));
    }
}

void MainWindow::restoreSession()
{
    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
    m_splitter->restoreState(settings.value(QStringLiteral("window/splitter")).toByteArray());
    m_showHiddenAction->setChecked(settings.value(QStringLiteral("view/showHidden"), false).toBool());
    m_binaryUnitsAction->setChecked(settings.value(QStringLiteral("view/binaryUnits"), false).toBool());
    m_folderSizesAction->setChecked(settings.value(QStringLiteral("view/folderSizes"), false).toBool());

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
    updateStatus();
}

void MainWindow::saveSession()
{
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/splitter"), m_splitter->saveState());
    settings.setValue(QStringLiteral("view/showHidden"), m_showHiddenAction->isChecked());
    settings.setValue(QStringLiteral("view/binaryUnits"), m_binaryUnitsAction->isChecked());
    settings.setValue(QStringLiteral("view/folderSizes"), m_folderSizesAction->isChecked());

    QStringList paths;
    for (int i = 0; i < m_tabs->count(); ++i) {
        // Unos resultados no se guardan: al volver se abre la carpeta donde se buscó.
        const QString path = static_cast<BrowserPane *>(m_tabs->widget(i))->currentPath();
        paths.append(isSearchLocation(path) ? searchRootOf(path) : path);
    }
    settings.setValue(QStringLiteral("session/tabs"), paths);
    settings.setValue(QStringLiteral("session/currentTab"), m_tabs->currentIndex());
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
