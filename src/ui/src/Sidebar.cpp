// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/Sidebar.h"

#include "eu/core/NaturalCompare.h"
#include "eu/core/SizeFormat.h"
#include "eu/fs/DirectoryLister.h"
#include "eu/ui/Locations.h"
#include "eu/ui/VolumesView.h"

#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QPainter>
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
    m_volumesSection->setToolTip(tr("Ver todas las unidades con su tamaño y su espacio libre"));
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
    // Un solo clic va al lugar y despliega sus subcarpetas; otro clic sobre
    // la misma carpeta las recoge. El doble clic no hace nada distinto (si lo
    // hiciera, desplegaría y recogería seguido).
    setExpandsOnDoubleClick(false);
    connect(this, &QTreeView::clicked, this, &Sidebar::onClicked);
    // Con el teclado: Intro va al lugar; las flechas despliegan y recogen.
    connect(this, &QTreeView::activated, this, [this](const QModelIndex &index) {
        const QString path = index == m_volumesSection->index() ? volumesLocation()
                                                                 : index.data(PathRole).toString();
        if (!path.isEmpty() && !(QApplication::mouseButtons() & Qt::LeftButton)) {
            emit placeActivated(path);
        }
    });

    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        const QString path = pathAt(position);
        if (!path.isEmpty()) {
            emit contextMenuRequested(path, viewport()->mapToGlobal(position));
        }
    });

    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DropOnly);
    setDropIndicatorShown(false);

    // Al recorrer el árbol con las flechas, la carpeta se abre tras una pausa
    // corta: así se puede pasar deprisa por encima de muchas sin leerlas todas.
    m_followTimer.setSingleShot(true);
    m_followTimer.setInterval(300);
    connect(&m_followTimer, &QTimer::timeout, this, [this] {
        const QString path = currentPlace();
        if (!path.isEmpty() && path != m_currentPath) {
            emit placeActivated(path);
        }
    });
}

void Sidebar::addPlace(const QString &name, const QString &path)
{
    m_favoritesSection->appendRow(
        makeFolderItem(name, QDir::cleanPath(path), themedIcon(QStringLiteral("folder"), QStyle::SP_DirIcon)));
}

QString Sidebar::currentPlace() const
{
    const QModelIndex index = currentIndex();
    return index == m_volumesSection->index() ? volumesLocation() : index.data(PathRole).toString();
}

void Sidebar::keyPressEvent(QKeyEvent *event)
{
    const QModelIndex before = currentIndex();
    const int key = event->key();
    const bool plain = (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
    if (plain && before.isValid() && (key == Qt::Key_Left || key == Qt::Key_Right)) {
        // Derecha: despliega la rama; si ya lo está, baja a la primera
        // subcarpeta. Izquierda: recoge la rama; si ya lo está, sube a la
        // carpeta padre. No depende del estilo del sistema.
        if (key == Qt::Key_Right) {
            if (m_model->hasChildren(before) && !isExpanded(before)) {
                expand(before);
            } else {
                const QModelIndex child = m_model->index(0, 0, before);
                if (child.isValid() && (child.flags() & Qt::ItemIsEnabled)) {
                    setCurrentIndex(child);
                }
            }
        } else if (m_model->hasChildren(before) && isExpanded(before)) {
            collapse(before);
        } else if (before.parent().isValid()) {
            setCurrentIndex(before.parent());
        }
        event->accept();
    } else {
        QTreeView::keyPressEvent(event);
    }
    if (currentIndex() != before) {
        scrollTo(currentIndex());
        m_followTimer.start();
    }
}

void Sidebar::onClicked(const QModelIndex &index)
{
    // El rótulo "Unidades" abre la lista de unidades con su espacio.
    const bool isVolumesSection = index == m_volumesSection->index();
    const QString path = isVolumesSection ? volumesLocation() : index.data(PathRole).toString();
    if (path.isEmpty()) {
        return;
    }
    // Un clic en la flecha de desplegar ya lo atiende la vista: solo despliega
    // o recoge, sin ir a la carpeta.
    const QPoint position = viewport()->mapFromGlobal(QCursor::pos());
    const bool onArrow = viewport()->rect().contains(position) && position.x() < visualRect(index).left();
    if (onArrow) {
        return;
    }
    // Si ya se está viendo esa carpeta, el clic solo cambia desplegado/recogido.
    if (isVolumesSection) {
        setExpanded(index, true);
        emit placeActivated(path);
    } else if (path == m_currentPath) {
        setExpanded(index, !isExpanded(index));
    } else {
        setExpanded(index, true);
        emit placeActivated(path);
    }
}

QString Sidebar::pathAt(const QPoint &position) const
{
    return indexAt(position).data(PathRole).toString();
}

QPoint Sidebar::positionOf(const QString &path) const
{
    const QModelIndexList found = m_model->match(m_model->index(0, 0), PathRole, path, 1,
                                                 Qt::MatchExactly | Qt::MatchRecursive);
    return found.isEmpty() ? QPoint() : visualRect(found.first()).center();
}

DropPlan Sidebar::planFor(const QDropEvent *event) const
{
    const QString target = pathAt(event->position().toPoint());
    if (target.isEmpty()) {
        return {};   // sobre un rótulo de sección o el espacio vacío
    }
    return planDrop(event->mimeData(), target, event->modifiers(), m_moveByDefault);
}

void Sidebar::setDropTarget(const QModelIndex &index)
{
    if (m_dropIndex != index) {
        m_dropIndex = index;
        viewport()->update();
    }
}

void Sidebar::dragEnterEvent(QDragEnterEvent *event)
{
    if (fileSelectionFrom(event->mimeData()).isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
}

void Sidebar::dragMoveEvent(QDragMoveEvent *event)
{
    const DropPlan plan = planFor(event);
    setDropTarget(plan.valid ? indexAt(event->position().toPoint()) : QModelIndex());
    if (!plan.valid) {
        event->ignore();
        return;
    }
    event->setDropAction(plan.move ? Qt::MoveAction : Qt::CopyAction);
    event->accept();
}

void Sidebar::dragLeaveEvent(QDragLeaveEvent *event)
{
    setDropTarget(QModelIndex());
    event->accept();
}

void Sidebar::dropEvent(QDropEvent *event)
{
    const DropPlan plan = planFor(event);
    setDropTarget(QModelIndex());
    if (!plan.valid) {
        event->ignore();
        return;
    }
    // Igual que en la lista de archivos: al origen se le contesta "copiado"
    // para que nunca borre el original por su cuenta.
    event->setDropAction(Qt::CopyAction);
    event->accept();
    emit filesDropped(plan.paths, plan.target, plan.move);
}

void Sidebar::paintEvent(QPaintEvent *event)
{
    QTreeView::paintEvent(event);
    if (!m_dropIndex.isValid()) {
        return;
    }
    QRect frame = visualRect(m_dropIndex);
    frame.setRight(viewport()->width() - 1);
    QPainter painter(viewport());
    painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
    painter.drawRect(frame.adjusted(1, 1, -2, -2));
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
