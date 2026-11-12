// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/MainWindow.h"

#include "eu/core/Version.h"
#include "eu/ui/BrowserPane.h"
#include "eu/ui/FileClipboard.h"
#include "eu/ui/PathBar.h"
#include "eu/ui/Sidebar.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
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
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &MainWindow::updateEditActions);

    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_sidebar, &Sidebar::placeActivated, this, &MainWindow::navigateCurrentTo);
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
    connect(m_refreshAction, &QAction::triggered, this, [this] { if (auto *pane = currentPane()) pane->refresh(); });

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
        for (int i = 0; i < m_tabs->count(); ++i) {
            static_cast<BrowserPane *>(m_tabs->widget(i))->model()->setSizeUnits(units);
        }
        updateStatus();
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
    viewMenu->addSeparator();
    viewMenu->addAction(m_showHiddenAction);
    viewMenu->addAction(m_binaryUnitsAction);

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
        if (auto *pane = currentPane()) pane->view()->selectAll();
    });

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
    const bool canModify = pane != nullptr && m_actions->isAvailable();
    const qsizetype selected = pane ? pane->view()->selectionModel()->selectedRows().size() : 0;
    const bool canPaste = !fileSelectionFrom(QGuiApplication::clipboard()->mimeData()).isEmpty();

    m_openAction->setEnabled(selected > 0);
    m_copyAction->setEnabled(selected > 0);
    m_cutAction->setEnabled(canModify && selected > 0);
    m_pasteAction->setEnabled(canModify && canPaste);
    m_renameAction->setEnabled(canModify && selected == 1);
    m_trashAction->setEnabled(canModify && selected > 0);
    m_deleteAction->setEnabled(canModify && selected > 0);
    m_newFolderAction->setEnabled(canModify);
    m_newFileAction->setEnabled(canModify);
    m_undoAction->setEnabled(m_actions->isAvailable());
    m_selectAllAction->setEnabled(pane != nullptr);
}

void MainWindow::showContextMenu(BrowserPane *pane, const QPoint &globalPosition)
{
    QMenu menu(this);
    if (!pane->view()->selectionModel()->selectedRows().isEmpty()) {
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
    if (pane == nullptr || !m_actions->isAvailable()) {
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
    if (pane == nullptr || !m_actions->isAvailable()) {
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
    if (pane == nullptr) {
        return;
    }
    const FileSelection selection = fileSelectionFrom(QGuiApplication::clipboard()->mimeData());
    if (selection.isEmpty()) {
        return;
    }
    transfer(selection.paths, pane->currentPath(), selection.cut);
    if (selection.cut) {
        // Lo cortado se pega una sola vez.
        QGuiApplication::clipboard()->clear();
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
            pane->refresh();
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
            message = tr("%n elemento(s) eliminado(s) permanentemente.", nullptr, qMax(0, requested - failed));
            break;
        case ActionKind::Undo:
            if (outcome.ok()) {
                message = tr("Se deshizo: %1").arg(result.undoneDescription);
            }
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
    if (result.kind == ActionKind::Undo) {
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

    // La búsqueda llega en la Fase 2. Se muestra deshabilitada y rotulada como
    // tal: ningún control aparenta funcionar sin hacerlo (SPEC §25.1).
    auto *search = new QLineEdit(toolBar);
    search->setPlaceholderText(tr("Buscar (próximamente)"));
    search->setEnabled(false);
    search->setFixedWidth(220);
    toolBar->addWidget(search);

    toolBar->addAction(m_newTabAction);
}

BrowserPane *MainWindow::openTab(const QString &path)
{
    auto *pane = new BrowserPane(m_scheduler, m_watcher, m_tabs);
    pane->setShowHidden(m_showHiddenAction->isChecked());
    pane->model()->setSizeUnits(m_binaryUnitsAction->isChecked() ? core::SizeUnits::Binary
                                                                : core::SizeUnits::Decimal);

    connect(pane, &BrowserPane::pathChanged, this, [this, pane] {
        updateTabTitle(pane);
        if (pane == currentPane()) {
            m_pathBar->setPath(pane->currentPath());
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
    connect(pane, &BrowserPane::openFailed, this, [this](const QString &failedPath) {
        statusBar()->showMessage(tr("No se pudo abrir %1").arg(QDir::toNativeSeparators(failedPath)), 6000);
    });

    const int index = m_tabs->addTab(pane, QString());
    pane->navigateTo(path);
    m_tabs->setCurrentIndex(index);
    pane->view()->setFocus();
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
        pane->view()->setFocus();
    }
}

void MainWindow::onCurrentTabChanged()
{
    if (BrowserPane *pane = currentPane()) {
        m_pathBar->setPath(pane->currentPath());
    }
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
    QString title = QDir(path).dirName();
    if (title.isEmpty()) {
        title = QDir::toNativeSeparators(path);   // raíces: "/" o "C:\"
    }
    m_tabs->setTabText(index, title);
    m_tabs->setTabToolTip(index, QDir::toNativeSeparators(path));
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

    QStringList paths;
    for (int i = 0; i < m_tabs->count(); ++i) {
        paths.append(static_cast<BrowserPane *>(m_tabs->widget(i))->currentPath());
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
