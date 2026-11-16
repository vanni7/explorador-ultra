// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/OrganizeDialog.h"

#include "eu/ops/Engine.h"
#include "eu/ui/BulkRenameDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace eu::ui {

namespace {

enum Column { NameColumn, ExtensionsColumn, FolderColumn, ColumnCount };
const QString kSavedRules = QStringLiteral("organizador/reglas");

} // namespace

OrganizeDialog::OrganizeDialog(tasks::Scheduler &scheduler, QString folder, QWidget *parent)
    : QDialog(parent), m_scheduler(scheduler), m_folder(QDir::cleanPath(folder))
{
    setWindowTitle(tr("Organizar «%1»").arg(QFileInfo(m_folder).fileName()));
    resize(1000, 680);

    m_preset = new QComboBox(this);
    const QByteArray saved = QSettings().value(kSavedRules).toByteArray();
    if (!saved.isEmpty()) {
        m_preset->addItem(tr("Mis reglas guardadas"), saved);
    }
    for (const auto &[name, rules] : organize::presets()) {
        m_preset->addItem(name, organize::rulesToJson(rules));
    }
    m_recursive = new QCheckBox(tr("Incluir lo de las subcarpetas"), this);
    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Reglas:"), this));
    top->addWidget(m_preset, 1);
    top->addWidget(m_recursive);

    m_rules = new QTableWidget(0, ColumnCount, this);
    m_rules->setHorizontalHeaderLabels({tr("Nombre"), tr("Extensiones (separadas por espacios; vacío: todas)"),
                                        tr("Subcarpeta de destino (admite datos: {fecha:yyyy}, {artista}…)")});
    m_rules->horizontalHeader()->setSectionResizeMode(FolderColumn, QHeaderView::Stretch);
    m_rules->horizontalHeader()->resizeSection(NameColumn, 140);
    m_rules->horizontalHeader()->resizeSection(ExtensionsColumn, 300);
    m_rules->verticalHeader()->setVisible(false);
    m_rules->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_rules->setToolTip(tr("Gana la primera regla que encaja. Lo que no cubre ninguna se queda donde está."));
    auto *addButton = new QPushButton(tr("Añadir"), this);
    auto *removeButton = new QPushButton(tr("Quitar"), this);
    auto *upButton = new QPushButton(tr("Subir"), this);
    auto *downButton = new QPushButton(tr("Bajar"), this);
    auto *saveButton = new QPushButton(tr("Guardar como mis reglas"), this);
    auto *ruleButtons = new QHBoxLayout;
    for (QPushButton *button : {addButton, removeButton, upButton, downButton}) {
        ruleButtons->addWidget(button);
    }
    ruleButtons->addStretch(1);
    ruleButtons->addWidget(saveButton);
    auto *rulesBox = new QWidget(this);
    auto *rulesLayout = new QVBoxLayout(rulesBox);
    rulesLayout->setContentsMargins(0, 0, 0, 0);
    rulesLayout->addWidget(m_rules, 1);
    rulesLayout->addLayout(ruleButtons);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Cómo quedará"), tr("Viene de / nota")});
    m_tree->header()->resizeSection(0, 420);
    m_tree->setAccessibleName(tr("Simulación"));
    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(rulesBox);
    splitter->addWidget(m_tree);
    splitter->setStretchFactor(1, 2);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_applyButton = new QPushButton(tr("Organizar"), this);
    m_applyButton->setDefault(true);
    m_applyButton->setEnabled(false);
    auto *cancelButton = new QPushButton(tr("Cancelar"), this);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_summary, 1);
    bottom->addWidget(m_applyButton);
    bottom->addWidget(cancelButton);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(splitter, 1);
    layout->addLayout(bottom);

    connect(m_applyButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_preset, &QComboBox::currentIndexChanged, this, [this] {
        loadRulesIntoTable(organize::rulesFromJson(m_preset->currentData().toByteArray()));
    });
    connect(m_recursive, &QCheckBox::toggled, this, &OrganizeDialog::scheduleSimulation);
    connect(m_rules, &QTableWidget::itemChanged, this, [this] {
        if (!m_loading) {
            scheduleSimulation();
        }
    });
    connect(addButton, &QPushButton::clicked, this, [this] {
        QList<organize::Rule> rules = rulesFromTable();
        rules.append(organize::Rule{tr("Nueva"), {}, -1, -1, tr("Carpeta")});
        loadRulesIntoTable(rules);
        m_rules->selectRow(m_rules->rowCount() - 1);
    });
    connect(removeButton, &QPushButton::clicked, this, [this] {
        const int row = m_rules->currentRow();
        QList<organize::Rule> rules = rulesFromTable();
        if (row >= 0 && row < rules.size()) {
            rules.removeAt(row);
            loadRulesIntoTable(rules);
        }
    });
    const auto move = [this](int delta) {
        const int row = m_rules->currentRow();
        QList<organize::Rule> rules = rulesFromTable();
        if (row < 0 || row + delta < 0 || row + delta >= rules.size()) {
            return;
        }
        rules.swapItemsAt(row, row + delta);
        loadRulesIntoTable(rules);
        m_rules->selectRow(row + delta);
    };
    connect(upButton, &QPushButton::clicked, this, [move] { move(-1); });
    connect(downButton, &QPushButton::clicked, this, [move] { move(1); });
    connect(saveButton, &QPushButton::clicked, this, [this] {
        QSettings().setValue(kSavedRules, organize::rulesToJson(rulesFromTable()));
        m_summary->setText(tr("Reglas guardadas: aparecerán como «Mis reglas guardadas».") + QLatin1Char(' ')
                           + m_summary->text());
    });

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(250);
    connect(&m_debounce, &QTimer::timeout, this, &OrganizeDialog::startSimulation);
    loadRulesIntoTable(organize::rulesFromJson(m_preset->currentData().toByteArray()));
}

OrganizeDialog::~OrganizeDialog()
{
    m_task.cancel();
}

void OrganizeDialog::setRules(const QList<organize::Rule> &rules)
{
    loadRulesIntoTable(rules);
}

void OrganizeDialog::setRecursive(bool recursive)
{
    m_recursive->setChecked(recursive);
    scheduleSimulation();
}

void OrganizeDialog::loadRulesIntoTable(const QList<organize::Rule> &rules)
{
    m_loading = true;
    m_rules->setRowCount(0);
    for (const organize::Rule &rule : rules) {
        const int row = m_rules->rowCount();
        m_rules->insertRow(row);
        m_rules->setItem(row, NameColumn, new QTableWidgetItem(rule.name));
        m_rules->setItem(row, ExtensionsColumn, new QTableWidgetItem(rule.extensions.join(QLatin1Char(' '))));
        m_rules->setItem(row, FolderColumn, new QTableWidgetItem(rule.folder));
    }
    m_loading = false;
    scheduleSimulation();
}

QList<organize::Rule> OrganizeDialog::rulesFromTable() const
{
    QList<organize::Rule> rules;
    for (int row = 0; row < m_rules->rowCount(); ++row) {
        const auto text = [&](int column) {
            const QTableWidgetItem *item = m_rules->item(row, column);
            return item != nullptr ? item->text().trimmed() : QString();
        };
        organize::Rule rule;
        rule.name = text(NameColumn);
        rule.extensions = text(ExtensionsColumn).toLower().remove(QLatin1Char('.')).split(QLatin1Char(' '),
                                                                                          Qt::SkipEmptyParts);
        rule.folder = text(FolderColumn);
        if (!rule.folder.isEmpty()) {
            rules.append(rule);
        }
    }
    return rules;
}

void OrganizeDialog::scheduleSimulation()
{
    ++m_generation;
    m_applyButton->setEnabled(false);   // siempre se ve la simulación de lo que se va a hacer
    m_summary->setText(tr("Simulando…"));
    m_debounce.start();
}

void OrganizeDialog::startSimulation()
{
    m_task.cancel();
    organize::Options options;
    options.rules = rulesFromTable();
    options.recursive = m_recursive->isChecked();
    const QPointer<OrganizeDialog> self(this);
    const quint64 generation = m_generation;
    m_task = m_scheduler.submitForPaths(
        tasks::Category::General, tasks::Priority::UserInteraction, {m_folder},
        [self, generation, options, folder = m_folder](const tasks::CancelToken &cancel) {
            organize::PlanOptions planOptions;
            planOptions.rulesFor = [](const QString &directory) { return ops::nameRulesFor(directory); };
            planOptions.metadataFor = [&cancel](const QString &path) { return renameMetadataFor(path, cancel); };
            const organize::Plan plan = organize::simulate(folder, options, cancel, planOptions);
            if (cancel.isCancelled()) {
                return;
            }
            QMetaObject::invokeMethod(qApp, [self, generation, plan] {
                if (self) {
                    self->showSimulation(generation, plan);
                }
            }, Qt::QueuedConnection);
        });
}

void OrganizeDialog::showSimulation(quint64 generation, const organize::Plan &plan)
{
    if (generation != m_generation) {
        return;
    }
    m_shownGeneration = generation;
    m_plan = plan;
    m_tree->clear();
    // El árbol resultante: cada carpeta de destino con lo que recibirá.
    QHash<QString, QTreeWidgetItem *> folders;
    const QSet<QString> created(plan.foldersToCreate.constBegin(), plan.foldersToCreate.constEnd());
    const std::function<QTreeWidgetItem *(const QString &)> folderItem = [&](const QString &path) -> QTreeWidgetItem * {
        if (path == m_folder) {
            return nullptr;
        }
        if (QTreeWidgetItem *existing = folders.value(path)) {
            return existing;
        }
        QTreeWidgetItem *parent = folderItem(QFileInfo(path).absolutePath());
        auto *item = parent != nullptr ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
        item->setText(0, QFileInfo(path).fileName());
        item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        if (created.contains(path)) {
            QFont font = item->font(0);
            font.setBold(true);
            item->setFont(0, font);
            item->setText(1, tr("carpeta nueva"));
        }
        folders.insert(path, item);
        return item;
    };
    for (const organize::Move &move : plan.moves) {
        QTreeWidgetItem *parent = folderItem(QFileInfo(move.target).absolutePath());
        auto *item = parent != nullptr ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
        item->setText(0, QFileInfo(move.target).fileName());
        const QString from = QDir(m_folder).relativeFilePath(move.source);
        item->setText(1, move.note.isEmpty() ? tr("← %1").arg(from) : tr("← %1 · %2").arg(from, move.note));
        item->setToolTip(1, item->text(1));
    }
    m_tree->sortItems(0, Qt::AscendingOrder);
    m_tree->expandToDepth(1);

    QStringList parts{tr("%n archivo(s) se moverán", nullptr, int(plan.moves.size()))};
    if (!plan.foldersToCreate.isEmpty()) {
        parts.append(tr("%n carpeta(s) nueva(s)", nullptr, int(plan.foldersToCreate.size())));
    }
    if (plan.alreadyInPlace > 0) {
        parts.append(tr("%n ya en su sitio", nullptr, plan.alreadyInPlace));
    }
    if (plan.withoutRule > 0) {
        parts.append(tr("%n sin regla (se quedan)", nullptr, plan.withoutRule));
    }
    m_summary->setText(parts.join(QStringLiteral(" · ")) + tr(". Se deshace todo junto con Deshacer."));
    m_applyButton->setEnabled(!plan.moves.isEmpty());
    emit simulated();
}

} // namespace eu::ui
