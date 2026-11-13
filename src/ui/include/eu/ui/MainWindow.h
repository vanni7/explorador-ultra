// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/fs/DirectoryWatcher.h"
#include "eu/fs/Volumes.h"
#include "eu/journal/Journal.h"
#include "eu/tasks/Scheduler.h"
#include "eu/ui/FileActions.h"
#include "eu/ui/Locations.h"

#include <QHash>
#include <QMainWindow>
#include <QTimer>

#include <functional>
#include <memory>

class QAction;
class QLabel;
class QLineEdit;
class QProgressBar;
class QToolButton;
class QSplitter;
class QTabWidget;

namespace eu::ui {

class BrowserPane;
class PathBar;
class Sidebar;
class TrashDialog;

/// Ventana principal: barra superior, panel de lugares y pestañas de exploración.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    /// `journal` es el historial de operaciones. Si es nulo (no se pudo abrir),
    /// la ventana sirve para navegar pero no modifica archivos.
    explicit MainWindow(tasks::Scheduler &scheduler, std::shared_ptr<journal::Journal> journal = nullptr,
                        QWidget *parent = nullptr);
    ~MainWindow() override;

    /// Abre una pestaña nueva en `path` y la deja activa.
    BrowserPane *openTab(const QString &path);
    BrowserPane *currentPane() const;
    Sidebar *sidebar() const { return m_sidebar; }
    /// Buscador de la barra superior.
    QLineEdit *searchEdit() const { return m_searchEdit; }
    int tabCount() const;

    /// Opciones del buscador; son las del botón "Opciones" que tiene al lado.
    void setSearchScope(SearchScope scope);
    SearchScope searchScope() const { return m_searchScope; }
    void setSearchInContent(bool inContent);
    void setSearchCaseSensitive(bool caseSensitive);
    /// Unidades para el alcance "las unidades que elija" (puntos de montaje).
    void setChosenSearchVolumes(const QStringList &roots) { m_chosenVolumes = roots; }
    /// Sustituye la lista de unidades que usa el buscador. Para las pruebas,
    /// que no deben recorrer los discos reales del equipo.
    void setSearchVolumeSource(std::function<QList<fs::Volume>()> source) { m_searchVolumeSource = std::move(source); }

    /// Restaura pestañas y geometría de la sesión anterior. Si no hay sesión
    /// guardada, abre la carpeta personal.
    void restoreSession();

    FileActions *fileActions() const { return m_actions; }

    /// Sustituye la pregunta de confirmación de la eliminación permanente.
    /// Recibe las rutas y devuelve true para eliminar. Lo usan las pruebas,
    /// que no pueden contestar a un cuadro de diálogo.
    void setDeleteConfirmation(std::function<bool(const QStringList &)> confirmation);

    /// Qué hacer con las operaciones que quedaron a medias (SPEC §9).
    enum class RecoveryChoice {
        Revert,   ///< Deshacer lo que alcanzaron a hacer.
        Discard,  ///< Dejar lo hecho y limpiar los archivos a medio escribir.
        Later,    ///< No tocar nada; se volverá a preguntar en el próximo arranque.
    };
    /// Sustituye el diálogo de recuperación (para las pruebas).
    void setRecoveryChoice(std::function<RecoveryChoice(const QList<journal::Operation> &)> choice);
    /// Revisa el historial en segundo plano y, si hay operaciones a medias,
    /// pregunta qué hacer. Solo debe llamarlo la instancia que es dueña del
    /// historial: otra instancia en marcha tendría operaciones "en curso" legítimas.
    void checkInterruptedOperations();

public slots:
    // Acciones sobre la pestaña activa (las mismas del menú y los atajos).
    void newFolder();
    void newFile();
    void renameSelection();
    void copySelection();
    void cutSelection();
    void paste();
    /// Pega lo que hay en el portapapeles dentro de `destination`.
    void pasteInto(const QString &destination);
    void trashSelection();
    void deleteSelectionPermanently();
    void undo();
    /// Abre (o trae al frente) la ventana de la papelera.
    eu::ui::TrashDialog *showTrash();

signals:
    /// Lo que se le comunica al usuario al terminar una acción (texto de la
    /// barra de estado o del aviso de error).
    void actionReported(const QString &message, bool isError);

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
    void runSearch();
    void syncSearchBox();

    void createEditActions();
    void createProgressWidgets();
    void showContextMenu(BrowserPane *pane, const QPoint &globalPosition);
    void showSidebarMenu(const QString &path, const QPoint &globalPosition);
    void updateEditActions();
    void copyToClipboard(bool cut);
    /// Copia o mueve `paths` a `destination` (pegar y soltar acaban aquí).
    void transfer(const QStringList &paths, const QString &destination, bool move);
    bool movesByDefault(const QString &source, const QString &destination) const;
    bool askPermanentDelete(const QStringList &paths, const QString &reason);
    void onInterruptedOperationsFound(const QList<journal::Operation> &operations);
    RecoveryChoice askRecovery(const QList<journal::Operation> &operations);
    void onActionStarted(quint64 id, ActionKind kind);
    void onActionProgressed(quint64 id, const ops::Progress &progress);
    void onActionFinished(const ActionResult &result);
    void onRunningCountChanged(int count);
    void report(const QString &message, bool isError, const QStringList &details = {});

    tasks::Scheduler &m_scheduler;
    fs::DirectoryWatcher m_watcher;
    fs::VolumeMonitor m_volumeMonitor;

    QTabWidget *m_tabs = nullptr;
    Sidebar *m_sidebar = nullptr;
    PathBar *m_pathBar = nullptr;
    QSplitter *m_splitter = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_unitsLabel = nullptr;

    FileActions *m_actions = nullptr;
    std::function<bool(const QStringList &)> m_deleteConfirmation;
    std::function<RecoveryChoice(const QList<journal::Operation> &)> m_recoveryChoice;
    QHash<quint64, ActionKind> m_runningKinds;
    QHash<quint64, int> m_requestedCounts;   ///< Cuántos elementos se pidieron en cada acción.
    quint64 m_renameAfterCreateId = 0;       ///< Acción "nuevo…" cuyo resultado se deja listo para nombrar.

    // Indicador de avance en la barra de estado.
    QWidget *m_progressBox = nullptr;
    QLabel *m_progressLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QToolButton *m_cancelButton = nullptr;
    QTimer m_progressDelay;   ///< El indicador solo aparece si la acción no es instantánea.

    TrashDialog *m_trashDialog = nullptr;

    QAction *m_backAction = nullptr;
    QAction *m_forwardAction = nullptr;
    QAction *m_upAction = nullptr;
    QAction *m_refreshAction = nullptr;
    QAction *m_newTabAction = nullptr;
    QAction *m_closeTabAction = nullptr;
    QAction *m_duplicateTabAction = nullptr;
    QAction *m_showHiddenAction = nullptr;
    QAction *m_binaryUnitsAction = nullptr;
    QAction *m_folderSizesAction = nullptr;
    QAction *m_filterAction = nullptr;
    QAction *m_searchAction = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QToolButton *m_searchOptionsButton = nullptr;
    QHash<int, QAction *> m_scopeActions;   ///< Por SearchScope.
    QAction *m_searchInNameAction = nullptr;
    QAction *m_searchInContentAction = nullptr;
    QAction *m_searchCaseAction = nullptr;
    SearchScope m_searchScope = SearchScope::Subfolders;
    QStringList m_chosenVolumes;
    std::function<QList<fs::Volume>()> m_searchVolumeSource;
    void buildSearchOptions();
    void updateSearchHints();
    bool chooseSearchVolumes();
    QList<fs::Volume> searchVolumes() const;
    QAction *m_editPathAction = nullptr;

    QAction *m_openAction = nullptr;
    QAction *m_newFolderAction = nullptr;
    QAction *m_newFileAction = nullptr;
    QAction *m_renameAction = nullptr;
    QAction *m_cutAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_pasteAction = nullptr;
    QAction *m_trashAction = nullptr;
    QAction *m_deleteAction = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_selectAllAction = nullptr;
    QAction *m_showTrashAction = nullptr;
};

} // namespace eu::ui
