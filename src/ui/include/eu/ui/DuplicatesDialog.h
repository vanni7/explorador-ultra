// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/dupes/Duplicates.h"
#include "eu/tasks/Scheduler.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace eu::ui {

/// Ventana "Buscar duplicados" (SPEC §14.1). Nunca borra sola: se marcan
/// (a mano o con una regla), se confirma y lo marcado va a la papelera propia.
class DuplicatesDialog : public QDialog
{
    Q_OBJECT

public:
    /// `folder`: la carpeta a la vista; `volumeOfFolder`: su unidad;
    /// `allVolumes`: las raíces de todas las unidades.
    DuplicatesDialog(tasks::Scheduler &scheduler, QString folder, QString volumeOfFolder, QStringList allVolumes,
                     QWidget *parent = nullptr);
    ~DuplicatesDialog() override;

    /// Lo marcado para enviar a la papelera (tras aceptar).
    QStringList marked() const;

    // Para las pruebas.
    void startSearch();
    bool isSearching() const { return m_searching; }
    QTreeWidget *results() const { return m_tree; }
    void markWithRule(dupes::Keep keep, const QString &preferredPath = {});
    QLabel *summary() const { return m_summary; }
    /// Sin la ventana de confirmación (pruebas).
    void setConfirmForTesting(std::function<bool(int files, qint64 bytes)> confirm) { m_confirm = std::move(confirm); }

signals:
    void searchFinished();

private:
    void showResult(quint64 generation, const dupes::Result &result);
    void onItemChanged(QTreeWidgetItem *item, int column);
    void updateSummary();
    void sendToTrash();

    tasks::Scheduler &m_scheduler;
    QString m_folder;
    QString m_volume;
    QStringList m_allVolumes;
    QList<dupes::Group> m_groups;
    tasks::TaskHandle m_task;
    quint64 m_generation = 0;
    bool m_searching = false;
    bool m_updating = false;
    std::function<bool(int, qint64)> m_confirm;

    QComboBox *m_scope = nullptr;
    QComboBox *m_minSize = nullptr;
    QCheckBox *m_bytes = nullptr;
    QPushButton *m_searchButton = nullptr;
    QLabel *m_progress = nullptr;
    QTreeWidget *m_tree = nullptr;
    QComboBox *m_rule = nullptr;
    QLineEdit *m_preferred = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_trashButton = nullptr;
};

} // namespace eu::ui
