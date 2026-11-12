// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/Sidebar.h"

#include "eu/core/NaturalCompare.h"
#include "eu/core/SizeFormat.h"
#include "eu/fs/DirectoryLister.h"

#include <QApplication>
#include <QDir>
#include <QPointer>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyle>

#include <algorithm>

namespace eu::ui {

namespace {

enum ItemRole {
    PathRole = Qt::UserRole + 1,   ///< Ruta del nodo (vacía en las secciones).
    LoadedRole,                    ///< true si ya se leyeron sus subcarpetas.
    PlaceholderRole,               ///< Hijo ficticio para que aparezca la flecha de desplegar.
};

QIcon themedIcon(const QString &name, QStyle::StandardPixmap fallback)
{
    return QIcon::fromTheme(name, QApplication::style()->standardIcon(fallback));
}

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

QStandardItem *makeSection(const QString &title)
{
    auto *section = new QStandardItem(title);
    section->setFlags(Qt::ItemIsEnabled);   // no seleccionable: es un rótulo
    QFont font = section->font();
    font.setBold(true);
    section->setFont(font);
    return section;
}

} // namespace

Sidebar::Sidebar(tasks::Scheduler &scheduler, fs::VolumeMonitor &volumes, QWidget *parent)
    : QTreeView(parent), m_scheduler(scheduler), m_model(new QStandardItemModel(this))
{
    setModel(m_model);
    setHeaderHidden(true);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setUniformRowHeights(true);
    setAccessibleName(tr("Lugares"));

    m_favoritesSection = makeSection(tr("Favoritos"));
    m_volumesSection = makeSection(tr("Unidades"));
    m_model->appendRow(m_favoritesSection);
    m_model->appendRow(m_volumesSection);

    buildFavorites();
    rebuildVolumes(volumes.volumes());
    expand(m_favoritesSection->index());
    expand(m_volumesSection->index());

    connect(&volumes, &fs::VolumeMonitor::volumesChanged, this, &Sidebar::rebuildVolumes);
    connect(this, &QTreeView::expanded, this, [this](const QModelIndex &index) {
        loadChildren(m_model->itemFromIndex(index));
    });
    // Un solo clic basta para ir a un lugar, como en los exploradores habituales.
    connect(this, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        const QString path = index.data(PathRole).toString();
        if (!path.isEmpty()) {
            emit placeActivated(path);
        }
    });
    connect(this, &QTreeView::activated, this, [this](const QModelIndex &index) {
        const QString path = index.data(PathRole).toString();
        if (!path.isEmpty()) {
            emit placeActivated(path);
        }
    });
}

QStandardItem *Sidebar::makeFolderItem(const QString &text, const QString &path, const QIcon &icon)
{
    auto *item = new QStandardItem(icon, text);
    item->setData(path, PathRole);
    item->setData(false, LoadedRole);
    item->setToolTip(QDir::toNativeSeparators(path));
    // Hijo ficticio: sin él la vista no ofrece desplegar. Se sustituye por las
    // subcarpetas reales la primera vez que se despliega.
    auto *placeholder = new QStandardItem(tr("Cargando…"));
    placeholder->setData(true, PlaceholderRole);
    placeholder->setFlags(Qt::NoItemFlags);
    item->appendRow(placeholder);
    return item;
}

void Sidebar::buildFavorites()
{
    struct Favorite
    {
        QStandardPaths::StandardLocation location;
        QString iconName;
    };
    const QList<Favorite> favorites{
        {QStandardPaths::HomeLocation, QStringLiteral("user-home")},
        {QStandardPaths::DesktopLocation, QStringLiteral("user-desktop")},
        {QStandardPaths::DocumentsLocation, QStringLiteral("folder-documents")},
        {QStandardPaths::DownloadLocation, QStringLiteral("folder-download")},
        {QStandardPaths::PicturesLocation, QStringLiteral("folder-pictures")},
        {QStandardPaths::MusicLocation, QStringLiteral("folder-music")},
        {QStandardPaths::MoviesLocation, QStringLiteral("folder-videos")},
    };

    QStringList seen;
    for (const Favorite &favorite : favorites) {
        const QString path = QStandardPaths::writableLocation(favorite.location);
        // Algunas ubicaciones no existen o coinciden con la carpeta personal.
        if (path.isEmpty() || seen.contains(path)) {
            continue;
        }
        seen.append(path);
        // Se muestra el nombre real de la carpeta ("Escritorio", "Descargas"…),
        // que es el que el usuario ve en el resto del sistema.
        const QString name = (favorite.location == QStandardPaths::HomeLocation)
            ? tr("Carpeta personal")
            : QDir(path).dirName();
        m_favoritesSection->appendRow(
            makeFolderItem(name, path, themedIcon(favorite.iconName, QStyle::SP_DirIcon)));
    }
}

void Sidebar::rebuildVolumes(const QList<fs::Volume> &volumes)
{
    // Los nodos que se van a borrar ya no esperan resultados.
    m_loadingItems.removeIf([this](QStandardItem *item) {
        for (QStandardItem *parent = item; parent != nullptr; parent = parent->parent()) {
            if (parent == m_volumesSection) {
                return true;
            }
        }
        return false;
    });
    m_volumesSection->removeRows(0, m_volumesSection->rowCount());

    for (const fs::Volume &volume : volumes) {
        QStandardItem *item = makeFolderItem(volume.displayName, volume.rootPath, iconForVolume(volume));
        item->setToolTip(tr("%1\n%2 libres de %3 · %4")
                             .arg(QDir::toNativeSeparators(volume.rootPath),
                                  core::formatSize(volume.freeBytes), core::formatSize(volume.totalBytes),
                                  volume.fileSystem));
        m_volumesSection->appendRow(item);
    }
}

void Sidebar::loadChildren(QStandardItem *item)
{
    if (item == nullptr || item->data(LoadedRole).toBool() || m_loadingItems.contains(item)) {
        return;
    }
    const QString path = item->data(PathRole).toString();
    if (path.isEmpty()) {
        return;
    }
    m_loadingItems.append(item);

    // La tarea no toca `this` directamente: el resultado vuelve al hilo de
    // interfaz y allí se comprueba, con el QPointer, que el panel sigue vivo.
    const QPointer<Sidebar> self(this);
    m_scheduler.submit(tasks::Category::Listing, tasks::Priority::VisibleNavigation,
                       [self, path](const tasks::CancelToken &cancel) {
        QStringList folders;
        const core::Status status = fs::listDirectory(path, fs::ListOptions(), cancel,
                                                      [&](std::vector<fs::Entry> &&batch) {
            for (const fs::Entry &entry : batch) {
                if (entry.isDirectory && !entry.isHidden) {
                    folders.append(entry.name);
                }
            }
        });
        std::sort(folders.begin(), folders.end(), [](const QString &a, const QString &b) {
            return core::naturalCompare(a, b) < 0;
        });
        const bool failed = !status.ok();
        QMetaObject::invokeMethod(qApp, [self, path, folders, failed] {
            if (self) {
                self->applyChildren(path, folders, failed);
            }
        }, Qt::QueuedConnection);
    });
}

QStandardItem *Sidebar::findLoadingItem(const QString &path) const
{
    for (QStandardItem *item : m_loadingItems) {
        if (item->data(PathRole).toString() == path) {
            return item;
        }
    }
    return nullptr;
}

void Sidebar::applyChildren(const QString &path, const QStringList &folderNames, bool failed)
{
    QStandardItem *item = findLoadingItem(path);
    if (item == nullptr) {
        return;   // el nodo desapareció mientras se leía (p. ej. se desconectó la unidad)
    }
    m_loadingItems.removeAll(item);
    item->removeRows(0, item->rowCount());

    if (failed) {
        auto *message = new QStandardItem(tr("No se puede leer"));
        message->setFlags(Qt::NoItemFlags);
        item->appendRow(message);
        return;   // LoadedRole queda en false: al volver a desplegar se reintenta
    }
    item->setData(true, LoadedRole);
    const QIcon folderIcon = themedIcon(QStringLiteral("folder"), QStyle::SP_DirIcon);
    for (const QString &name : folderNames) {
        item->appendRow(makeFolderItem(name, QDir(path).filePath(name), folderIcon));
    }
}

} // namespace eu::ui
