// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/VolumesView.h"

#include "eu/core/NaturalCompare.h"

#include <QApplication>
#include <QDir>
#include <QHeaderView>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionProgressBar>
#include <QStyledItemDelegate>

#include <algorithm>

namespace eu::ui {

namespace {

enum ItemRole {
    RootRole = Qt::UserRole + 1,   ///< Punto de montaje de la unidad.
    SortRole,                      ///< Número por el que se ordena la columna (bytes o porcentaje).
};

QIcon themedIcon(const QString &name, QStyle::StandardPixmap fallback)
{
    return QIcon::fromTheme(name, QApplication::style()->standardIcon(fallback));
}

QString kindText(const fs::Volume &volume)
{
    QString text;
    switch (volume.kind) {
    case fs::DeviceKind::Ssd:       text = VolumesView::tr("Disco SSD"); break;
    case fs::DeviceKind::Hdd:       text = VolumesView::tr("Disco duro"); break;
    case fs::DeviceKind::Removable: text = VolumesView::tr("Unidad extraíble"); break;
    case fs::DeviceKind::Network:   text = VolumesView::tr("Unidad de red"); break;
    case fs::DeviceKind::Optical:   text = VolumesView::tr("Disco óptico"); break;
    case fs::DeviceKind::Virtual:   text = VolumesView::tr("Unidad virtual"); break;
    case fs::DeviceKind::Unknown:   text = VolumesView::tr("Disco"); break;
    }
    if (!volume.fileSystem.isEmpty()) {
        text += QStringLiteral(" · ") + volume.fileSystem;
    }
    if (volume.readOnly) {
        text += VolumesView::tr(" (solo lectura)");
    }
    return text;
}

/// Porcentaje ocupado, de 0 a 100; -1 si no se conoce el tamaño.
int usedPercent(const fs::Volume &volume)
{
    if (volume.totalBytes <= 0) {
        return -1;
    }
    const qint64 used = qBound<qint64>(0, volume.totalBytes - volume.freeBytes, volume.totalBytes);
    return int((used * 100 + volume.totalBytes / 2) / volume.totalBytes);
}

/// Fila que ordena los tamaños por su valor y los textos con orden natural.
class VolumeItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        const QVariant mine = data(column, SortRole);
        const QVariant theirs = other.data(column, SortRole);
        if (mine.isValid() && theirs.isValid()) {
            return mine.toLongLong() < theirs.toLongLong();
        }
        return core::naturalCompare(text(column), other.text(column)) < 0;
    }
};

/// Dibuja la columna "Uso" como una barra con el porcentaje ocupado.
class UsageDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        // Fondo de la fila (selección, filas alternas) sin texto.
        QStyleOptionViewItem background = option;
        initStyleOption(&background, index);
        background.text.clear();
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &background, painter, widget);

        const int percent = index.data(SortRole).toInt();
        if (percent < 0) {
            return;   // tamaño desconocido: sin barra
        }
        QStyleOptionProgressBar bar;
        bar.rect = option.rect.adjusted(4, 3, -4, -3);
        bar.minimum = 0;
        bar.maximum = 100;
        bar.progress = percent;
        bar.text = index.data(Qt::DisplayRole).toString();
        bar.textVisible = true;
        bar.textAlignment = Qt::AlignCenter;
        bar.state = QStyle::State_Enabled | QStyle::State_Horizontal;
        bar.palette = option.palette;
        style->drawControl(QStyle::CE_ProgressBar, &bar, painter, widget);
    }
};

} // namespace

QIcon iconForVolume(const fs::Volume &volume)
{
    switch (volume.kind) {
    case fs::DeviceKind::Removable:
        return themedIcon(QStringLiteral("drive-removable-media"), QStyle::SP_DriveFDIcon);
    case fs::DeviceKind::Network:
        return themedIcon(QStringLiteral("folder-remote"), QStyle::SP_DriveNetIcon);
    case fs::DeviceKind::Optical:
        return themedIcon(QStringLiteral("media-optical"), QStyle::SP_DriveCDIcon);
    default:
        return themedIcon(QStringLiteral("drive-harddisk"), QStyle::SP_DriveHDIcon);
    }
}

VolumesView::VolumesView(QWidget *parent)
    : QTreeWidget(parent)
{
    setColumnCount(ColumnCount);
    setHeaderLabels({tr("Nombre"), tr("Ubicación"), tr("Tipo"), tr("Tamaño total"), tr("Ocupado"), tr("Libre"),
                     tr("Uso")});
    setRootIsDecorated(false);
    setUniformRowHeights(true);
    setItemsExpandable(false);
    setAllColumnsShowFocus(true);
    setAlternatingRowColors(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setAccessibleName(tr("Unidades"));
    setItemDelegateForColumn(UsageColumn, new UsageDelegate(this));
    setSortingEnabled(true);
    sortByColumn(NameColumn, Qt::AscendingOrder);

    header()->setStretchLastSection(false);
    // El nombre tiene ancho fijo para que nunca quede aplastado; lo que se
    // estira con la ventana es la ubicación.
    header()->resizeSection(NameColumn, 170);
    header()->setSectionResizeMode(LocationColumn, QHeaderView::Stretch);
    header()->resizeSection(TypeColumn, 230);
    header()->resizeSection(TotalColumn, 100);
    header()->resizeSection(UsedColumn, 100);
    header()->resizeSection(FreeColumn, 100);
    header()->resizeSection(UsageColumn, 130);

    connect(this, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        const QString root = item ? item->data(NameColumn, RootRole).toString() : QString();
        if (!root.isEmpty()) {
            emit volumeActivated(root);
        }
    });
}

void VolumesView::setVolumes(const QList<fs::Volume> &volumes)
{
    // Volume::operator== no mira el espacio (así el árbol no se rehace con
    // cada byte escrito); aquí sí importa.
    const bool same = volumes.size() == m_volumes.size()
        && std::equal(volumes.begin(), volumes.end(), m_volumes.begin(), [](const fs::Volume &a, const fs::Volume &b) {
               return a == b && a.totalBytes == b.totalBytes && a.freeBytes == b.freeBytes
                   && a.fileSystem == b.fileSystem && a.kind == b.kind;
           });
    if (same) {
        return;
    }
    m_volumes = volumes;
    rebuild();
}

void VolumesView::setSizeUnits(core::SizeUnits units)
{
    if (m_units != units) {
        m_units = units;
        rebuild();
    }
}

void VolumesView::rebuild()
{
    const QString selected = selectedRoot();
    clear();
    for (const fs::Volume &volume : std::as_const(m_volumes)) {
        auto *item = new VolumeItem(this);
        const QString nativeRoot = QDir::toNativeSeparators(volume.rootPath);
        item->setIcon(NameColumn, iconForVolume(volume));
        item->setText(NameColumn, volume.displayName);
        item->setData(NameColumn, RootRole, volume.rootPath);
        item->setText(LocationColumn, nativeRoot);
        item->setText(TypeColumn, kindText(volume));
        if (!volume.device.isEmpty()) {
            item->setToolTip(TypeColumn, volume.device);
        }

        const int percent = usedPercent(volume);
        if (percent >= 0) {
            const qint64 used = qMax<qint64>(0, volume.totalBytes - volume.freeBytes);
            item->setText(TotalColumn, core::formatSize(volume.totalBytes, m_units));
            item->setText(UsedColumn, core::formatSize(used, m_units));
            item->setText(FreeColumn, core::formatSize(volume.freeBytes, m_units));
            item->setText(UsageColumn, tr("%1 %").arg(percent));
            item->setData(TotalColumn, SortRole, volume.totalBytes);
            item->setData(UsedColumn, SortRole, used);
            item->setData(FreeColumn, SortRole, volume.freeBytes);
            item->setToolTip(NameColumn, tr("%1\n%2 libres de %3")
                                             .arg(nativeRoot, core::formatSize(volume.freeBytes, m_units),
                                                  core::formatSize(volume.totalBytes, m_units)));
        } else {
            item->setToolTip(NameColumn, nativeRoot);
        }
        item->setData(UsageColumn, SortRole, percent);
        for (const int column : {TotalColumn, UsedColumn, FreeColumn}) {
            item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
        }
    }
    selectRoot(selected);
}

void VolumesView::selectRoot(const QString &rootPath)
{
    if (rootPath.isEmpty()) {
        return;
    }
    for (int row = 0; row < topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = topLevelItem(row);
        if (item->data(NameColumn, RootRole).toString() == rootPath) {
            setCurrentItem(item);
            scrollToItem(item);
            return;
        }
    }
}

QString VolumesView::selectedRoot() const
{
    const QTreeWidgetItem *item = currentItem();
    return (item != nullptr && item->isSelected()) ? item->data(NameColumn, RootRole).toString() : QString();
}

} // namespace eu::ui
