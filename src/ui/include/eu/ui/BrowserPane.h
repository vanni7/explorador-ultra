// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/ui/DirectoryModel.h"

#include <QStringList>
#include <QWidget>

class QLabel;
class QPushButton;
class QStackedLayout;
class QTreeView;

namespace eu::fs {
class DirectoryWatcher;
}

namespace eu::ui {

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

    QString currentPath() const { return m_model->path(); }
    DirectoryModel *model() const { return m_model; }
    QTreeView *view() const { return m_view; }

    bool canGoBack() const { return !m_backHistory.isEmpty(); }
    bool canGoForward() const { return !m_forwardHistory.isEmpty(); }
    bool canGoUp() const { return !parentPathOf(currentPath()).isEmpty(); }

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

public slots:
    void navigateTo(const QString &path);
    void goBack();
    void goForward();
    void goUp();
    void refresh();
    void setShowHidden(bool show);
    /// Abre lo seleccionado: entra en la carpeta o lanza el archivo con su programa.
    void openSelection();
    /// Empieza a renombrar el elemento actual escribiendo sobre su nombre.
    void startRename();

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

private:
    void changePath(const QString &path, const QString &nameToSelect);
    void onActivated(const QModelIndex &index);
    void onModelStateChanged(DirectoryModel::State state);
    void rememberSelection();
    void restoreSelection();
    bool applyPendingSelection();
    void onEditingFinished();

    DirectoryModel *m_model = nullptr;
    fs::DirectoryWatcher &m_watcher;
    QTreeView *m_view = nullptr;
    QStackedLayout *m_stack = nullptr;
    QWidget *m_messagePage = nullptr;
    QLabel *m_messageLabel = nullptr;
    QPushButton *m_retryButton = nullptr;

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
