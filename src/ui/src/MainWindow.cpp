// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/MainWindow.h"

#include "eu/core/Version.h"
#include "eu/ui/BrowserPane.h"
#include "eu/ui/PathBar.h"
#include "eu/ui/Sidebar.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
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

MainWindow::MainWindow(tasks::Scheduler &scheduler, QWidget *parent)
    : QMainWindow(parent), m_scheduler(scheduler)
{
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
    createToolBar();

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
        }
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
    saveSession();
    QMainWindow::closeEvent(event);
}

} // namespace eu::ui
