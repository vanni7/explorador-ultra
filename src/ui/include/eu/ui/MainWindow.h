// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/fs/DirectoryWatcher.h"
#include "eu/fs/Volumes.h"
#include "eu/tasks/Scheduler.h"

#include <QMainWindow>

class QAction;
class QLabel;
class QSplitter;
class QTabWidget;

namespace eu::ui {

class BrowserPane;
class PathBar;
class Sidebar;

/// Ventana principal: barra superior, panel de lugares y pestañas de exploración.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(tasks::Scheduler &scheduler, QWidget *parent = nullptr);
    ~MainWindow() override;

    /// Abre una pestaña nueva en `path` y la deja activa.
    BrowserPane *openTab(const QString &path);
    BrowserPane *currentPane() const;
    int tabCount() const;

    /// Restaura pestañas y geometría de la sesión anterior. Si no hay sesión
    /// guardada, abre la carpeta personal.
    void restoreSession();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void createActions();
    void createToolBar();
    void closeTab(int index);
    void duplicateCurrentTab();
    void onCurrentTabChanged();
    void updateNavigationActions();
    void updateStatus();
    void updateTabTitle(BrowserPane *pane);
    void saveSession();
    void navigateCurrentTo(const QString &path);

    tasks::Scheduler &m_scheduler;
    fs::DirectoryWatcher m_watcher;
    fs::VolumeMonitor m_volumeMonitor;

    QTabWidget *m_tabs = nullptr;
    Sidebar *m_sidebar = nullptr;
    PathBar *m_pathBar = nullptr;
    QSplitter *m_splitter = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_unitsLabel = nullptr;

    QAction *m_backAction = nullptr;
    QAction *m_forwardAction = nullptr;
    QAction *m_upAction = nullptr;
    QAction *m_refreshAction = nullptr;
    QAction *m_newTabAction = nullptr;
    QAction *m_closeTabAction = nullptr;
    QAction *m_duplicateTabAction = nullptr;
    QAction *m_showHiddenAction = nullptr;
    QAction *m_binaryUnitsAction = nullptr;
    QAction *m_editPathAction = nullptr;
};

} // namespace eu::ui
