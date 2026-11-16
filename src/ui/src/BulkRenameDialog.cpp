// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/BulkRenameDialog.h"

#include "eu/core/NaturalCompare.h"
#include "eu/ops/Engine.h"
#include "eu/thumbs/Isolated.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace eu::ui {

namespace {

using Kind = rename::Rule::Kind;

/// Orden de las páginas del editor: el de Rule::Kind.
const QList<Kind> &allKinds()
{
    static const QList<Kind> kinds{Kind::Case, Kind::Replace, Kind::Remove, Kind::Insert,
                                   Kind::Regex, Kind::Counter, Kind::Template};
    return kinds;
}

QString kindName(Kind kind)
{
    switch (kind) {
    case Kind::Case:     return BulkRenameDialog::tr("Mayúsculas y minúsculas");
    case Kind::Replace:  return BulkRenameDialog::tr("Sustituir texto");
    case Kind::Remove:   return BulkRenameDialog::tr("Eliminar caracteres");
    case Kind::Insert:   return BulkRenameDialog::tr("Añadir texto (prefijo, sufijo…)");
    case Kind::Regex:    return BulkRenameDialog::tr("Expresión regular");
    case Kind::Counter:  return BulkRenameDialog::tr("Contador");
    case Kind::Template: return BulkRenameDialog::tr("Nombre con datos (fecha, EXIF, etiquetas…)");
    }
    return {};
}

QString tokensHelp()
{
    QStringList lines{BulkRenameDialog::tr("Datos que se pueden usar:")};
    for (const auto &[token, meaning] : rename::templateTokens()) {
        lines.append(QStringLiteral("%1 — %2").arg(token, meaning));
    }
    return lines.join(QLatin1Char('\n'));
}

QSpinBox *spin(QWidget *parent, int minimum, int maximum, int value)
{
    auto *box = new QSpinBox(parent);
    box->setRange(minimum, maximum);
    box->setValue(value);
    return box;
}

} // namespace

rename::Metadata renameMetadataFor(const QString &path, const tasks::CancelToken &cancel)
{
    rename::Metadata data;
    // Los rótulos son los que pone el módulo de metadatos (meta::readMetadata).
    static const QHash<QString, QString> keys{
        {QStringLiteral("Fecha de la foto"), QStringLiteral("fecha_foto")},
        {QStringLiteral("Cámara"), QStringLiteral("camara")},
        {QStringLiteral("Título"), QStringLiteral("titulo")},
        {QStringLiteral("Artista"), QStringLiteral("artista")},
        {QStringLiteral("Álbum"), QStringLiteral("album")},
        {QStringLiteral("Año"), QStringLiteral("año")},
        {QStringLiteral("Pista"), QStringLiteral("pista")},
        {QStringLiteral("Género"), QStringLiteral("genero")},
    };
    static const QRegularExpression size(QStringLiteral("(\\d+)\\s*[×x]\\s*(\\d+)"));
    for (const meta::Section &section : thumbs::readMetadataIsolated(path, cancel)) {
        for (const meta::Field &field : section.fields) {
            const QString key = keys.value(field.label);
            if (!key.isEmpty() && !data.contains(key)) {
                data.insert(key, field.value);
            }
            if ((field.label == QLatin1String("Dimensiones") || field.label == QStringLiteral("Resolución"))
                && !data.contains(QStringLiteral("ancho"))) {
                const QRegularExpressionMatch match = size.match(field.value);
                if (match.hasMatch()) {
                    data.insert(QStringLiteral("ancho"), match.captured(1));
                    data.insert(QStringLiteral("alto"), match.captured(2));
                }
            }
        }
    }
    return data;
}

BulkRenameDialog::BulkRenameDialog(tasks::Scheduler &scheduler, QStringList selection, QString folder, QWidget *parent)
    : QDialog(parent), m_scheduler(scheduler), m_selection(std::move(selection)), m_folder(std::move(folder))
{
    setWindowTitle(tr("Renombrar varios"));
    resize(980, 640);

    // Alcance.
    m_scope = new QComboBox(this);
    m_scope->addItem(tr("Los %n seleccionados", nullptr, int(m_selection.size())), int(Scope::Selection));
    m_scope->addItem(tr("Todo lo de «%1»").arg(QFileInfo(m_folder).fileName()), int(Scope::Folder));
    m_scope->addItem(tr("«%1» y sus subcarpetas").arg(QFileInfo(m_folder).fileName()), int(Scope::Recursive));
    if (m_selection.isEmpty()) {
        m_scope->setCurrentIndex(1);
    }
    m_includeFolders = new QCheckBox(tr("Incluir carpetas"), this);
    m_includeFolders->setChecked(true);
    m_extension = new QComboBox(this);
    m_extension->addItem(tr("Conservar la extensión"), int(rename::ExtensionMode::Keep));
    m_extension->addItem(tr("Extensión en minúsculas"), int(rename::ExtensionMode::Lower));
    m_extension->addItem(tr("Extensión en MAYÚSCULAS"), int(rename::ExtensionMode::Upper));
    m_extension->addItem(tr("Las reglas también cambian la extensión"), int(rename::ExtensionMode::Include));
    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Renombrar:"), this));
    top->addWidget(m_scope, 1);
    top->addWidget(m_includeFolders);
    top->addSpacing(12);
    top->addWidget(m_extension);

    // Reglas.
    m_rules = new QListWidget(this);
    m_rules->setAccessibleName(tr("Reglas, en el orden en que se aplican"));
    auto *addButton = new QToolButton(this);
    addButton->setText(tr("Añadir regla"));
    addButton->setPopupMode(QToolButton::InstantPopup);
    auto *addMenu = new QMenu(addButton);
    for (const Kind kind : allKinds()) {
        addMenu->addAction(kindName(kind), this, [this, kind] { addRule(kind); });
    }
    addButton->setMenu(addMenu);
    auto *removeButton = new QToolButton(this);
    removeButton->setText(tr("Quitar"));
    auto *upButton = new QToolButton(this);
    upButton->setText(tr("Subir"));
    auto *downButton = new QToolButton(this);
    downButton->setText(tr("Bajar"));
    auto *ruleButtons = new QHBoxLayout;
    ruleButtons->addWidget(addButton);
    ruleButtons->addWidget(removeButton);
    ruleButtons->addWidget(upButton);
    ruleButtons->addWidget(downButton);
    ruleButtons->addStretch(1);

    // Editores, uno por clase de regla.
    m_editors = new QStackedWidget(this);
    const auto page = [this](const std::function<void(QFormLayout *, QWidget *)> &fill) {
        auto *widget = new QWidget(m_editors);
        auto *form = new QFormLayout(widget);
        form->setContentsMargins(0, 0, 0, 0);
        fill(form, widget);
        m_editors->addWidget(widget);
    };
    page([this](QFormLayout *form, QWidget *w) {
        m_caseMode = new QComboBox(w);
        m_caseMode->addItems({tr("MAYÚSCULAS"), tr("minúsculas"), tr("Tipo Título"), tr("Tipo oración")});
        form->addRow(tr("Poner en:"), m_caseMode);
    });
    page([this](QFormLayout *form, QWidget *w) {
        m_find = new QLineEdit(w);
        m_with = new QLineEdit(w);
        m_caseSensitive = new QCheckBox(tr("Distinguir mayúsculas"), w);
        form->addRow(tr("Buscar:"), m_find);
        form->addRow(tr("Sustituir por:"), m_with);
        form->addRow(QString(), m_caseSensitive);
    });
    page([this](QFormLayout *form, QWidget *w) {
        m_removeMode = new QComboBox(w);
        m_removeMode->addItems({tr("Estos caracteres"), tr("Los primeros"), tr("Los últimos"), tr("Desde una posición")});
        m_removeCharacters = new QLineEdit(w);
        m_removeCharacters->setPlaceholderText(tr("p. ej. _-()"));
        m_removeCount = spin(w, 0, 999, 1);
        m_removeFrom = spin(w, 0, 999, 0);
        form->addRow(tr("Quitar:"), m_removeMode);
        form->addRow(tr("Caracteres:"), m_removeCharacters);
        form->addRow(tr("Cuántos:"), m_removeCount);
        form->addRow(tr("Desde la posición (0 = la primera):"), m_removeFrom);
    });
    page([this](QFormLayout *form, QWidget *w) {
        m_insertText = new QLineEdit(w);
        m_insertText->setToolTip(tokensHelp());
        m_insertText->setPlaceholderText(tr("Texto; admite datos como {fecha:yyyy-MM-dd}"));
        m_insertPosition = new QComboBox(w);
        m_insertPosition->addItems({tr("Al principio (prefijo)"), tr("Al final (sufijo)"), tr("En una posición")});
        m_insertAt = spin(w, 0, 999, 0);
        form->addRow(tr("Texto:"), m_insertText);
        form->addRow(tr("Dónde:"), m_insertPosition);
        form->addRow(tr("Posición (0 = la primera):"), m_insertAt);
    });
    page([this](QFormLayout *form, QWidget *w) {
        m_regexFind = new QLineEdit(w);
        m_regexFind->setPlaceholderText(tr("p. ej. ^IMG_(\\d+)$"));
        m_regexWith = new QLineEdit(w);
        m_regexWith->setPlaceholderText(tr("p. ej. Foto $1"));
        m_regexCase = new QCheckBox(tr("Distinguir mayúsculas"), w);
        form->addRow(tr("Expresión:"), m_regexFind);
        form->addRow(tr("Sustituir por:"), m_regexWith);
        form->addRow(QString(), m_regexCase);
    });
    page([this](QFormLayout *form, QWidget *w) {
        m_counterStart = spin(w, 0, 999999, 1);
        m_counterStep = spin(w, 1, 1000, 1);
        m_counterDigits = spin(w, 1, 9, 3);
        m_counterPosition = new QComboBox(w);
        m_counterPosition->addItems({tr("Al principio"), tr("Al final")});
        m_counterSeparator = new QLineEdit(QStringLiteral(" "), w);
        form->addRow(tr("Empieza en:"), m_counterStart);
        form->addRow(tr("Sube de:"), m_counterStep);
        form->addRow(tr("Cifras (con ceros):"), m_counterDigits);
        form->addRow(tr("Dónde:"), m_counterPosition);
        form->addRow(tr("Separador:"), m_counterSeparator);
    });
    page([this](QFormLayout *form, QWidget *w) {
        m_template = new QLineEdit(w);
        m_template->setToolTip(tokensHelp());
        m_template->setPlaceholderText(QStringLiteral("{fecha_foto:yyyy-MM-dd} {nombre}"));
        auto *help = new QLabel(tokensHelp(), w);
        help->setWordWrap(true);
        help->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(tr("Nombre nuevo:"), m_template);
        form->addRow(help);
    });
    auto *noRule = new QLabel(tr("Añade una regla con «Añadir regla». Se aplican en orden, cada una sobre lo que "
                                 "dejó la anterior."), m_editors);
    noRule->setWordWrap(true);
    m_editors->addWidget(noRule);
    m_editors->setCurrentWidget(noRule);

    auto *rulesBox = new QVBoxLayout;
    rulesBox->addLayout(ruleButtons);
    rulesBox->addWidget(m_rules, 1);
    auto *middle = new QHBoxLayout;
    middle->addLayout(rulesBox, 1);
    middle->addWidget(m_editors, 2);

    // Vista previa.
    m_preview = new QTreeWidget(this);
    m_preview->setColumnCount(3);
    m_preview->setHeaderLabels({tr("Nombre actual"), tr("Nombre nuevo"), tr("Estado")});
    m_preview->setRootIsDecorated(false);
    m_preview->setUniformRowHeights(true);
    m_preview->header()->resizeSection(0, 300);
    m_preview->header()->resizeSection(1, 300);
    m_summary = new QLabel(this);
    m_renameButton = new QPushButton(tr("Renombrar"), this);
    m_renameButton->setDefault(true);
    auto *cancelButton = new QPushButton(tr("Cancelar"), this);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_summary, 1);
    bottom->addWidget(m_renameButton);
    bottom->addWidget(cancelButton);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addLayout(middle);
    layout->addWidget(m_preview, 2);
    layout->addLayout(bottom);

    connect(m_renameButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_scope, &QComboBox::currentIndexChanged, this, &BulkRenameDialog::schedulePreview);
    connect(m_includeFolders, &QCheckBox::toggled, this, &BulkRenameDialog::schedulePreview);
    connect(m_extension, &QComboBox::currentIndexChanged, this, [this] {
        m_options.extension = rename::ExtensionMode(m_extension->currentData().toInt());
        schedulePreview();
    });
    connect(m_rules, &QListWidget::currentRowChanged, this, &BulkRenameDialog::loadEditor);
    connect(removeButton, &QToolButton::clicked, this, [this] {
        const int row = m_rules->currentRow();
        if (row >= 0 && row < m_options.rules.size()) {
            m_options.rules.removeAt(row);
            refreshRuleList();
            m_rules->setCurrentRow(std::min(row, int(m_options.rules.size()) - 1));
            loadEditor();
            schedulePreview();
        }
    });
    const auto move = [this](int delta) {
        const int row = m_rules->currentRow();
        const int target = row + delta;
        if (row < 0 || target < 0 || target >= m_options.rules.size()) {
            return;
        }
        m_options.rules.swapItemsAt(row, target);
        refreshRuleList();
        m_rules->setCurrentRow(target);
        schedulePreview();
    };
    connect(upButton, &QToolButton::clicked, this, [move] { move(-1); });
    connect(downButton, &QToolButton::clicked, this, [move] { move(1); });

    // Cualquier cambio en un editor actualiza la regla y la vista previa.
    for (QLineEdit *edit : {m_find, m_with, m_removeCharacters, m_insertText, m_regexFind, m_regexWith,
                            m_counterSeparator, m_template}) {
        connect(edit, &QLineEdit::textChanged, this, &BulkRenameDialog::ruleEdited);
    }
    for (QComboBox *box : {m_caseMode, m_removeMode, m_insertPosition, m_counterPosition}) {
        connect(box, &QComboBox::currentIndexChanged, this, &BulkRenameDialog::ruleEdited);
    }
    for (QSpinBox *box : {m_removeCount, m_removeFrom, m_insertAt, m_counterStart, m_counterStep, m_counterDigits}) {
        connect(box, &QSpinBox::valueChanged, this, &BulkRenameDialog::ruleEdited);
    }
    for (QCheckBox *box : {m_caseSensitive, m_regexCase}) {
        connect(box, &QCheckBox::toggled, this, &BulkRenameDialog::ruleEdited);
    }

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(150);
    connect(&m_debounce, &QTimer::timeout, this, &BulkRenameDialog::startPreview);
    schedulePreview();
}

BulkRenameDialog::~BulkRenameDialog()
{
    m_task.cancel();
}

void BulkRenameDialog::setRules(const QList<rename::Rule> &rules)
{
    m_options.rules = rules;
    refreshRuleList();
    m_rules->setCurrentRow(rules.isEmpty() ? -1 : 0);
    loadEditor();
    schedulePreview();
}

void BulkRenameDialog::setExtensionMode(rename::ExtensionMode mode)
{
    m_extension->setCurrentIndex(m_extension->findData(int(mode)));
}

void BulkRenameDialog::setScope(Scope scope, bool includeFolders)
{
    m_includeFolders->setChecked(includeFolders);
    m_scope->setCurrentIndex(m_scope->findData(int(scope)));
    schedulePreview();
}

void BulkRenameDialog::addRule(rename::Rule::Kind kind)
{
    rename::Rule rule;
    rule.kind = kind;
    if (kind == Kind::Counter) {
        rule.count = 3;
    }
    if (kind == Kind::Template) {
        rule.with = QStringLiteral("{nombre}");
    }
    m_options.rules.append(rule);
    refreshRuleList();
    m_rules->setCurrentRow(int(m_options.rules.size()) - 1);
    loadEditor();
    schedulePreview();
}

QString BulkRenameDialog::describe(const rename::Rule &rule) const
{
    switch (rule.kind) {
    case Kind::Case:
        return QStringList{tr("Todo en MAYÚSCULAS"), tr("Todo en minúsculas"), tr("Tipo Título"),
                           tr("Tipo oración")}.value(int(rule.caseMode));
    case Kind::Replace:
        return tr("Sustituir «%1» por «%2»").arg(rule.find, rule.with);
    case Kind::Remove:
        switch (rule.removeMode) {
        case rename::Rule::RemoveMode::Characters: return tr("Quitar «%1»").arg(rule.characters);
        case rename::Rule::RemoveMode::First:      return tr("Quitar los %1 primeros").arg(rule.count);
        case rename::Rule::RemoveMode::Last:       return tr("Quitar los %1 últimos").arg(rule.count);
        case rename::Rule::RemoveMode::Range:      return tr("Quitar %1 desde la posición %2").arg(rule.count).arg(rule.from);
        }
        return {};
    case Kind::Insert:
        return rule.position == rename::Rule::Position::Start ? tr("Prefijo «%1»").arg(rule.with)
            : rule.position == rename::Rule::Position::End    ? tr("Sufijo «%1»").arg(rule.with)
                                                              : tr("Insertar «%1» en %2").arg(rule.with).arg(rule.from);
    case Kind::Regex:
        return tr("Expresión «%1» → «%2»").arg(rule.find, rule.with);
    case Kind::Counter:
        return tr("Contador desde %1 (%2 cifras) %3").arg(rule.start).arg(rule.count)
            .arg(rule.position == rename::Rule::Position::End ? tr("al final") : tr("al principio"));
    case Kind::Template:
        return tr("Nombre: %1").arg(rule.with);
    }
    return {};
}

void BulkRenameDialog::refreshRuleList()
{
    const QSignalBlocker blocker(m_rules);
    const int row = m_rules->currentRow();
    m_rules->clear();
    for (int i = 0; i < m_options.rules.size(); ++i) {
        m_rules->addItem(QStringLiteral("%1. %2").arg(i + 1).arg(describe(m_options.rules[i])));
    }
    m_rules->setCurrentRow(std::min(row, int(m_options.rules.size()) - 1));
}

void BulkRenameDialog::loadEditor()
{
    const int row = m_rules->currentRow();
    if (row < 0 || row >= m_options.rules.size()) {
        m_editors->setCurrentIndex(m_editors->count() - 1);
        return;
    }
    const rename::Rule &rule = m_options.rules[row];
    m_loading = true;
    m_editors->setCurrentIndex(int(allKinds().indexOf(rule.kind)));
    m_caseMode->setCurrentIndex(int(rule.caseMode));
    m_find->setText(rule.find);
    m_with->setText(rule.with);
    m_caseSensitive->setChecked(rule.caseSensitive);
    m_removeMode->setCurrentIndex(int(rule.removeMode));
    m_removeCharacters->setText(rule.characters);
    m_removeCount->setValue(rule.count);
    m_removeFrom->setValue(rule.from);
    m_insertText->setText(rule.with);
    m_insertPosition->setCurrentIndex(int(rule.position));
    m_insertAt->setValue(rule.from);
    m_regexFind->setText(rule.find);
    m_regexWith->setText(rule.with);
    m_regexCase->setChecked(rule.caseSensitive);
    m_counterStart->setValue(rule.start);
    m_counterStep->setValue(rule.step);
    m_counterDigits->setValue(rule.count);
    m_counterPosition->setCurrentIndex(rule.position == rename::Rule::Position::End ? 1 : 0);
    m_counterSeparator->setText(rule.separator);
    m_template->setText(rule.with);
    m_loading = false;
}

void BulkRenameDialog::ruleEdited()
{
    const int row = m_rules->currentRow();
    if (m_loading || row < 0 || row >= m_options.rules.size()) {
        return;
    }
    rename::Rule &rule = m_options.rules[row];
    switch (rule.kind) {
    case Kind::Case:
        rule.caseMode = rename::Rule::CaseMode(m_caseMode->currentIndex());
        break;
    case Kind::Replace:
        rule.find = m_find->text();
        rule.with = m_with->text();
        rule.caseSensitive = m_caseSensitive->isChecked();
        break;
    case Kind::Remove:
        rule.removeMode = rename::Rule::RemoveMode(m_removeMode->currentIndex());
        rule.characters = m_removeCharacters->text();
        rule.count = m_removeCount->value();
        rule.from = m_removeFrom->value();
        break;
    case Kind::Insert:
        rule.with = m_insertText->text();
        rule.position = rename::Rule::Position(m_insertPosition->currentIndex());
        rule.from = m_insertAt->value();
        break;
    case Kind::Regex:
        rule.find = m_regexFind->text();
        rule.with = m_regexWith->text();
        rule.caseSensitive = m_regexCase->isChecked();
        break;
    case Kind::Counter:
        rule.start = m_counterStart->value();
        rule.step = m_counterStep->value();
        rule.count = m_counterDigits->value();
        rule.position = m_counterPosition->currentIndex() == 1 ? rename::Rule::Position::End
                                                               : rename::Rule::Position::Start;
        rule.separator = m_counterSeparator->text();
        break;
    case Kind::Template:
        rule.with = m_template->text();
        break;
    }
    if (QListWidgetItem *item = m_rules->item(row)) {
        item->setText(QStringLiteral("%1. %2").arg(row + 1).arg(describe(rule)));
    }
    schedulePreview();
}

void BulkRenameDialog::schedulePreview()
{
    ++m_generation;
    m_renameButton->setEnabled(false);
    m_debounce.start();
}

void BulkRenameDialog::startPreview()
{
    m_task.cancel();
    const Scope scope = Scope(m_scope->currentData().toInt());
    const bool includeFolders = m_includeFolders->isChecked();
    const QPointer<BulkRenameDialog> self(this);
    const quint64 generation = m_generation;
    m_summary->setText(tr("Calculando…"));
    m_task = m_scheduler.submitForPaths(
        tasks::Category::General, tasks::Priority::UserInteraction, {m_folder},
        [self, generation, scope, includeFolders, selection = m_selection, folder = m_folder,
         options = m_options](const tasks::CancelToken &cancel) {
            // Qué se renombra.
            QStringList paths;
            if (scope == Scope::Selection) {
                paths = selection;
            } else {
                QDirIterator it(folder, QDir::AllEntries | QDir::NoDotAndDotDot,
                                scope == Scope::Recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
                while (it.hasNext() && !cancel.isCancelled()) {
                    paths.append(it.next());
                }
            }
            QList<rename::Item> items;
            for (const QString &path : std::as_const(paths)) {
                const QFileInfo info(path);
                if (!info.exists() && !info.isSymLink()) {
                    continue;
                }
                const bool isDirectory = info.isDir() && !info.isSymLink();
                if (isDirectory && !includeFolders && scope != Scope::Selection) {
                    continue;
                }
                rename::Item item;
                item.path = QDir::cleanPath(path);
                item.isDirectory = isDirectory;
                item.modifiedMs = info.lastModified().toMSecsSinceEpoch();
                item.createdMs = info.birthTime().isValid() ? info.birthTime().toMSecsSinceEpoch() : 0;
                items.append(item);
            }
            // En orden natural por carpeta y nombre: el contador sigue ese orden.
            std::sort(items.begin(), items.end(), [](const rename::Item &a, const rename::Item &b) {
                const QString folderA = QFileInfo(a.path).absolutePath();
                const QString folderB = QFileInfo(b.path).absolutePath();
                if (folderA != folderB) {
                    return core::naturalCompare(folderA, folderB) < 0;
                }
                return core::naturalCompare(QFileInfo(a.path).fileName(), QFileInfo(b.path).fileName()) < 0;
            });
            rename::PlanOptions planOptions;
            planOptions.rulesFor = [](const QString &directory) { return ops::nameRulesFor(directory); };
            planOptions.metadataFor = [&cancel](const rename::Item &item) {
                return item.isDirectory ? rename::Metadata() : renameMetadataFor(item.path, cancel);
            };
            const QList<rename::Proposal> proposals = rename::plan(items, options, planOptions);
            if (cancel.isCancelled()) {
                return;
            }
            QMetaObject::invokeMethod(qApp, [self, generation, proposals] {
                if (self) {
                    self->showPreview(generation, proposals);
                }
            }, Qt::QueuedConnection);
        });
}

void BulkRenameDialog::showPreview(quint64 generation, const QList<rename::Proposal> &proposals)
{
    if (generation != m_generation) {
        return;   // ya se pidió otra
    }
    m_shownGeneration = generation;
    m_proposals = proposals;
    m_preview->clear();
    int ok = 0;
    int unchanged = 0;
    int problems = 0;
    const QColor warning = palette().color(QPalette::Link).lighter(100);
    QList<QTreeWidgetItem *> rows;
    for (const rename::Proposal &proposal : proposals) {
        auto *row = new QTreeWidgetItem;
        const QString where = QDir(m_folder).relativeFilePath(QFileInfo(proposal.path).absolutePath());
        const QString prefix = where == QLatin1String(".") ? QString() : where + QLatin1Char('/');
        row->setText(0, prefix + proposal.oldName);
        row->setToolTip(0, QDir::toNativeSeparators(proposal.path));
        row->setText(1, proposal.status == rename::Proposal::Status::Unchanged ? QString() : proposal.newName);
        switch (proposal.status) {
        case rename::Proposal::Status::Unchanged:
            ++unchanged;
            row->setText(2, tr("Sin cambios"));
            row->setForeground(0, palette().color(QPalette::PlaceholderText));
            break;
        case rename::Proposal::Status::Ok:
            ++ok;
            row->setText(2, proposal.problem.isEmpty() ? tr("Bien") : proposal.problem);
            break;
        default:
            ++problems;
            row->setText(2, proposal.problem);
            row->setToolTip(2, proposal.problem);
            row->setForeground(1, QColor(0xC0, 0x39, 0x2B));
            row->setForeground(2, QColor(0xC0, 0x39, 0x2B));
            break;
        }
        Q_UNUSED(warning)
        rows.append(row);
    }
    m_preview->addTopLevelItems(rows);
    QStringList parts{tr("%n se renombrará(n)", nullptr, ok)};
    if (problems > 0) {
        parts.append(tr("%n con problemas (no se tocarán)", nullptr, problems));
    }
    if (unchanged > 0) {
        parts.append(tr("%n sin cambios", nullptr, unchanged));
    }
    m_summary->setText(parts.join(QStringLiteral(" · ")));
    m_renameButton->setEnabled(ok > 0);
    emit previewUpdated();
}

QList<QPair<QString, QString>> BulkRenameDialog::renames() const
{
    QList<QPair<QString, QString>> out;
    for (const rename::Proposal &proposal : m_proposals) {
        if (proposal.willRename()) {
            out.append({proposal.path, proposal.newName});
        }
    }
    return out;
}

} // namespace eu::ui
