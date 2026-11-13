// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/fs/Volumes.h"
#include "eu/search/Searcher.h"
#include "eu/ui/Locations.h"
#include "eu/ui/DirectoryModel.h"
#include "eu/ui/FileClipboard.h"

#include <QStringList>
#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedLayout;
class QTreeView;

namespace eu::fs {
class DirectoryWatcher;
}

namespace eu::ui {

class SearchResultsModel;
class VolumesView;

/// Carpeta superior de una ruta, o cadena vacía si ya es una raíz.
/// Es un cálculo sobre el texto: no toca el disco.
QString parentPathOf(const QString &path);

/// Un explorador: la vista detallada de una carpeta con su historial de
/// navegación. Cada pestaña contiene uno.
class BrowserPane : public QWidget
{
    Q_OBJECT

public:
    BrowserPane(tasks::Scheduler &scheduler, fs::DirectoryWatcher &watcher, QWidget *parent = nullptr);
    ~BrowserPane() override;

    /// Carpeta que se está viendo, o volumesLocation() si la pestaña muestra
    /// la lista de unidades.
    QString currentPath() const { return m_location; }
    /// true si la pestaña muestra la lista de unidades en vez de una carpeta.
    bool showsVolumes() const;
    VolumesView *volumesView() const { return m_volumesView; }
    /// De dónde sale la lista de unidades (por defecto, del sistema). Se llama
    /// en un hilo de trabajo. Las pruebas ponen aquí unidades inventadas.
    void setVolumeSource(std::function<QList<fs::Volume>()> source);
    void setSizeUnits(core::SizeUnits units);
    /// Pone el foco del teclado en la lista que esté a la vista.
    void focusContent();

    /// true si la pestaña muestra los resultados de una búsqueda.
    bool showsSearch() const;
    /// true si muestra una carpeta del disco (ni unidades ni resultados).
    bool showsFolder() const { return !showsVolumes() && !showsSearch(); }
    enum class SearchState {
        Idle,       ///< No hay búsqueda.
        Running,
        Paused,
        Finished,   ///< Recorrió todo (o llegó al tope de resultados).
        Stopped,    ///< El usuario la detuvo: los resultados están incompletos.
        Invalid,    ///< La consulta tiene un error de sintaxis.
    };
    SearchState searchState() const { return m_searchState; }
    const search::SearchProgress &searchProgress() const { return m_searchProgress; }
    SearchResultsModel *resultsModel() const { return m_resultsModel; }
    QTreeView *resultsView() const { return m_resultsView; }
    /// Dónde busca la búsqueda a la vista, dicho para una persona
    /// ("/home/ana", "todas las unidades"…).
    QString searchWhere() const;
    /// Texto de la franja que hay sobre los resultados.
    QString searchInfo() const;

    /// Cuántos elementos hay seleccionados en la lista que está a la vista.
    int selectedCount() const;
    void selectAll();
    DirectoryModel *model() const { return m_model; }
    QTreeView *view() const { return m_view; }

    bool canGoBack() const { return !m_backHistory.isEmpty(); }
    bool canGoForward() const { return !m_forwardHistory.isEmpty(); }
    /// Desde la raíz de una unidad se sube a la lista de unidades.
    bool canGoUp() const { return !currentPath().isEmpty() && !showsVolumes(); }

    /// Rutas completas de lo seleccionado.
    QStringList selectedPaths() const;
    /// Texto para la barra de estado: cuántos elementos, cuántos seleccionados…
    QString statusText() const;

    /// Selecciona estos nombres en cuanto estén a la vista (por ejemplo, lo
    /// recién copiado). Si aún no aparecen, vuelve a leer la carpeta. Con
    /// `editFirst`, además deja el primero listo para escribirle un nombre.
    void selectWhenAvailable(const QStringList &names, bool editFirst = false);
    /// Un nombre que todavía no existe en la carpeta: "Nueva carpeta",
    /// "Nueva carpeta (2)"… Se calcula con lo que hay a la vista, sin tocar el disco.
    QString unusedName(const QString &wanted) const;

    bool isFilterVisible() const;
    QLineEdit *filterEdit() const { return m_filterEdit; }
    /// Mensaje de la barra de filtro: recuento, error de sintaxis o aviso.
    QString filterMessage() const;

    /// Regla para decidir qué hace soltar archivos sin pulsar ninguna tecla:
    /// la función recibe (origen, carpeta de destino) y devuelve true si debe
    /// moverse. Sin regla, se copia (lo que nunca quita nada del origen).
    void setMoveByDefaultRule(MoveByDefaultRule rule);

public slots:
    void navigateTo(const QString &path);
    void goBack();
    void goForward();
    void goUp();
    void refresh();
    void setShowHidden(bool show);
    void setFolderSizesEnabled(bool enabled);
    /// Abre lo seleccionado: entra en la carpeta o lanza el archivo con su programa.
    void openSelection();
    /// Empieza a renombrar el elemento actual escribiendo sobre su nombre.
    void startRename();
    /// Muestra la barra de filtro de esta carpeta y pone el cursor en ella.
    void showFilter();
    /// Quita el filtro y oculta la barra.
    void hideFilter();
    /// Busca en la carpeta actual y sus subcarpetas; los resultados ocupan la
    /// pestaña como una ubicación más (se vuelve con Atrás).
    void search(const QString &queryText);
    /// Lanza una búsqueda con su alcance y sus opciones.
    void search(const eu::ui::SearchSpec &spec);
    void setSearchPaused(bool paused);
    void stopSearch();
    /// Va a la carpeta del resultado seleccionado y lo deja seleccionado.
    void openContainingFolder();
    /// Una operación de archivos terminó: la carpeta se vuelve a leer; de los
    /// resultados de una búsqueda se quitan los que ya no existen.
    void contentMayHaveChanged();

signals:
    void pathChanged(const QString &path);
    void historyChanged();
    void statusChanged();
    /// No se pudo abrir un archivo con el programa asociado.
    void openFailed(const QString &path);
    /// El usuario escribió un nombre nuevo para un elemento.
    void renameRequested(const QString &path, const QString &newName);
    /// Clic derecho (o tecla de menú) sobre la lista. `globalPosition` es
    /// dónde mostrar el menú.
    void contextMenuRequested(const QPoint &globalPosition);
    /// Se soltaron archivos (de esta u otra aplicación) sobre `destination`.
    void filesDropped(const QStringList &paths, const QString &destination, bool move);

private:
    void changePath(const QString &path, const QString &nameToSelect);
    void onActivated(const QModelIndex &index);
    void onModelStateChanged(DirectoryModel::State state);
    void rememberSelection();
    void restoreSelection();
    bool applyPendingSelection();
    void onEditingFinished();
    void applyFilterText(const QString &text);
    void updateFilterInfo();
    void reloadVolumes();
    void startSearch();
    void cancelSearch();
    void onSearchHits(quint64 generation, std::vector<search::Hit> hits, const search::SearchProgress &progress);
    void onSearchFinished(quint64 generation, const search::SearchSummary &summary);
    void onResultActivated(const QModelIndex &index);
    void updateSearchInfo();

    tasks::Scheduler &m_scheduler;
    QString m_location;   ///< Lo que muestra la pestaña: una carpeta o volumesLocation().
    DirectoryModel *m_model = nullptr;
    fs::DirectoryWatcher &m_watcher;
    QTreeView *m_view = nullptr;
    QStackedLayout *m_stack = nullptr;
    QWidget *m_messagePage = nullptr;
    QLabel *m_messageLabel = nullptr;
    QPushButton *m_retryButton = nullptr;

    // Lista de unidades (ubicación "Unidades").
    VolumesView *m_volumesView = nullptr;
    std::function<QList<fs::Volume>()> m_volumeSource;
    QTimer m_volumesTimer;            ///< Mantiene al día el espacio libre mientras se ve la lista.
    bool m_volumesLoading = false;
    QString m_volumeToSelect;         ///< Unidad que se selecciona al llegar la lista.

    // Resultados de búsqueda (ubicación "buscar:").
    QWidget *m_searchPage = nullptr;
    QTreeView *m_resultsView = nullptr;
    SearchResultsModel *m_resultsModel = nullptr;
    QLabel *m_searchInfo = nullptr;
    QPushButton *m_searchPauseButton = nullptr;
    QPushButton *m_searchStopButton = nullptr;
    QPushButton *m_searchAgainButton = nullptr;
    tasks::TaskHandle m_searchTask;
    std::shared_ptr<search::PauseGate> m_searchGate;
    quint64 m_searchGeneration = 0;   ///< Lo que llegue de una búsqueda anterior se descarta.
    SearchState m_searchState = SearchState::Idle;
    search::SearchProgress m_searchProgress;
    bool m_searchTruncated = false;
    bool m_searchReadsContent = false;   ///< La búsqueda en curso abre archivos.
    QString m_searchError;

    // Barra de filtro local (SPEC §6.6).
    QWidget *m_filterBar = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QLabel *m_filterInfo = nullptr;
    QString m_filterProblem;   ///< Error de sintaxis o aviso del filtro escrito.

    QStringList m_backHistory;
    QStringList m_forwardHistory;
    QString m_watchedPath;

    // Selección que se restaura cuando el modelo sustituye su contenido.
    QStringList m_selectedNames;
    QString m_currentName;

    // Nombres que hay que seleccionar cuando aparezcan (selectWhenAvailable).
    QStringList m_pendingNames;
    bool m_pendingEdit = false;
    int m_pendingAttempts = 0;

    // Mientras el usuario escribe un nombre no se sustituye el contenido de
    // la vista: se perdería lo que lleva escrito. El refresco se hace al acabar.
    bool m_editing = false;
    bool m_refreshDeferred = false;
};

} // namespace eu::ui
