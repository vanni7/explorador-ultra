// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/organize/Organize.h"
#include "eu/tasks/Scheduler.h"

#include <QDialog>
#include <QTimer>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QTreeWidget;

namespace eu::ui {

/// Ventana "Organizar carpeta" (SPEC §12): reglas, y siempre la simulación
/// completa (árbol resultante y lista de movimientos) antes de aplicar.
class OrganizeDialog : public QDialog
{
    Q_OBJECT

public:
    OrganizeDialog(tasks::Scheduler &scheduler, QString folder, QWidget *parent = nullptr);
    ~OrganizeDialog() override;

    /// La simulación que se aprobó (vacía si no se aceptó).
    const organize::Plan &plan() const { return m_plan; }
    QString folder() const { return m_folder; }

    // Para las pruebas.
    void setRules(const QList<organize::Rule> &rules);
    void setRecursive(bool recursive);
    bool isSimulationCurrent() const { return m_shownGeneration == m_generation; }
    QTreeWidget *simulation() const { return m_tree; }
    QPushButton *applyButton() const { return m_applyButton; }

signals:
    void simulated();

private:
    void loadRulesIntoTable(const QList<organize::Rule> &rules);
    QList<organize::Rule> rulesFromTable() const;
    void scheduleSimulation();
    void startSimulation();
    void showSimulation(quint64 generation, const organize::Plan &plan);

    tasks::Scheduler &m_scheduler;
    QString m_folder;
    organize::Plan m_plan;
    quint64 m_generation = 0;
    quint64 m_shownGeneration = 0;
    tasks::TaskHandle m_task;
    QTimer m_debounce;
    bool m_loading = false;

    QComboBox *m_preset = nullptr;
    QCheckBox *m_recursive = nullptr;
    QTableWidget *m_rules = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_applyButton = nullptr;
};

} // namespace eu::ui
