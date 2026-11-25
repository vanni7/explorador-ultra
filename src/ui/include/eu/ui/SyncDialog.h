// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/sync/Sync.h"
#include "eu/tasks/Scheduler.h"

#include <QDialog>

#include <QHash>
#include <functional>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTreeView;

namespace eu::ui {

class FileActions;
struct ActionResult;
class SyncModel;

/// Ventana «Comparar y sincronizar carpetas» (SPEC §16). Compara dos carpetas
/// y enseña cada diferencia; para sincronizar hay que simular antes (el plan
/// se ve en la columna «Acción») y confirmar. Lo reemplazado o quitado va a la
/// papelera propia y todo se deshace junto con Deshacer. Sin «Propagar
/// borrados» nunca se borra nada. Tras sincronizar se vuelve a comparar y se
/// guarda el estado del par (para la vez siguiente en los dos sentidos).
class SyncDialog : public QDialog
{
    Q_OBJECT

public:
    /// `stateDirectory`: la carpeta de datos de la aplicación (ahí va el estado de cada par).
    SyncDialog(tasks::Scheduler &scheduler, FileActions *actions, QString left, QString right,
               QString stateDirectory, QWidget *parent = nullptr);
    ~SyncDialog() override;

    // Para las pruebas.
    void startCompare();
    void simulate();
    void synchronize();
    bool isBusy() const;
    const sync::Comparison &comparison() const { return m_comparison; }
    const sync::Plan &plan() const { return m_plan; }
    bool planIsCurrent() const { return m_planValid; }
    QTreeView *view() const { return m_view; }
    QLabel *summary() const { return m_summary; }
    void setDirection(sync::Direction direction);
    void setConflictPolicy(sync::ConflictPolicy policy);
    /// true: «Quitarlo también del otro lado»; false: «Dejarlo como está».
    void setPropagateDeletions(bool on);
    /// Vuelve a «Preguntarme qué hacer con cada uno» (lo de fábrica).
    void setAskAboutDeletions();
    /// Qué responder, en las pruebas, a la pregunta por lo borrado desde la
    /// última sincronización (sin la ventana). Vacío = cancelar.
    using DeletionChooser = std::function<std::optional<QHash<QString, sync::DeletionChoice>>(const QList<sync::Action> &)>;
    void setDeletionChooserForTesting(DeletionChooser chooser) { m_deletionChooser = std::move(chooser); }
    void setCriterion(sync::Criterion criterion);
    /// Sin la ventana de confirmación (pruebas).
    void setConfirmForTesting(std::function<bool()> confirm) { m_confirm = std::move(confirm); }

signals:
    void compareFinished();
    void syncFinished();

private:
    void onCompared(quint64 generation, const sync::Comparison &comparison);
    void onActionFinished(const ActionResult &result);
    void invalidateComparison();
    void invalidatePlan();
    void updateButtons();
    void updateSummary();
    sync::SyncOptions syncOptions() const;
    /// Pregunta qué hacer con cada borrado detectado. nullopt si se cancela.
    std::optional<QHash<QString, sync::DeletionChoice>> askDeletions(const QList<sync::Action> &deletions);
    /// Elegir carpeta con las unidades montadas a la vista.
    QString chooseFolder(const QString &start);
    QString leftPath() const;
    QString rightPath() const;
    bool pathsAreValid(QString *problem) const;

    tasks::Scheduler &m_scheduler;
    FileActions *m_actions;
    QString m_stateDirectory;
    sync::Comparison m_comparison;
    bool m_hasComparison = false;
    sync::Plan m_plan;
    bool m_planValid = false;
    sync::SyncState m_previous;
    tasks::TaskHandle m_task;
    quint64 m_generation = 0;
    bool m_comparing = false;
    quint64 m_runningSync = 0;
    /// true (y lo dice) si otra sincronización sigue trabajando en estas carpetas.
    bool otherSyncRunning();
    sync::Plan m_appliedPlan;          ///< Lo que se aplicó: para guardar el estado tras volver a comparar.
    bool m_saveStateAfterCompare = false;
    std::function<bool()> m_confirm;
    DeletionChooser m_deletionChooser;
    /// Lo que se respondió sobre los borrados de esta comparación.
    QHash<QString, sync::DeletionChoice> m_deletionChoices;
    /// Borrados detectados en la última comparación (ruta → ¿a la izquierda?).
    QHash<QString, bool> m_deletedOnLeft;

    QLineEdit *m_leftEdit = nullptr;
    QLineEdit *m_rightEdit = nullptr;
    QComboBox *m_criterion = nullptr;
    QCheckBox *m_hidden = nullptr;
    QPushButton *m_compareButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QLabel *m_progress = nullptr;
    QCheckBox *m_showIdentical = nullptr;
    QTreeView *m_view = nullptr;
    SyncModel *m_model = nullptr;
    QLabel *m_summary = nullptr;
    QRadioButton *m_toRight = nullptr;
    QRadioButton *m_toLeft = nullptr;
    QRadioButton *m_both = nullptr;
    QComboBox *m_conflicts = nullptr;
    QComboBox *m_deletions = nullptr;   ///< Qué hacer con lo borrado en un lado (o preguntar).
    QCheckBox *m_verify = nullptr;
    QPushButton *m_simulateButton = nullptr;
    QPushButton *m_syncButton = nullptr;
};

} // namespace eu::ui
