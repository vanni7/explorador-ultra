// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/PreferencesDialog.h"

#include "eu/core/Version.h"
#include "eu/ui/Shortcuts.h"

#include <QAction>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace eu::ui {

namespace {

/// Lo que es de este equipo y de esta sesión, no preferencias: ni se exporta
/// ni se pisa al importar o restaurar.
bool isMachineState(const QString &key)
{
    return key.startsWith(QLatin1String("window/")) || key.startsWith(QLatin1String("session/"))
        || key.startsWith(QLatin1String("exportado/"));
}

/// Lo que no se quita al restaurar los valores de fábrica: los índices (son
/// datos, no gustos) y lo de este equipo.
bool survivesFactoryReset(const QString &key)
{
    return isMachineState(key) || key.startsWith(QLatin1String("index/")) || key.startsWith(QLatin1String("contenido/"));
}

QIcon swatch(const QColor &color)
{
    QPixmap pixmap(28, 16);
    pixmap.fill(color);
    QPainter painter(&pixmap);
    painter.setPen(QColor(0, 0, 0, 120));
    painter.drawRect(pixmap.rect().adjusted(0, 0, -1, -1));
    return QIcon(pixmap);
}

} // namespace

PreferencesDialog::PreferencesDialog(QList<QAction *> actions, QWidget *parent)
    : QDialog(parent), m_actions(std::move(actions))
{
    setWindowTitle(tr("Preferencias"));
    resize(720, 560);
    m_appearance = loadAppearance(QSettings());

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(makeAppearancePage(), tr("Apariencia"));
    m_tabs->addTab(makeShortcutsPage(), tr("Atajos de teclado"));
    m_tabs->addTab(makeSettingsPage(), tr("Configuración"));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_tabs, 1);
    layout->addWidget(buttons);
    showAppearance();
    fillShortcuts();
}

// ------------------------------------------------------------- apariencia

QWidget *PreferencesDialog::makeAppearancePage()
{
    auto *page = new QWidget(this);
    m_theme = new QComboBox(page);
    m_theme->addItem(tr("El del sistema"), int(ThemeKind::System));
    m_theme->addItem(tr("Claro"), int(ThemeKind::Light));
    m_theme->addItem(tr("Oscuro"), int(ThemeKind::Dark));
    m_theme->addItem(tr("Personalizado"), int(ThemeKind::Custom));
    connect(m_theme, &QComboBox::currentIndexChanged, this, &PreferencesDialog::appearanceEdited);

    auto *form = new QFormLayout;
    form->addRow(tr("Tema:"), m_theme);
    const QList<QPair<QString, QColor Appearance::*>> colors{
        {tr("Fondo de las listas:"), &Appearance::background},
        {tr("Texto:"), &Appearance::text},
        {tr("Selección:"), &Appearance::selection},
        {tr("Acento:"), &Appearance::accent},
        {tr("Paneles y barras:"), &Appearance::panels},
    };
    for (const auto &[label, member] : colors) {
        auto *button = new QPushButton(page);
        button->setAccessibleName(label);
        const auto memberPointer = member;
        connect(button, &QPushButton::clicked, this, [this, memberPointer, label] {
            const QColor chosen = QColorDialog::getColor(m_appearance.*memberPointer, this, label);
            if (chosen.isValid()) {
                m_appearance.*memberPointer = chosen;
                m_theme->setCurrentIndex(m_theme->findData(int(ThemeKind::Custom)));
                appearanceEdited();
            }
        });
        m_colorButtons.append({button, member});
        form->addRow(label, button);
    }
    auto *colorsNote = new QLabel(tr("Los colores son los del tema «Personalizado»; elegir uno lo activa."), page);
    colorsNote->setEnabled(false);
    form->addRow(QString(), colorsNote);
    m_fontSize = new QSpinBox(page);
    m_fontSize->setRange(0, 32);
    m_fontSize->setSpecialValueText(tr("La del sistema"));
    m_fontSize->setSuffix(tr(" pt"));
    connect(m_fontSize, &QSpinBox::valueChanged, this, &PreferencesDialog::appearanceEdited);
    form->addRow(tr("Tamaño de la letra:"), m_fontSize);
    m_iconSize = new QComboBox(page);
    for (const int size : {16, 20, 24, 32, 48}) {
        m_iconSize->addItem(tr("%1 píxeles").arg(size), size);
    }
    connect(m_iconSize, &QComboBox::currentIndexChanged, this, &PreferencesDialog::appearanceEdited);
    form->addRow(tr("Iconos de las listas:"), m_iconSize);

    auto *note = new QLabel(tr("Los cambios se ven al momento y se conservan al cerrar. El tamaño de las miniaturas "
                               "se elige en Ver → Miniaturas."), page);
    note->setWordWrap(true);
    note->setEnabled(false);
    auto *defaults = new QPushButton(tr("Valores de fábrica"), page);
    connect(defaults, &QPushButton::clicked, this, [this] {
        setAppearance(Appearance());
        appearanceEdited();
    });
    auto *layout = new QVBoxLayout(page);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addStretch(1);
    layout->addWidget(defaults, 0, Qt::AlignLeft);
    return page;
}

void PreferencesDialog::showAppearance()
{
    m_updating = true;
    m_theme->setCurrentIndex(m_theme->findData(int(m_appearance.theme)));
    for (const auto &[button, member] : std::as_const(m_colorButtons)) {
        const QColor color = m_appearance.*member;
        button->setIcon(swatch(color));
        button->setText(color.name());
        button->setEnabled(true);
    }
    m_fontSize->setValue(m_appearance.fontPointSize);
    const int iconIndex = m_iconSize->findData(m_appearance.listIconSize);
    m_iconSize->setCurrentIndex(iconIndex >= 0 ? iconIndex : 0);
    m_updating = false;
}

void PreferencesDialog::setAppearance(const Appearance &appearance)
{
    m_appearance = appearance;
    showAppearance();
}

void PreferencesDialog::chooseAppearance(const Appearance &appearance)
{
    setAppearance(appearance);
    QSettings settings;
    saveAppearance(settings, m_appearance);
    emit appearanceChanged(m_appearance);
}

void PreferencesDialog::appearanceEdited()
{
    if (m_updating) {
        return;
    }
    m_appearance.theme = ThemeKind(m_theme->currentData().toInt());
    m_appearance.fontPointSize = m_fontSize->value();
    m_appearance.listIconSize = m_iconSize->currentData().toInt();
    QSettings settings;
    saveAppearance(settings, m_appearance);
    showAppearance();
    emit appearanceChanged(m_appearance);
}

// ----------------------------------------------------------------- atajos

QWidget *PreferencesDialog::makeShortcutsPage()
{
    auto *page = new QWidget(this);
    m_shortcutFilter = new QLineEdit(page);
    m_shortcutFilter->setPlaceholderText(tr("Buscar una acción o un atajo…"));
    m_shortcutFilter->setClearButtonEnabled(true);
    connect(m_shortcutFilter, &QLineEdit::textChanged, this, &PreferencesDialog::fillShortcuts);
    m_shortcutList = new QTreeWidget(page);
    m_shortcutList->setColumnCount(3);
    m_shortcutList->setHeaderLabels({tr("Acción"), tr("Atajo"), tr("De fábrica")});
    m_shortcutList->setRootIsDecorated(false);
    m_shortcutList->setUniformRowHeights(true);
    m_shortcutList->setAlternatingRowColors(true);
    m_shortcutList->header()->resizeSection(0, 320);
    m_shortcutList->header()->resizeSection(1, 170);
    m_shortcutList->setAccessibleName(tr("Atajos de teclado"));
    connect(m_shortcutList, &QTreeWidget::currentItemChanged, this, &PreferencesDialog::onShortcutSelected);

    m_sequenceEdit = new QKeySequenceEdit(page);
    m_sequenceEdit->setMaximumSequenceLength(1);
    m_sequenceEdit->setAccessibleName(tr("Atajo nuevo"));
    m_assignButton = new QPushButton(tr("Asignar"), page);
    m_clearButton = new QPushButton(tr("Sin atajo"), page);
    m_defaultButton = new QPushButton(tr("El de fábrica"), page);
    connect(m_assignButton, &QPushButton::clicked, this, [this] {
        if (QAction *action = selectedAction()) {
            assignShortcut(action, m_sequenceEdit->keySequence());
        }
    });
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (QAction *action = selectedAction()) {
            action->setShortcuts({});
            QSettings settings;
            saveShortcuts(settings, action);
            m_shortcutMessage->setText(tr("«%1» ya no tiene atajo.").arg(shortcutId(action)));
            fillShortcuts();
        }
    });
    connect(m_defaultButton, &QPushButton::clicked, this, [this] {
        QAction *action = selectedAction();
        if (action == nullptr) {
            return;
        }
        for (const QKeySequence &sequence : defaultShortcuts(action)) {
            if (QAction *other = shortcutConflict(m_actions, sequence, action)) {
                m_shortcutMessage->setText(tr("No se puede: «%1» lo usa ahora «%2».")
                                               .arg(sequence.toString(QKeySequence::NativeText), shortcutId(other)));
                return;
            }
        }
        action->setShortcuts(defaultShortcuts(action));
        QSettings settings;
        saveShortcuts(settings, action);
        m_shortcutMessage->clear();
        fillShortcuts();
    });
    m_shortcutMessage = new QLabel(page);
    m_shortcutMessage->setWordWrap(true);
    auto *allDefaults = new QPushButton(tr("Todos los de fábrica"), page);
    connect(allDefaults, &QPushButton::clicked, this, &PreferencesDialog::restoreDefaultShortcuts);

    auto *editRow = new QHBoxLayout;
    editRow->addWidget(new QLabel(tr("Atajo nuevo:"), page));
    editRow->addWidget(m_sequenceEdit, 1);
    editRow->addWidget(m_assignButton);
    editRow->addWidget(m_clearButton);
    editRow->addWidget(m_defaultButton);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_shortcutFilter);
    layout->addWidget(m_shortcutList, 1);
    layout->addLayout(editRow);
    layout->addWidget(m_shortcutMessage);
    layout->addWidget(allDefaults, 0, Qt::AlignLeft);
    return page;
}

void PreferencesDialog::fillShortcuts()
{
    QAction *selected = selectedAction();
    const QString filter = m_shortcutFilter->text().trimmed();
    m_shortcutList->clear();
    const auto join = [](const QList<QKeySequence> &sequences) {
        QStringList texts;
        for (const QKeySequence &sequence : sequences) {
            texts.append(sequence.toString(QKeySequence::NativeText));
        }
        return texts.join(QStringLiteral(", "));
    };
    for (QAction *action : std::as_const(m_actions)) {
        QString name = action->text();
        name.remove(QLatin1Char('&'));
        const QString menu = action->property("eu-menu").toString();
        const QString label = menu.isEmpty() ? name : menu + QStringLiteral(" → ") + name;
        const QString current = join(action->shortcuts());
        const QString factory = join(defaultShortcuts(action));
        if (!filter.isEmpty() && !label.contains(filter, Qt::CaseInsensitive)
            && !current.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        auto *item = new QTreeWidgetItem(m_shortcutList, {label, current, factory});
        item->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<QObject *>(action)));
        if (current != factory) {
            QFont font = item->font(1);
            font.setBold(true);
            item->setFont(1, font);   // cambiado: en negrita
        }
        if (action == selected) {
            m_shortcutList->setCurrentItem(item);
        }
    }
    onShortcutSelected();
}

QAction *PreferencesDialog::selectedAction() const
{
    const QTreeWidgetItem *item = m_shortcutList->currentItem();
    return item == nullptr ? nullptr : qobject_cast<QAction *>(item->data(0, Qt::UserRole).value<QObject *>());
}

void PreferencesDialog::onShortcutSelected()
{
    QAction *action = selectedAction();
    m_assignButton->setEnabled(action != nullptr);
    m_clearButton->setEnabled(action != nullptr && !action->shortcuts().isEmpty());
    m_defaultButton->setEnabled(action != nullptr && action->shortcuts() != defaultShortcuts(action));
    m_sequenceEdit->setKeySequence(action != nullptr ? action->shortcut() : QKeySequence());
}

bool PreferencesDialog::assignShortcut(QAction *action, const QKeySequence &sequence)
{
    if (sequence.isEmpty()) {
        m_shortcutMessage->setText(tr("Pulsa la combinación en «Atajo nuevo» y después «Asignar»."));
        return false;
    }
    const QString text = sequence.toString(QKeySequence::NativeText);
    if (isReservedBySystem(sequence)) {
        m_shortcutMessage->setText(tr("«%1» lo usa el sistema; elige otra combinación.").arg(text));
        return false;
    }
    if (QAction *other = shortcutConflict(m_actions, sequence, action)) {
        const bool take = m_conflictAnswer
            ? m_conflictAnswer(other)
            : QMessageBox::question(this, windowTitle(),
                                    tr("«%1» ya lo usa «%2». ¿Quitárselo y dárselo a «%3»?")
                                        .arg(text, shortcutId(other), shortcutId(action)),
                                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                == QMessageBox::Yes;
        if (!take) {
            m_shortcutMessage->setText(tr("No se cambió: «%1» sigue siendo de «%2».").arg(text, shortcutId(other)));
            return false;
        }
        QList<QKeySequence> remaining = other->shortcuts();
        remaining.removeAll(sequence);
        other->setShortcuts(remaining);
        QSettings settings;
        saveShortcuts(settings, other);
    }
    action->setShortcuts({sequence});
    QSettings settings;
    saveShortcuts(settings, action);
    m_shortcutMessage->setText(tr("«%1» es ahora el atajo de «%2».").arg(text, shortcutId(action)));
    fillShortcuts();
    return true;
}

void PreferencesDialog::restoreDefaultShortcuts()
{
    QSettings settings;
    settings.remove(QStringLiteral("atajos"));
    applyShortcuts(m_actions, settings);
    m_shortcutMessage->setText(tr("Todos los atajos vuelven a ser los de fábrica."));
    fillShortcuts();
}

// ---------------------------------------------------------- configuración

QWidget *PreferencesDialog::makeSettingsPage()
{
    auto *page = new QWidget(this);
    auto *explanation = new QLabel(
        tr("La configuración (tema, atajos, opciones de las vistas, transferencias, miniaturas, reglas guardadas…) "
           "se puede guardar en un archivo de texto para llevarla a otro equipo o recuperarla después. No incluye la "
           "sesión (pestañas abiertas), el tamaño de la ventana, el historial, la papelera ni las etiquetas."),
        page);
    explanation->setWordWrap(true);
    auto *exportButton = new QPushButton(tr("Exportar configuración…"), page);
    auto *importButton = new QPushButton(tr("Importar configuración…"), page);
    auto *resetButton = new QPushButton(tr("Restaurar toda la configuración de fábrica…"), page);
    connect(exportButton, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(
            this, tr("Exportar configuración"),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                + QStringLiteral("/explorador-ultra.ini"),
            tr("Configuración (*.ini)"));
        if (!file.isEmpty() && exportTo(file)) {
            QMessageBox::information(this, windowTitle(), tr("Configuración guardada en «%1».").arg(QDir::toNativeSeparators(file)));
        }
    });
    connect(importButton, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(
            this, tr("Importar configuración"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
            tr("Configuración (*.ini)"));
        if (!file.isEmpty() && !importFrom(file)) {
            QMessageBox::warning(this, windowTitle(), tr("Ese archivo no es una configuración exportada desde Explorador Ultra."));
        }
    });
    connect(resetButton, &QPushButton::clicked, this, &PreferencesDialog::restoreFactorySettings);
    auto *layout = new QVBoxLayout(page);
    layout->addWidget(explanation);
    layout->addWidget(exportButton, 0, Qt::AlignLeft);
    layout->addWidget(importButton, 0, Qt::AlignLeft);
    layout->addSpacing(16);
    layout->addWidget(resetButton, 0, Qt::AlignLeft);
    layout->addStretch(1);
    return page;
}

bool PreferencesDialog::exportTo(const QString &file)
{
    QFile::remove(file);
    QSettings settings;
    QSettings output(file, QSettings::IniFormat);
    output.setValue(QStringLiteral("exportado/aplicacion"), QStringLiteral("Explorador Ultra"));
    output.setValue(QStringLiteral("exportado/version"), 1);
    output.setValue(QStringLiteral("exportado/programa"), core::versionString());
    output.setValue(QStringLiteral("exportado/fecha"), QDateTime::currentDateTime().toString(Qt::ISODate));
    for (const QString &key : settings.allKeys()) {
        if (!isMachineState(key)) {
            output.setValue(key, settings.value(key));
        }
    }
    output.sync();
    return output.status() == QSettings::NoError;
}

bool PreferencesDialog::importFrom(const QString &file)
{
    QSettings input(file, QSettings::IniFormat);
    if (input.status() != QSettings::NoError
        || input.value(QStringLiteral("exportado/aplicacion")).toString() != QLatin1String("Explorador Ultra")) {
        return false;
    }
    QSettings settings;
    // Lo de antes se sustituye (salvo lo de este equipo): si no, quedarían
    // atajos o reglas que en el archivo ya no están.
    for (const QString &key : settings.allKeys()) {
        if (!isMachineState(key)) {
            settings.remove(key);
        }
    }
    for (const QString &key : input.allKeys()) {
        if (!isMachineState(key)) {
            settings.setValue(key, input.value(key));
        }
    }
    settings.sync();
    applyShortcuts(m_actions, settings);
    m_appearance = loadAppearance(settings);
    showAppearance();
    fillShortcuts();
    emit appearanceChanged(m_appearance);
    emit settingsReplaced();
    return true;
}

bool PreferencesDialog::confirm(const QString &question)
{
    if (m_confirm) {
        return m_confirm();
    }
    return QMessageBox::question(this, windowTitle(), question, QMessageBox::Yes | QMessageBox::Cancel,
                                 QMessageBox::Cancel)
        == QMessageBox::Yes;
}

void PreferencesDialog::restoreFactorySettings()
{
    if (!confirm(tr("¿Volver a la configuración de fábrica? Se pierden el tema, los atajos, las opciones de las "
                    "vistas y las reglas guardadas (no las pestañas, los índices, el historial ni las etiquetas). "
                    "Si quieres, exporta antes la configuración."))) {
        return;
    }
    QSettings settings;
    for (const QString &key : settings.allKeys()) {
        if (!survivesFactoryReset(key)) {
            settings.remove(key);
        }
    }
    settings.sync();
    applyShortcuts(m_actions, settings);
    m_appearance = Appearance();
    showAppearance();
    fillShortcuts();
    emit appearanceChanged(m_appearance);
    emit settingsReplaced();
}

} // namespace eu::ui
