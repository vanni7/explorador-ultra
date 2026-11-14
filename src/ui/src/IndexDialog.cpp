// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/IndexDialog.h"

#include "eu/ui/VolumesView.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace eu::ui {

namespace {

enum Column { NameColumn, LocationColumn, StateColumn, EntriesColumn, ColumnCount };
const int RootRole = Qt::UserRole + 1;

} // namespace

QString indexStateText(const index::RootStatus &status)
{
    using State = index::RootStatus::State;
    const QString folders = QLocale().toString(status.progress.foldersScanned);
    switch (status.state) {
    case State::Building:
        return IndexDialog::tr("Indexando… %1 carpetas").arg(folders);
    case State::Updating:
        return IndexDialog::tr("Poniéndose al día… %1 carpetas").arg(folders);
    case State::Disconnected:
        return IndexDialog::tr("Unidad no conectada (se conserva el índice)");
    case State::Failed:
        return IndexDialog::tr("Error: %1").arg(status.error);
    case State::Ready:
        break;
    }
    if (status.watching && !status.watchLimited) {
        return IndexDialog::tr("Al día (sigue los cambios)");
    }
    return status.watchLimited ? IndexDialog::tr("Al día (vigilancia parcial: se repasa cada 30 min)")
                               : IndexDialog::tr("Al día (se repasa cada 30 min)");
}

IndexDialog::IndexDialog(index::IndexService &service, VolumeSource volumes, QWidget *parent)
    : QDialog(parent), m_service(service), m_volumes(std::move(volumes))
{
    setWindowTitle(tr("Índice de búsqueda"));
    resize(760, 420);

    auto *intro = new QLabel(tr("Con un índice, buscar por nombre en una unidad entera es casi instantáneo. "
                                "Marca las unidades que quieras indexar. El índice se crea en segundo plano, "
                                "anota nombres, tamaños y fechas (no el contenido de los archivos) y se guarda "
                                "solo en este equipo."), this);
    intro->setWordWrap(true);

    m_list = new QTreeWidget(this);
    m_list->setColumnCount(ColumnCount);
    m_list->setHeaderLabels({tr("Unidad"), tr("Ubicación"), tr("Estado"), tr("Entradas")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAccessibleName(tr("Unidades indexadas"));
    m_list->header()->resizeSection(NameColumn, 170);
    m_list->header()->resizeSection(LocationColumn, 190);
    m_list->header()->setSectionResizeMode(StateColumn, QHeaderView::Stretch);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->resizeSection(EntriesColumn, 90);

    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    m_note->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_note->hide();

    m_refreshButton = new QPushButton(tr("Actualizar ahora"), this);
    m_refreshButton->setToolTip(tr("Vuelve a recorrer la unidad seleccionada (o todas) para poner su índice al día"));
    m_rebuildButton = new QPushButton(tr("Reconstruir"), this);
    m_rebuildButton->setToolTip(tr("Borra el índice de la unidad seleccionada y lo crea de nuevo"));
    m_pauseButton = new QPushButton(this);
    auto *closeButton = new QPushButton(tr("Cerrar"), this);
    closeButton->setDefault(true);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_refreshButton);
    buttons->addWidget(m_rebuildButton);
    buttons->addWidget(m_pauseButton);
    buttons->addStretch(1);
    buttons->addWidget(closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_note);
    layout->addLayout(buttons);

    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_pauseButton, &QPushButton::clicked, this, [this] { m_service.setPaused(!m_service.isPaused()); });
    connect(m_refreshButton, &QPushButton::clicked, this, [this] {
        const QString selected = selectedRoot();
        const QStringList roots = selected.isEmpty() ? m_service.roots() : QStringList{selected};
        for (const QString &root : roots) {
            m_service.refresh(root);
        }
    });
    connect(m_rebuildButton, &QPushButton::clicked, this, [this] {
        const QString selected = selectedRoot();
        if (!selected.isEmpty()) {
            m_service.rebuild(selected);
        }
    });
    connect(m_list, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        if (m_updating || column != NameColumn) {
            return;
        }
        const QString root = item->data(NameColumn, RootRole).toString();
        const bool wanted = item->checkState(NameColumn) == Qt::Checked;
        const bool indexed = m_service.roots().contains(root);
        if (wanted && !indexed) {
            m_service.addRoot(root);
            emit rootsChanged();
        } else if (!wanted && indexed) {
            m_service.removeRoot(root);
            emit rootsChanged();
        }
    });
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &IndexDialog::updateRows);
    connect(&m_service, &index::IndexService::changed, this, &IndexDialog::updateRows);

    rebuild();
}

void IndexDialog::rebuild()
{
    m_updating = true;
    m_list->clear();
    QStringList listed;
    const auto addRow = [&](const QString &root, const QString &name, const QIcon &icon) {
        auto *item = new QTreeWidgetItem(m_list);
        item->setData(NameColumn, RootRole, root);
        item->setText(NameColumn, name);
        item->setIcon(NameColumn, icon);
        item->setText(LocationColumn, QDir::toNativeSeparators(root));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setTextAlignment(EntriesColumn, Qt::AlignRight | Qt::AlignVCenter);
        listed.append(root);
    };
    for (const fs::Volume &volume : m_volumes()) {
        addRow(QDir::cleanPath(volume.rootPath), volume.displayName, iconForVolume(volume));
    }
    // Lo que se indexa aunque ahora no aparezca como unidad (está desconectada).
    for (const QString &root : m_service.roots()) {
        if (!listed.contains(root)) {
            addRow(root, QDir(root).dirName(), QIcon());
        }
    }
    m_updating = false;
    updateRows();
}

void IndexDialog::updateRows()
{
    m_updating = true;
    bool limited = false;
    for (int row = 0; row < m_list->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = m_list->topLevelItem(row);
        const auto status = m_service.statusOf(item->data(NameColumn, RootRole).toString());
        item->setCheckState(NameColumn, status ? Qt::Checked : Qt::Unchecked);
        item->setText(StateColumn, status ? indexStateText(*status) : tr("Sin índice"));
        item->setText(EntriesColumn, (status && status->info.entries > 0)
                                         ? QLocale().toString(status->info.entries) : QString());
        if (status && status->info.updatedMs > 0) {
            item->setToolTip(StateColumn, tr("Último recorrido completo: %1")
                                              .arg(QLocale().toString(
                                                  QDateTime::fromMSecsSinceEpoch(status->info.updatedMs),
                                                  QLocale::ShortFormat)));
        }
        limited = limited || (status && status->watchLimited);
    }
    m_updating = false;

    m_pauseButton->setText(m_service.isPaused() ? tr("Reanudar la indexación") : tr("Pausar la indexación"));
    m_pauseButton->setEnabled(!m_service.roots().isEmpty());
    m_refreshButton->setEnabled(!m_service.roots().isEmpty());
    m_rebuildButton->setEnabled(m_service.roots().contains(selectedRoot()));
    if (limited) {
        // SPEC §6.4: explicar el límite de inotify y cómo ampliarlo.
        m_note->setText(tr("Linux limita cuántas carpetas puede vigilar cada usuario, y aquí hay más de las que "
                           "caben: las que no se vigilan se repasan cada 30 minutos, o al pulsar «Actualizar "
                           "ahora». Para vigilarlas todas, amplía el límite en una terminal:\n"
                           "    sudo sysctl fs.inotify.max_user_watches=1048576\n"
                           "y, para que se conserve al reiniciar:\n"
                           "    echo fs.inotify.max_user_watches=1048576 | sudo tee /etc/sysctl.d/60-inotify.conf\n"
                           "Después reinicia Explorador Ultra."));
    }
    m_note->setVisible(limited);
}

QTreeWidgetItem *IndexDialog::itemFor(const QString &root) const
{
    for (int row = 0; row < m_list->topLevelItemCount(); ++row) {
        if (m_list->topLevelItem(row)->data(NameColumn, RootRole).toString() == root) {
            return m_list->topLevelItem(row);
        }
    }
    return nullptr;
}

QString IndexDialog::selectedRoot() const
{
    const QTreeWidgetItem *item = m_list->currentItem();
    return (item != nullptr && item->isSelected()) ? item->data(NameColumn, RootRole).toString() : QString();
}

void IndexDialog::setIndexed(const QString &root, bool indexed)
{
    if (QTreeWidgetItem *item = itemFor(QDir::cleanPath(root))) {
        item->setCheckState(NameColumn, indexed ? Qt::Checked : Qt::Unchecked);
    }
}

} // namespace eu::ui
