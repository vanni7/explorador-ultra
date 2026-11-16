// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/DuplicatesDialog.h"

#include "eu/core/SizeFormat.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace eu::ui {

namespace {

const int GroupRole = Qt::UserRole + 1;
const int PhysicalRole = Qt::UserRole + 2;
const int PathRole = Qt::UserRole + 3;

QString stageText(const dupes::Progress &progress)
{
    const QLocale locale;
    switch (progress.stage) {
    case dupes::Progress::Stage::Listing:
        return DuplicatesDialog::tr("Buscando archivos… %1").arg(locale.toString(progress.filesSeen));
    case dupes::Progress::Stage::BySize:
        return DuplicatesDialog::tr("Agrupando por tamaño…");
    case dupes::Progress::Stage::PartialHash:
        return DuplicatesDialog::tr("Comparando el principio, el medio y el final: %1 de %2")
            .arg(locale.toString(progress.done), locale.toString(progress.candidates));
    case dupes::Progress::Stage::FullHash:
        return DuplicatesDialog::tr("Comparando el contenido completo: %1 de %2 (%3 leídos)")
            .arg(locale.toString(progress.done), locale.toString(progress.candidates),
                 core::formatSize(progress.bytesRead));
    case dupes::Progress::Stage::Bytes:
        return DuplicatesDialog::tr("Comparando byte a byte: %1 de %2")
            .arg(locale.toString(progress.done), locale.toString(progress.candidates));
    case dupes::Progress::Stage::Done:
        break;
    }
    return {};
}

} // namespace

DuplicatesDialog::DuplicatesDialog(tasks::Scheduler &scheduler, QString folder, QString volumeOfFolder,
                                   QStringList allVolumes, QWidget *parent)
    : QDialog(parent), m_scheduler(scheduler), m_folder(std::move(folder)), m_volume(std::move(volumeOfFolder)),
      m_allVolumes(std::move(allVolumes))
{
    setWindowTitle(tr("Buscar duplicados"));
    resize(1000, 680);

    m_scope = new QComboBox(this);
    m_scope->addItem(tr("«%1» y sus subcarpetas").arg(QFileInfo(m_folder).fileName().isEmpty()
                                                         ? QDir::toNativeSeparators(m_folder)
                                                         : QFileInfo(m_folder).fileName()));
    if (!m_volume.isEmpty()) {
        m_scope->addItem(tr("Toda la unidad %1").arg(QDir::toNativeSeparators(m_volume)));
    }
    if (!m_allVolumes.isEmpty()) {
        m_scope->addItem(tr("Todas las unidades (%1)").arg(m_allVolumes.size()));
    }
    m_minSize = new QComboBox(this);
    m_minSize->addItem(tr("Cualquier tamaño"), qint64(1));
    m_minSize->addItem(tr("Desde 100 kB"), qint64(100) * 1000);
    m_minSize->addItem(tr("Desde 1 MB"), qint64(1000) * 1000);
    m_minSize->addItem(tr("Desde 100 MB"), qint64(100) * 1000 * 1000);
    m_bytes = new QCheckBox(tr("Comprobar además byte a byte"), this);
    m_bytes->setToolTip(tr("El SHA-256 ya basta en la práctica; byte a byte lee todo otra vez para no dejar ninguna duda"));
    m_searchButton = new QPushButton(tr("Buscar"), this);
    m_searchButton->setDefault(true);
    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Dónde:"), this));
    top->addWidget(m_scope, 1);
    top->addWidget(m_minSize);
    top->addWidget(m_bytes);
    top->addWidget(m_searchButton);

    m_progress = new QLabel(tr("Pulsa «Buscar». No se borra nada sin que lo marques y lo confirmes."), this);
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Archivo"), tr("Modificado"), tr("Carpeta")});
    m_tree->header()->resizeSection(0, 380);
    m_tree->header()->resizeSection(1, 150);
    m_tree->setAccessibleName(tr("Grupos de archivos iguales"));
    m_tree->setTextElideMode(Qt::ElideMiddle);

    m_rule = new QComboBox(this);
    m_rule->addItem(tr("Todas menos la más antigua"), int(dupes::Keep::Oldest));
    m_rule->addItem(tr("Todas menos la más reciente"), int(dupes::Keep::Newest));
    m_rule->addItem(tr("Todas menos la de ruta más corta"), int(dupes::Keep::ShortestPath));
    m_rule->addItem(tr("Todas menos la que está en…"), int(dupes::Keep::Preferred));
    m_preferred = new QLineEdit(this);
    m_preferred->setPlaceholderText(tr("Carpeta preferida"));
    m_preferred->setEnabled(false);
    auto *browse = new QPushButton(tr("Elegir…"), this);
    browse->setEnabled(false);
    auto *markButton = new QPushButton(tr("Marcar"), this);
    auto *clearButton = new QPushButton(tr("Desmarcar todo"), this);
    auto *rules = new QHBoxLayout;
    rules->addWidget(new QLabel(tr("Marcar en cada grupo:"), this));
    rules->addWidget(m_rule);
    rules->addWidget(m_preferred, 1);
    rules->addWidget(browse);
    rules->addWidget(markButton);
    rules->addWidget(clearButton);

    m_summary = new QLabel(this);
    m_trashButton = new QPushButton(tr("Enviar lo marcado a la papelera…"), this);
    m_trashButton->setEnabled(false);
    auto *closeButton = new QPushButton(tr("Cerrar"), this);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_summary, 1);
    bottom->addWidget(m_trashButton);
    bottom->addWidget(closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(m_progress);
    layout->addWidget(m_tree, 1);
    layout->addLayout(rules);
    layout->addLayout(bottom);

    connect(m_searchButton, &QPushButton::clicked, this, [this] {
        if (m_searching) {
            m_task.cancel();
        } else {
            startSearch();
        }
    });
    connect(m_rule, &QComboBox::currentIndexChanged, this, [this, browse] {
        const bool preferred = dupes::Keep(m_rule->currentData().toInt()) == dupes::Keep::Preferred;
        m_preferred->setEnabled(preferred);
        browse->setEnabled(preferred);
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString chosen = QFileDialog::getExistingDirectory(this, tr("Carpeta cuyas copias se conservan"), m_folder);
        if (!chosen.isEmpty()) {
            m_preferred->setText(chosen);
        }
    });
    connect(markButton, &QPushButton::clicked, this, [this] {
        markWithRule(dupes::Keep(m_rule->currentData().toInt()), m_preferred->text());
    });
    connect(clearButton, &QPushButton::clicked, this, [this] {
        m_updating = true;
        for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
            for (int f = 0; f < m_tree->topLevelItem(g)->childCount(); ++f) {
                m_tree->topLevelItem(g)->child(f)->setCheckState(0, Qt::Unchecked);
            }
        }
        m_updating = false;
        updateSummary();
    });
    connect(m_tree, &QTreeWidget::itemChanged, this, &DuplicatesDialog::onItemChanged);
    connect(m_trashButton, &QPushButton::clicked, this, &DuplicatesDialog::sendToTrash);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    updateSummary();
}

DuplicatesDialog::~DuplicatesDialog()
{
    m_task.cancel();
}

void DuplicatesDialog::startSearch()
{
    dupes::Options options;
    switch (m_scope->currentIndex()) {
    case 0:
        options.roots = {m_folder};
        break;
    case 1:
        options.roots = m_volume.isEmpty() ? m_allVolumes : QStringList{m_volume};
        break;
    default:
        options.roots = m_allVolumes;
        break;
    }
    options.minSize = m_minSize->currentData().toLongLong();
    options.byteByByte = m_bytes->isChecked();
    m_searching = true;
    m_searchButton->setText(tr("Detener"));
    m_tree->clear();
    m_groups.clear();
    updateSummary();
    const QPointer<DuplicatesDialog> self(this);
    const quint64 generation = ++m_generation;
    m_task = m_scheduler.submitForPaths(
        tasks::Category::Indexing, tasks::Priority::UserInteraction, options.roots,
        [self, generation, options](const tasks::CancelToken &cancel) {
            const dupes::Result result = dupes::findDuplicates(options, cancel, [self](const dupes::Progress &progress) {
                QMetaObject::invokeMethod(qApp, [self, progress] {
                    if (self) {
                        self->m_progress->setText(stageText(progress));
                    }
                }, Qt::QueuedConnection);
            });
            const bool stopped = cancel.isCancelled();
            QMetaObject::invokeMethod(qApp, [self, generation, result, stopped] {
                if (self) {
                    if (stopped) {
                        self->m_progress->setText(tr("Búsqueda detenida."));
                    }
                    self->showResult(generation, result);
                }
            }, Qt::QueuedConnection);
        });
}

void DuplicatesDialog::showResult(quint64 generation, const dupes::Result &result)
{
    if (generation != m_generation) {
        return;
    }
    m_searching = false;
    m_searchButton->setText(tr("Buscar"));
    m_groups = result.groups;
    m_updating = true;
    m_tree->clear();
    const QLocale locale;
    for (int g = 0; g < m_groups.size(); ++g) {
        const dupes::Group &group = m_groups[g];
        auto *top = new QTreeWidgetItem(m_tree);
        top->setText(0, tr("%1 copias de %2 · se recuperan %3")
                            .arg(group.physicalCopies)
                            .arg(core::formatSize(group.size), core::formatSize(group.recoverable())));
        top->setFirstColumnSpanned(true);
        QFont font = top->font(0);
        font.setBold(true);
        top->setFont(0, font);
        QHash<int, int> namesPerCopy;
        for (const dupes::File &file : group.files) {
            ++namesPerCopy[file.physical];
        }
        for (const dupes::File &file : group.files) {
            auto *item = new QTreeWidgetItem(top);
            const QFileInfo info(file.path);
            item->setText(0, info.fileName());
            if (namesPerCopy.value(file.physical) > 1) {
                item->setText(0, tr("%1 (enlace físico: el mismo archivo con otro nombre)").arg(info.fileName()));
            }
            item->setText(1, locale.toString(QDateTime::fromMSecsSinceEpoch(file.modifiedMs), QLocale::ShortFormat));
            // Desde la carpeta buscada, si está dentro; si no, la ruta entera.
            const QString base = QDir::cleanPath(m_folder);
            const QString where = info.absolutePath();
            const QString shown = where == base ? tr("(esta carpeta)")
                : where.startsWith(base + QLatin1Char('/')) ? QDir(base).relativeFilePath(where)
                                                            : where;
            item->setText(2, QDir::toNativeSeparators(shown));
            item->setToolTip(2, QDir::toNativeSeparators(file.path));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, Qt::Unchecked);
            item->setData(0, GroupRole, g);
            item->setData(0, PhysicalRole, file.physical);
            item->setData(0, PathRole, file.path);
        }
    }
    m_tree->expandAll();
    m_updating = false;
    if (m_progress->text() != tr("Búsqueda detenida.")) {
        m_progress->setText(result.groups.isEmpty()
                                ? tr("No hay duplicados entre %1 archivos.").arg(locale.toString(result.filesSeen))
                                : tr("%1 archivos revisados: %2 grupos de iguales, %3 recuperables.")
                                      .arg(locale.toString(result.filesSeen))
                                      .arg(locale.toString(result.groups.size()))
                                      .arg(core::formatSize(result.recoverable())));
        if (result.hardlinkOnlySets > 0) {
            m_progress->setText(m_progress->text() + QLatin1Char(' ')
                                + tr("(%n conjunto(s) eran solo enlaces físicos: no ocupan de más)", nullptr,
                                     result.hardlinkOnlySets));
        }
    }
    updateSummary();
    emit searchFinished();
}

void DuplicatesDialog::markWithRule(dupes::Keep keep, const QString &preferredPath)
{
    m_updating = true;
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *top = m_tree->topLevelItem(g);
        const QStringList remove = dupes::proposeRemoval(m_groups.value(top->child(0)->data(0, GroupRole).toInt()),
                                                         keep, preferredPath);
        for (int f = 0; f < top->childCount(); ++f) {
            QTreeWidgetItem *item = top->child(f);
            item->setCheckState(0, remove.contains(item->data(0, PathRole).toString()) ? Qt::Checked : Qt::Unchecked);
        }
    }
    m_updating = false;
    updateSummary();
}

void DuplicatesDialog::onItemChanged(QTreeWidgetItem *item, int column)
{
    if (m_updating || column != 0 || item->parent() == nullptr || item->checkState(0) != Qt::Checked) {
        updateSummary();
        return;
    }
    // Nunca todas las copias reales de un grupo: se desmarca y se avisa.
    QTreeWidgetItem *top = item->parent();
    QSet<int> unmarkedCopies;
    for (int f = 0; f < top->childCount(); ++f) {
        if (top->child(f)->checkState(0) != Qt::Checked) {
            unmarkedCopies.insert(top->child(f)->data(0, PhysicalRole).toInt());
        }
    }
    if (unmarkedCopies.isEmpty()) {
        m_updating = true;
        item->setCheckState(0, Qt::Unchecked);
        m_updating = false;
        m_progress->setText(tr("Se conserva siempre al menos una copia de cada grupo."));
    }
    updateSummary();
}

QStringList DuplicatesDialog::marked() const
{
    QStringList paths;
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *top = m_tree->topLevelItem(g);
        for (int f = 0; f < top->childCount(); ++f) {
            if (top->child(f)->checkState(0) == Qt::Checked) {
                paths.append(top->child(f)->data(0, PathRole).toString());
            }
        }
    }
    return paths;
}

void DuplicatesDialog::updateSummary()
{
    int count = 0;
    qint64 bytes = 0;
    for (int g = 0; g < m_tree->topLevelItemCount(); ++g) {
        QTreeWidgetItem *top = m_tree->topLevelItem(g);
        const qint64 size = m_groups.value(g).size;
        QSet<int> markedCopies;
        QHash<int, int> names;
        QHash<int, int> markedNames;
        for (int f = 0; f < top->childCount(); ++f) {
            const int physical = top->child(f)->data(0, PhysicalRole).toInt();
            ++names[physical];
            if (top->child(f)->checkState(0) == Qt::Checked) {
                ++count;
                ++markedNames[physical];
            }
        }
        // Se libera el espacio de una copia solo si se quitan todos sus nombres.
        for (auto it = markedNames.constBegin(); it != markedNames.constEnd(); ++it) {
            if (it.value() == names.value(it.key())) {
                bytes += size;
            }
        }
    }
    m_summary->setText(count == 0 ? tr("Nada marcado.")
                                  : tr("%n marcado(s): se liberan %1", nullptr, count).arg(core::formatSize(bytes)));
    m_trashButton->setEnabled(count > 0);
}

void DuplicatesDialog::sendToTrash()
{
    const QStringList paths = marked();
    if (paths.isEmpty()) {
        return;
    }
    qint64 bytes = 0;
    for (const QString &path : paths) {
        bytes += QFileInfo(path).size();
    }
    const bool confirmed = m_confirm
        ? m_confirm(int(paths.size()), bytes)
        : QMessageBox::question(this, tr("Enviar a la papelera"),
                                tr("Se enviarán %n archivo(s) (%1) a la papelera propia. Se pueden recuperar desde "
                                   "allí o con Deshacer.", nullptr, int(paths.size()))
                                    .arg(core::formatSize(bytes)),
                                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
              == QMessageBox::Yes;
    if (confirmed) {
        accept();
    }
}

} // namespace eu::ui
