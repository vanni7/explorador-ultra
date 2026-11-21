// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/fs/Volumes.h"
#include "eu/index/IndexService.h"

#include <QDialog>
#include <QTimer>

#include <functional>
#include <optional>

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace eu::ui {

/// Texto corto del estado de un índice ("Al día", "Indexando… 1.200 carpetas").
QString indexStateText(const index::RootStatus &status);

/// Ventana "Índice de búsqueda": qué unidades se indexan y cómo va cada una
/// (SPEC §6.4). Marcar una unidad empieza a indexarla; desmarcarla borra su índice.
class IndexDialog : public QDialog
{
    Q_OBJECT

public:
    using VolumeSource = std::function<QList<fs::Volume>()>;

    IndexDialog(index::IndexService &service, VolumeSource volumes, QWidget *parent = nullptr);

    QTreeWidget *list() const { return m_list; }
    /// El recuadro del servicio de Windows (oculto donde no existe).
    QWidget *serviceBox() const { return m_serviceBox; }
    /// Marca o desmarca una unidad, como haría el usuario.
    void setIndexed(const QString &root, bool indexed);

signals:
    /// Cambió la lista de unidades indexadas (para guardarla).
    void rootsChanged();

private:
    void rebuild();
    void updateRows();
    QTreeWidgetItem *itemFor(const QString &root) const;
    QString selectedRoot() const;
    void updateService();
    void runServiceSetup(bool install);
    void onServiceSetupFinished();

    index::IndexService &m_service;
    VolumeSource m_volumes;
    QTreeWidget *m_list = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_rebuildButton = nullptr;
    QWidget *m_serviceBox = nullptr;
    QLabel *m_serviceLabel = nullptr;
    QPushButton *m_serviceButton = nullptr;
    QTimer m_setupTimer;   ///< Mira si terminó el instalador lanzado como administrador.
    std::optional<index::journalservice::Setup> m_setup;
    bool m_setupInstalls = false;
    QString m_serviceMessage;   ///< Resultado de la última instalación o desinstalación.
    bool m_updating = false;
};

} // namespace eu::ui
