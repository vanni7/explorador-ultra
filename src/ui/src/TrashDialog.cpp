// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/TrashDialog.h"

#include "eu/ui/FileActions.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace eu::ui {

namespace {

enum TrashColumn {
    NameColumn = 0,
    OriginColumn,
    DeletedColumn,
    SizeColumn,
    TrashFolderColumn,
    ColumnCount,
};

} // namespace

TrashDialog::TrashDialog(FileActions *actions, QWidget *parent) : QDialog(parent), m_actions(actions)
{
    setWindowTitle(tr("Papelera"));
    resize(900, 480);

    m_list = new QTreeWidget(this);
    m_list->setColumnCount(ColumnCount);
    m_list->setHeaderLabels({tr("Nombre"), tr("Ubicación original"), tr("Eliminado"), tr("Tamaño"), tr("Papelera")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAlternatingRowColors(true);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setAccessibleName(tr("Elementos de la papelera"));
    m_list->header()->resizeSection(NameColumn, 220);
    m_list->header()->resizeSection(OriginColumn, 300);
    m_list->header()->resizeSection(DeletedColumn, 140);
    m_list->header()->resizeSection(SizeColumn, 90);

    m_summary = new QLabel(this);
    m_restoreButton = new QPushButton(tr("Restaurar"), this);
    m_restoreButton->setToolTip(tr("Devuelve lo seleccionado a su ubicación original"));
    m_purgeButton = new QPushButton(tr("Eliminar permanentemente…"), this);
    m_emptyButton = new QPushButton(tr("Vaciar la papelera…"), this);
    auto *reloadButton = new QPushButton(tr("Actualizar"), this);
    auto *closeButton = new QPushButton(tr("Cerrar"), this);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_restoreButton);
    buttons->addWidget(m_purgeButton);
    buttons->addStretch();
    buttons->addWidget(m_emptyButton);
    buttons->addWidget(reloadButton);
    buttons->addWidget(closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_summary);
    layout->addLayout(buttons);

    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &TrashDialog::updateButtons);
    connect(m_restoreButton, &QPushButton::clicked, this, &TrashDialog::restoreSelected);
    connect(m_purgeButton, &QPushButton::clicked, this, &TrashDialog::purgeSelected);
    connect(m_emptyButton, &QPushButton::clicked, this, &TrashDialog::emptyTrash);
    connect(reloadButton, &QPushButton::clicked, this, &TrashDialog::reload);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

    connect(m_actions, &FileActions::trashListed, this, &TrashDialog::showItems);
    // Cualquier operación puede cambiar la papelera (enviar, deshacer,
    // restaurar…): al terminar una, se vuelve a leer.
    connect(m_actions, &FileActions::finished, this, [this] {
        if (isVisible()) {
            reload();
        }
    });
    updateButtons();
}

void TrashDialog::setSizeUnits(core::SizeUnits units)
{
    m_sizeUnits = units;
    if (!m_loading) {
        showItems(m_items);
    }
}

void TrashDialog::reload()
{
    m_loading = true;
    m_summary->setText(tr("Leyendo la papelera…"));
    m_actions->listTrash();
}

void TrashDialog::showItems(const QList<trash::TrashItem> &items)
{
    // Se conserva la selección por identificador entre lecturas.
    QStringList selectedIds;
    for (const trash::TrashItem &item : selectedItems()) {
        selectedIds.append(item.id);
    }

    m_loading = false;
    m_items = items;
    m_list->clear();
    qint64 knownBytes = 0;
    for (qsizetype i = 0; i < items.size(); ++i) {
        const trash::TrashItem &item = items[i];
        auto *row = new QTreeWidgetItem(m_list);
        row->setData(NameColumn, Qt::UserRole, int(i));
        row->setText(NameColumn, item.name());
        row->setText(OriginColumn, QDir::toNativeSeparators(QFileInfo(item.originalPath).path()));
        row->setToolTip(OriginColumn, QDir::toNativeSeparators(item.originalPath));
        row->setText(DeletedColumn,
                     QLocale().toString(QDateTime::fromMSecsSinceEpoch(item.deletedMs), QLocale::ShortFormat));
        // El tamaño del contenido de una carpeta no se guarda al eliminarla.
        row->setText(SizeColumn, item.isDirectory ? tr("Carpeta") : core::formatSize(item.size, m_sizeUnits));
        row->setTextAlignment(SizeColumn, Qt::AlignRight | Qt::AlignVCenter);
        row->setText(TrashFolderColumn, QDir::toNativeSeparators(item.trashDirectory));
        if (selectedIds.contains(item.id)) {
            row->setSelected(true);
        }
        if (!item.isDirectory) {
            knownBytes += item.size;
        }
    }
    m_summary->setText(items.isEmpty()
                           ? tr("La papelera está vacía.")
                           : tr("%n elemento(s) en la papelera", nullptr, int(items.size()))
                                 + tr(" · %1 en archivos (las carpetas no se cuentan)")
                                       .arg(core::formatSize(knownBytes, m_sizeUnits)));
    updateButtons();
}

QList<trash::TrashItem> TrashDialog::selectedItems() const
{
    QList<trash::TrashItem> selected;
    const QList<QTreeWidgetItem *> rows = m_list->selectedItems();
    for (const QTreeWidgetItem *row : rows) {
        const int index = row->data(NameColumn, Qt::UserRole).toInt();
        if (index >= 0 && index < m_items.size()) {
            selected.append(m_items[index]);
        }
    }
    return selected;
}

void TrashDialog::selectOriginalPaths(const QStringList &originalPaths)
{
    m_list->clearSelection();
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *row = m_list->topLevelItem(i);
        const int index = row->data(NameColumn, Qt::UserRole).toInt();
        if (index >= 0 && index < m_items.size() && originalPaths.contains(m_items[index].originalPath)) {
            row->setSelected(true);
        }
    }
}

void TrashDialog::updateButtons()
{
    const bool canModify = m_actions->isAvailable();
    const bool hasSelection = !m_list->selectedItems().isEmpty();
    m_restoreButton->setEnabled(canModify && hasSelection);
    m_purgeButton->setEnabled(canModify && hasSelection);
    m_emptyButton->setEnabled(canModify && !m_items.isEmpty());
}

void TrashDialog::restoreSelected()
{
    const QList<trash::TrashItem> selected = selectedItems();
    if (!selected.isEmpty()) {
        emit restoreRequested(selected);
    }
}

void TrashDialog::purgeSelected()
{
    const QList<trash::TrashItem> selected = selectedItems();
    if (!selected.isEmpty()) {
        emit purgeRequested(selected, false);
    }
}

void TrashDialog::emptyTrash()
{
    if (!m_items.isEmpty()) {
        emit purgeRequested(m_items, true);
    }
}

} // namespace eu::ui
