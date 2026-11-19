// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/SyncDialog.h"

#include "eu/core/SizeFormat.h"
#include "eu/ui/FileActions.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace eu::ui {

namespace {

QString stateText(sync::State state)
{
    switch (state) {
    case sync::State::Identical:
        return SyncDialog::tr("Idéntico");
    case sync::State::Different:
        return SyncDialog::tr("Distinto");
    case sync::State::LeftNewer:
        return SyncDialog::tr("Más reciente a la izquierda");
    case sync::State::RightNewer:
        return SyncDialog::tr("Más reciente a la derecha");
    case sync::State::LeftOnly:
        return SyncDialog::tr("Solo a la izquierda");
    case sync::State::RightOnly:
        return SyncDialog::tr("Solo a la derecha");
    case sync::State::TypeConflict:
        return SyncDialog::tr("Archivo y carpeta");
    }
    return {};
}

QColor stateColor(sync::State state)
{
    switch (state) {
    case sync::State::Identical:
        return QApplication::palette().color(QPalette::Disabled, QPalette::Text);
    case sync::State::Different:
    case sync::State::LeftNewer:
    case sync::State::RightNewer:
        return QColor(0xc8, 0x7f, 0x0a);   // naranja: cambió
    case sync::State::LeftOnly:
    case sync::State::RightOnly:
        return QColor(0x2a, 0x7a, 0xb0);   // azul: falta en un lado
    case sync::State::TypeConflict:
        return QColor(0xc0, 0x39, 0x2b);   // rojo: conflicto
    }
    return {};
}

QString actionText(const sync::Action &action)
{
    switch (action.kind) {
    case sync::ActionKind::CopyToRight:
        return action.replaces ? SyncDialog::tr("→ Reemplazar a la derecha") : SyncDialog::tr("→ Copiar a la derecha");
    case sync::ActionKind::CopyToLeft:
        return action.replaces ? SyncDialog::tr("← Reemplazar a la izquierda") : SyncDialog::tr("← Copiar a la izquierda");
    case sync::ActionKind::TrashLeft:
        return SyncDialog::tr("Papelera (izquierda)");
    case sync::ActionKind::TrashRight:
        return SyncDialog::tr("Papelera (derecha)");
    case sync::ActionKind::Skip:
        return action.conflict ? SyncDialog::tr("Conflicto: no se toca") : SyncDialog::tr("No se toca");
    }
    return {};
}

} // namespace

/// Las diferencias de una comparación y, tras simular, qué se hará con cada una.
class SyncModel : public QAbstractTableModel
{
public:
    enum ColumnIndex { StateColumn, PathColumn, LeftColumn, RightColumn, ActionColumn, ColumnCount };

    using QAbstractTableModel::QAbstractTableModel;

    void setComparison(const sync::Comparison *comparison, bool showIdentical)
    {
        beginResetModel();
        m_comparison = comparison;
        m_rows.clear();
        if (comparison != nullptr) {
            for (qsizetype i = 0; i < comparison->items.size(); ++i) {
                const sync::Item &item = comparison->items.at(i);
                const bool folderOnBoth = item.left.exists && item.right.exists && item.left.isDirectory
                    && item.right.isDirectory;
                if (folderOnBoth || (item.state == sync::State::Identical && !showIdentical)) {
                    continue;
                }
                m_rows.append(int(i));
            }
        }
        m_actions.clear();
        endResetModel();
    }

    void setPlan(const sync::Plan *plan)
    {
        m_actions.clear();
        if (plan != nullptr) {
            for (qsizetype i = 0; i < plan->actions.size(); ++i) {
                m_actions.insert(plan->actions.at(i).relativePath, plan->actions.at(i));
            }
        }
        if (!m_rows.isEmpty()) {
            emit dataChanged(index(0, ActionColumn), index(int(m_rows.size()) - 1, ActionColumn));
        }
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : int(m_rows.size());
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : ColumnCount;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return QAbstractTableModel::headerData(section, orientation, role);
        }
        switch (section) {
        case StateColumn:
            return SyncDialog::tr("Estado");
        case PathColumn:
            return SyncDialog::tr("Elemento");
        case LeftColumn:
            return SyncDialog::tr("Izquierda");
        case RightColumn:
            return SyncDialog::tr("Derecha");
        case ActionColumn:
            return SyncDialog::tr("Acción");
        default:
            return {};
        }
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (m_comparison == nullptr || !index.isValid() || index.row() >= m_rows.size()) {
            return {};
        }
        const sync::Item &item = m_comparison->items.at(m_rows.at(index.row()));
        const auto side = [](const sync::Side &s) -> QString {
            if (!s.exists) {
                return QStringLiteral("—");
            }
            const QString when = s.modifiedMs > 0
                ? QLocale().toString(QDateTime::fromMSecsSinceEpoch(s.modifiedMs), QLocale::ShortFormat)
                : QString();
            const QString size = core::formatSize(s.size);
            if (s.isDirectory) {
                return SyncDialog::tr("Carpeta (%1)").arg(size);
            }
            return when.isEmpty() ? size : size + QStringLiteral(" · ") + when;
        };
        const auto action = m_actions.constFind(item.relativePath);
        switch (role) {
        case Qt::DisplayRole:
            switch (index.column()) {
            case StateColumn:
                return stateText(item.state);
            case PathColumn:
                return QDir::toNativeSeparators(item.relativePath);
            case LeftColumn:
                return side(item.left);
            case RightColumn:
                return side(item.right);
            case ActionColumn:
                return action == m_actions.cend() ? QString() : actionText(*action);
            default:
                return {};
            }
        case Qt::ToolTipRole:
            if (index.column() == ActionColumn && action != m_actions.cend()) {
                return action->reason;
            }
            if (index.column() == PathColumn) {
                return QDir::toNativeSeparators(item.relativePath);
            }
            return {};
        case Qt::ForegroundRole:
            if (index.column() == StateColumn) {
                return stateColor(item.state);
            }
            if (index.column() == ActionColumn && action != m_actions.cend() && action->conflict
                && action->kind == sync::ActionKind::Skip) {
                return stateColor(sync::State::TypeConflict);
            }
            return {};
        case Qt::DecorationRole:
            if (index.column() == PathColumn) {
                const bool folder = item.left.exists ? item.left.isDirectory : item.right.isDirectory;
                return QApplication::style()->standardIcon(folder ? QStyle::SP_DirIcon : QStyle::SP_FileIcon);
            }
            return {};
        default:
            return {};
        }
    }

private:
    const sync::Comparison *m_comparison = nullptr;
    QList<int> m_rows;                           ///< Índices en comparison->items.
    QHash<QString, sync::Action> m_actions;      ///< Por ruta relativa, tras simular.
};

SyncDialog::SyncDialog(tasks::Scheduler &scheduler, FileActions *actions, QString left, QString right,
                       QString stateDirectory, QWidget *parent)
    : QDialog(parent), m_scheduler(scheduler), m_actions(actions), m_stateDirectory(std::move(stateDirectory))
{
    setWindowTitle(tr("Comparar y sincronizar carpetas"));
    resize(1000, 640);

    // Las dos carpetas.
    m_leftEdit = new QLineEdit(QDir::toNativeSeparators(left), this);
    m_rightEdit = new QLineEdit(QDir::toNativeSeparators(right), this);
    m_leftEdit->setAccessibleName(tr("Carpeta de la izquierda"));
    m_rightEdit->setAccessibleName(tr("Carpeta de la derecha"));
    const auto chooser = [this](QLineEdit *edit) {
        auto *button = new QPushButton(tr("Elegir…"), this);
        connect(button, &QPushButton::clicked, this, [this, edit] {
            const QString folder = QFileDialog::getExistingDirectory(this, tr("Elegir carpeta"),
                                                                     QDir::fromNativeSeparators(edit->text()));
            if (!folder.isEmpty()) {
                edit->setText(QDir::toNativeSeparators(folder));
            }
        });
        return button;
    };
    auto *swapButton = new QPushButton(tr("⇄ Intercambiar"), this);
    swapButton->setToolTip(tr("Pasa la carpeta de la izquierda a la derecha y al revés"));
    connect(swapButton, &QPushButton::clicked, this, [this] {
        const QString text = m_leftEdit->text();
        m_leftEdit->setText(m_rightEdit->text());
        m_rightEdit->setText(text);
    });
    auto *paths = new QFormLayout;
    auto *leftRow = new QHBoxLayout;
    leftRow->addWidget(m_leftEdit, 1);
    leftRow->addWidget(chooser(m_leftEdit));
    auto *rightRow = new QHBoxLayout;
    rightRow->addWidget(m_rightEdit, 1);
    rightRow->addWidget(chooser(m_rightEdit));
    rightRow->addWidget(swapButton);
    paths->addRow(tr("Izquierda:"), leftRow);
    paths->addRow(tr("Derecha:"), rightRow);

    // Comparar.
    m_criterion = new QComboBox(this);
    m_criterion->addItem(tr("Tamaño y fecha (rápido)"), int(sync::Criterion::SizeAndDate));
    m_criterion->addItem(tr("Contenido: SHA-256 (lee los archivos del mismo tamaño)"), int(sync::Criterion::Hash));
    m_criterion->addItem(tr("Contenido: byte a byte"), int(sync::Criterion::Content));
    m_criterion->addItem(tr("Solo tamaño"), int(sync::Criterion::Size));
    m_criterion->addItem(tr("Solo nombre"), int(sync::Criterion::Name));
    m_hidden = new QCheckBox(tr("Incluir ocultos"), this);
    m_compareButton = new QPushButton(tr("Comparar"), this);
    m_compareButton->setDefault(true);
    m_stopButton = new QPushButton(tr("Detener"), this);
    m_progress = new QLabel(this);
    m_progress->setEnabled(false);
    auto *compareRow = new QHBoxLayout;
    compareRow->addWidget(new QLabel(tr("Comparar por:"), this));
    compareRow->addWidget(m_criterion);
    compareRow->addWidget(m_hidden);
    compareRow->addWidget(m_compareButton);
    compareRow->addWidget(m_stopButton);
    compareRow->addWidget(m_progress, 1);

    // Resultado.
    m_model = new SyncModel(this);
    m_view = new QTreeView(this);
    m_view->setModel(m_model);
    m_view->setRootIsDecorated(false);
    m_view->setUniformRowHeights(true);
    m_view->setAlternatingRowColors(true);
    m_view->setAccessibleName(tr("Diferencias entre las dos carpetas"));
    m_view->header()->setStretchLastSection(true);
    m_view->header()->resizeSection(SyncModel::StateColumn, 190);
    m_view->header()->resizeSection(SyncModel::PathColumn, 300);
    m_view->header()->resizeSection(SyncModel::LeftColumn, 190);
    m_view->header()->resizeSection(SyncModel::RightColumn, 190);
    m_showIdentical = new QCheckBox(tr("Mostrar también lo idéntico"), this);
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);

    // Sincronizar.
    auto *syncBox = new QGroupBox(tr("Sincronizar"), this);
    m_toRight = new QRadioButton(tr("Izquierda → derecha"), syncBox);
    m_toLeft = new QRadioButton(tr("Derecha → izquierda"), syncBox);
    m_both = new QRadioButton(tr("En los dos sentidos"), syncBox);
    m_both->setToolTip(tr("Usa lo que quedó la última vez que se sincronizó este par para saber qué es nuevo, "
                          "qué se borró y qué cambió en cada lado"));
    m_toRight->setChecked(true);
    auto *directions = new QButtonGroup(this);
    directions->addButton(m_toRight);
    directions->addButton(m_toLeft);
    directions->addButton(m_both);
    m_conflicts = new QComboBox(syncBox);
    m_conflicts->addItem(tr("No tocar ninguno"), int(sync::ConflictPolicy::Skip));
    m_conflicts->addItem(tr("Gana el más reciente"), int(sync::ConflictPolicy::NewerWins));
    m_conflicts->addItem(tr("Manda la izquierda"), int(sync::ConflictPolicy::LeftWins));
    m_conflicts->addItem(tr("Manda la derecha"), int(sync::ConflictPolicy::RightWins));
    m_deletions = new QCheckBox(tr("Propagar borrados (a la papelera propia)"), syncBox);
    m_deletions->setToolTip(tr("En un sentido: quita del destino lo que no está en el origen. En los dos: lo "
                               "borrado en un lado desde la última vez se quita del otro. Sin esto nunca se borra nada."));
    m_verify = new QCheckBox(tr("Comprobar cada copia (SHA-256)"), syncBox);
    m_simulateButton = new QPushButton(tr("Simular"), syncBox);
    m_simulateButton->setToolTip(tr("Calcula qué se hará, sin tocar nada (obligatorio antes de sincronizar)"));
    m_syncButton = new QPushButton(tr("Sincronizar…"), syncBox);
    auto *syncLayout = new QGridLayout(syncBox);
    syncLayout->addWidget(m_toRight, 0, 0);
    syncLayout->addWidget(m_toLeft, 0, 1);
    syncLayout->addWidget(m_both, 0, 2);
    syncLayout->addWidget(new QLabel(tr("Si cambió en los dos lados o el destino es más reciente:"), syncBox), 1, 0, 1, 2);
    syncLayout->addWidget(m_conflicts, 1, 2);
    syncLayout->addWidget(m_deletions, 2, 0, 1, 2);
    syncLayout->addWidget(m_verify, 2, 2);
    auto *syncButtons = new QHBoxLayout;
    syncButtons->addStretch(1);
    syncButtons->addWidget(m_simulateButton);
    syncButtons->addWidget(m_syncButton);
    syncLayout->addLayout(syncButtons, 3, 0, 1, 3);

    auto *closeButton = new QPushButton(tr("Cerrar"), this);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_showIdentical);
    bottom->addStretch(1);
    bottom->addWidget(closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(paths);
    layout->addLayout(compareRow);
    layout->addWidget(m_view, 1);
    layout->addWidget(m_summary);
    layout->addWidget(syncBox);
    layout->addLayout(bottom);

    connect(m_compareButton, &QPushButton::clicked, this, &SyncDialog::startCompare);
    connect(m_stopButton, &QPushButton::clicked, this, [this] {
        if (m_task.isValid()) {
            m_task.cancel();
        }
    });
    connect(m_simulateButton, &QPushButton::clicked, this, &SyncDialog::simulate);
    connect(m_syncButton, &QPushButton::clicked, this, &SyncDialog::synchronize);
    connect(m_showIdentical, &QCheckBox::toggled, this, [this](bool show) {
        m_model->setComparison(m_hasComparison ? &m_comparison : nullptr, show);
        m_model->setPlan(m_planValid ? &m_plan : nullptr);
    });
    // Lo que cambia la comparación la invalida; lo que cambia el plan obliga a simular de nuevo.
    connect(m_leftEdit, &QLineEdit::textChanged, this, &SyncDialog::invalidateComparison);
    connect(m_rightEdit, &QLineEdit::textChanged, this, &SyncDialog::invalidateComparison);
    connect(m_criterion, &QComboBox::currentIndexChanged, this, &SyncDialog::invalidateComparison);
    connect(m_hidden, &QCheckBox::toggled, this, &SyncDialog::invalidateComparison);
    connect(directions, &QButtonGroup::buttonToggled, this, [this] { invalidatePlan(); });
    connect(m_conflicts, &QComboBox::currentIndexChanged, this, &SyncDialog::invalidatePlan);
    connect(m_deletions, &QCheckBox::toggled, this, &SyncDialog::invalidatePlan);
    if (m_actions != nullptr) {
        connect(m_actions, &FileActions::finished, this, &SyncDialog::onActionFinished);
    }
    updateButtons();
    updateSummary();
}

SyncDialog::~SyncDialog()
{
    if (m_task.isValid()) {
        m_task.cancel();
    }
}

QString SyncDialog::leftPath() const
{
    return QDir::cleanPath(QDir::fromNativeSeparators(m_leftEdit->text().trimmed()));
}

QString SyncDialog::rightPath() const
{
    return QDir::cleanPath(QDir::fromNativeSeparators(m_rightEdit->text().trimmed()));
}

bool SyncDialog::pathsAreValid(QString *problem) const
{
    const QString left = leftPath();
    const QString right = rightPath();
    if (m_leftEdit->text().trimmed().isEmpty() || m_rightEdit->text().trimmed().isEmpty()) {
        *problem = tr("Elige las dos carpetas.");
        return false;
    }
    if (!QFileInfo(left).isDir() || !QFileInfo(right).isDir()) {
        *problem = tr("Las dos tienen que ser carpetas que existen.");
        return false;
    }
    const auto inside = [](const QString &inner, const QString &outer) {
        const QString prefix = outer.endsWith(QLatin1Char('/')) ? outer : outer + QLatin1Char('/');
        return inner == outer || inner.startsWith(prefix);
    };
    if (inside(left, right) || inside(right, left)) {
        *problem = tr("Una carpeta no puede estar dentro de la otra (ni ser la misma).");
        return false;
    }
    return true;
}

bool SyncDialog::isBusy() const
{
    return m_comparing || m_runningSync != 0;
}

void SyncDialog::startCompare()
{
    QString problem;
    if (!pathsAreValid(&problem)) {
        m_progress->setText(problem);
        return;
    }
    if (m_task.isValid()) {
        m_task.cancel();
    }
    m_comparing = true;
    invalidatePlan();
    const quint64 generation = ++m_generation;
    const QString left = leftPath();
    const QString right = rightPath();
    sync::CompareOptions options;
    options.criterion = sync::Criterion(m_criterion->currentData().toInt());
    options.includeHidden = m_hidden->isChecked();
    m_progress->setText(tr("Comparando…"));
    const QPointer<SyncDialog> self(this);
    m_task = m_scheduler.submitForPaths(
        tasks::Category::Hashing, tasks::Priority::UserInteraction, {left, right},
        [self, generation, left, right, options](const tasks::CancelToken &cancel) {
            qint64 lastReport = 0;
            const sync::Comparison result = sync::compare(left, right, options, cancel,
                                                          [&](const sync::CompareProgress &progress) {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now - lastReport < 150) {
                    return;
                }
                lastReport = now;
                QMetaObject::invokeMethod(qApp, [self, generation, progress] {
                    if (self && self->m_generation == generation) {
                        self->m_progress->setText(tr("Comparando… %1 carpetas, %2 archivos")
                                                      .arg(progress.foldersRead)
                                                      .arg(progress.filesCompared));
                    }
                }, Qt::QueuedConnection);
            });
            QMetaObject::invokeMethod(qApp, [self, generation, result] {
                if (self) {
                    self->onCompared(generation, result);
                }
            }, Qt::QueuedConnection);
        });
    updateButtons();
}

void SyncDialog::onCompared(quint64 generation, const sync::Comparison &comparison)
{
    if (generation != m_generation) {
        return;
    }
    m_comparing = false;
    m_task = tasks::TaskHandle();
    m_comparison = comparison;
    m_hasComparison = !comparison.cancelled;
    m_model->setComparison(m_hasComparison ? &m_comparison : nullptr, m_showIdentical->isChecked());
    // El estado de la última sincronización de este par (para los dos sentidos).
    const auto loaded = sync::loadState(sync::stateFile(m_stateDirectory, comparison.left, comparison.right));
    m_previous = loaded.ok() ? loaded.value() : sync::SyncState();
    if (m_saveStateAfterCompare && m_hasComparison) {
        m_saveStateAfterCompare = false;
        const sync::SyncState state = sync::stateFrom(comparison, m_appliedPlan, &m_previous);
        sync::saveState(sync::stateFile(m_stateDirectory, comparison.left, comparison.right), state);
        m_previous = state;
    }
    m_progress->setText(comparison.cancelled ? tr("Comparación detenida.") : QString());
    updateSummary();
    updateButtons();
    emit compareFinished();
}

void SyncDialog::invalidateComparison()
{
    if (m_hasComparison) {
        m_hasComparison = false;
        m_model->setComparison(nullptr, false);
    }
    invalidatePlan();
    updateSummary();
}

void SyncDialog::invalidatePlan()
{
    m_planValid = false;
    m_model->setPlan(nullptr);
    updateButtons();
    updateSummary();
}

sync::SyncOptions SyncDialog::syncOptions() const
{
    sync::SyncOptions options;
    options.direction = m_both->isChecked()     ? sync::Direction::Both
                        : m_toLeft->isChecked() ? sync::Direction::RightToLeft
                                                : sync::Direction::LeftToRight;
    options.conflicts = sync::ConflictPolicy(m_conflicts->currentData().toInt());
    options.propagateDeletions = m_deletions->isChecked();
    return options;
}

void SyncDialog::simulate()
{
    if (!m_hasComparison || isBusy()) {
        return;
    }
    const sync::SyncOptions options = syncOptions();
    m_plan = sync::plan(m_comparison, options, options.direction == sync::Direction::Both ? &m_previous : nullptr);
    m_planValid = true;
    m_model->setPlan(&m_plan);
    updateSummary();
    updateButtons();
}

void SyncDialog::synchronize()
{
    if (!m_planValid || !m_plan.hasChanges() || isBusy() || m_actions == nullptr || !m_actions->isAvailable()) {
        return;
    }
    const int toRight = m_plan.count(sync::ActionKind::CopyToRight);
    const int toLeft = m_plan.count(sync::ActionKind::CopyToLeft);
    const int trashed = m_plan.count(sync::ActionKind::TrashLeft) + m_plan.count(sync::ActionKind::TrashRight);
    bool confirmed = false;
    if (m_confirm) {
        confirmed = m_confirm();
    } else {
        QStringList lines;
        if (toRight > 0) {
            lines.append(tr("%n elemento(s) se copiarán a la derecha.", nullptr, toRight));
        }
        if (toLeft > 0) {
            lines.append(tr("%n elemento(s) se copiarán a la izquierda.", nullptr, toLeft));
        }
        if (trashed > 0) {
            lines.append(tr("%n elemento(s) irán a la papelera propia.", nullptr, trashed));
        }
        lines.append(tr("En total %1. Lo que se reemplace va a la papelera; todo se deshace junto con Deshacer.")
                         .arg(core::formatSize(m_plan.bytesToCopy)));
        confirmed = QMessageBox::question(this, windowTitle(), lines.join(QLatin1Char('\n')),
                                          QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
            == QMessageBox::Yes;
    }
    if (!confirmed) {
        return;
    }
    const QString left = m_comparison.left;
    const QString right = m_comparison.right;
    const auto join = [](const QString &root, const QString &relative) { return root + QLatin1Char('/') + relative; };
    QList<QPair<QString, QString>> copies;
    QStringList toTrash;
    for (const sync::Action &action : std::as_const(m_plan.actions)) {
        switch (action.kind) {
        case sync::ActionKind::CopyToRight:
            copies.append({join(left, action.relativePath), join(right, action.relativePath)});
            break;
        case sync::ActionKind::CopyToLeft:
            copies.append({join(right, action.relativePath), join(left, action.relativePath)});
            break;
        case sync::ActionKind::TrashLeft:
            toTrash.append(join(left, action.relativePath));
            break;
        case sync::ActionKind::TrashRight:
            toTrash.append(join(right, action.relativePath));
            break;
        case sync::ActionKind::Skip:
            break;
        }
    }
    const QString description = tr("Sincronizar «%1» y «%2»")
                                    .arg(QFileInfo(left).fileName().isEmpty() ? QDir::toNativeSeparators(left)
                                                                              : QFileInfo(left).fileName(),
                                         QFileInfo(right).fileName().isEmpty() ? QDir::toNativeSeparators(right)
                                                                               : QFileInfo(right).fileName());
    m_appliedPlan = m_plan;
    m_runningSync = m_actions->synchronize(copies, toTrash, description, m_verify->isChecked(), {left, right});
    m_progress->setText(tr("Sincronizando… (el avance se ve en Transferencias)"));
    updateButtons();
}

void SyncDialog::onActionFinished(const ActionResult &result)
{
    if (result.id != m_runningSync || m_runningSync == 0) {
        return;
    }
    m_runningSync = 0;
    // Se vuelve a comparar para enseñar cómo quedó y guardar el estado del par.
    m_saveStateAfterCompare = result.outcome.ok();
    startCompare();
    emit syncFinished();
}

void SyncDialog::updateButtons()
{
    const bool busy = isBusy();
    m_compareButton->setEnabled(!busy);
    m_stopButton->setEnabled(m_comparing);
    m_simulateButton->setEnabled(m_hasComparison && !busy);
    m_syncButton->setEnabled(m_planValid && m_plan.hasChanges() && !busy && m_actions != nullptr
                             && m_actions->isAvailable());
    m_syncButton->setToolTip(m_planValid ? QString() : tr("Primero hay que simular con las opciones elegidas"));
}

void SyncDialog::updateSummary()
{
    if (!m_hasComparison) {
        m_summary->setText(m_comparing ? QString() : tr("Elige las dos carpetas y pulsa «Comparar»."));
        return;
    }
    QStringList parts;
    const auto add = [&](sync::State state, const QString &text) {
        const int count = m_comparison.count(state);
        if (count > 0) {
            parts.append(text.arg(count));
        }
    };
    add(sync::State::Different, tr("%1 distintos"));
    add(sync::State::LeftNewer, tr("%1 más recientes a la izquierda"));
    add(sync::State::RightNewer, tr("%1 más recientes a la derecha"));
    add(sync::State::LeftOnly, tr("%1 solo a la izquierda"));
    add(sync::State::RightOnly, tr("%1 solo a la derecha"));
    add(sync::State::TypeConflict, tr("%1 archivo contra carpeta"));
    QString text = parts.isEmpty() ? tr("Las dos carpetas son iguales.") : tr("Diferencias: %1.").arg(parts.join(QStringLiteral(", ")));
    text += QLatin1Char(' ') + tr("%n idéntico(s).", nullptr, m_comparison.count(sync::State::Identical));
    if (m_comparison.skippedLinks > 0) {
        text += QLatin1Char(' ') + tr("%n enlace(s) simbólico(s) sin comparar.", nullptr, m_comparison.skippedLinks);
    }
    if (!m_comparison.errors.isEmpty()) {
        text += QLatin1Char(' ') + tr("%n elemento(s) no se pudieron leer: %1", nullptr, int(m_comparison.errors.size()))
                                       .arg(m_comparison.errors.first().message);
    }
    if (m_planValid) {
        const int toRight = m_plan.count(sync::ActionKind::CopyToRight);
        const int toLeft = m_plan.count(sync::ActionKind::CopyToLeft);
        const int trashed = m_plan.count(sync::ActionKind::TrashLeft) + m_plan.count(sync::ActionKind::TrashRight);
        text += QStringLiteral("\n")
            + (m_plan.hasChanges()
                   ? tr("Simulación: %1 → derecha, %2 ← izquierda, %3 a la papelera; %4 en total.")
                         .arg(toRight)
                         .arg(toLeft)
                         .arg(trashed)
                         .arg(core::formatSize(m_plan.bytesToCopy))
                   : tr("Simulación: no hay nada que hacer con estas opciones."));
        if (m_plan.conflicts > 0) {
            text += QLatin1Char(' ') + tr("%n conflicto(s) sin resolver (no se tocan).", nullptr, m_plan.conflicts);
        }
        if (m_plan.deletionsNotPropagated > 0) {
            text += QLatin1Char(' ')
                + tr("%n borrado(s) no se propagan (activa «Propagar borrados» para quitarlos).", nullptr,
                     m_plan.deletionsNotPropagated);
        }
        if (syncOptions().direction == sync::Direction::Both && m_previous.isEmpty()) {
            text += QLatin1Char(' ') + tr("Es la primera sincronización de este par: lo distinto en los dos lados "
                                          "cuenta como conflicto.");
        }
    }
    m_summary->setText(text);
}

void SyncDialog::setDirection(sync::Direction direction)
{
    (direction == sync::Direction::Both ? m_both : direction == sync::Direction::RightToLeft ? m_toLeft : m_toRight)
        ->setChecked(true);
}

void SyncDialog::setConflictPolicy(sync::ConflictPolicy policy)
{
    m_conflicts->setCurrentIndex(m_conflicts->findData(int(policy)));
}

void SyncDialog::setPropagateDeletions(bool on)
{
    m_deletions->setChecked(on);
}

void SyncDialog::setCriterion(sync::Criterion criterion)
{
    m_criterion->setCurrentIndex(m_criterion->findData(int(criterion)));
}

} // namespace eu::ui
