// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/SearchResultsModel.h"

#include "eu/core/NaturalCompare.h"
#include "eu/ui/EntrySort.h"
#include "eu/ui/FileClipboard.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QMimeData>
#include <QPointer>
#include <QSet>

#include <algorithm>

namespace eu::ui {

namespace {

/// Ordena los resultados por una columna; el nombre y la ruta desempatan,
/// para que el orden sea siempre el mismo.
void sortHits(std::vector<search::Hit> &hits, int column, Qt::SortOrder order, const tasks::CancelToken &cancel)
{
    const auto byName = [](const search::Hit &a, const search::Hit &b) {
        const int names = core::naturalCompare(a.entry.name, b.entry.name);
        return names != 0 ? names : core::naturalCompare(a.directory, b.directory);
    };
    const auto compare = [&](const search::Hit &a, const search::Hit &b) {
        int result = 0;
        switch (column) {
        case SearchResultsModel::FolderColumn:
            result = core::naturalCompare(a.directory, b.directory);
            break;
        case SearchResultsModel::SizeColumn:
            // Las carpetas no tienen tamaño propio: van juntas, antes que los archivos.
            if (a.entry.isDirectory != b.entry.isDirectory) {
                result = a.entry.isDirectory ? -1 : 1;
            } else if (!a.entry.isDirectory && a.entry.size != b.entry.size) {
                result = a.entry.size < b.entry.size ? -1 : 1;
            }
            break;
        case SearchResultsModel::TypeColumn: {
            if (a.entry.isDirectory != b.entry.isDirectory) {
                result = a.entry.isDirectory ? -1 : 1;
            } else {
                result = core::naturalCompare(suffixOf(a.entry.name), suffixOf(b.entry.name));
            }
            break;
        }
        case SearchResultsModel::ModifiedColumn:
            if (a.entry.modifiedMs != b.entry.modifiedMs) {
                result = a.entry.modifiedMs < b.entry.modifiedMs ? -1 : 1;
            }
            break;
        default:
            break;
        }
        if (result == 0) {
            result = byName(a, b);
        }
        // Si se cancela, cualquier respuesta coherente vale: el resultado se descarta.
        return cancel.isCancelled() ? false : (order == Qt::AscendingOrder ? result < 0 : result > 0);
    };
    std::stable_sort(hits.begin(), hits.end(), compare);
}

} // namespace

SearchResultsModel::SearchResultsModel(tasks::Scheduler &scheduler, QObject *parent)
    : QAbstractTableModel(parent), m_scheduler(scheduler)
{
}

void SearchResultsModel::setRoot(const QString &root)
{
    m_root = QDir::cleanPath(root);
}

void SearchResultsModel::setFromIndex(bool fromIndex)
{
    if (m_fromIndex == fromIndex) {
        return;
    }
    m_fromIndex = fromIndex;
    if (count() > 0) {
        emit dataChanged(index(0, OriginColumn), index(count() - 1, OriginColumn));
    }
}

void SearchResultsModel::clear()
{
    ++m_generation;
    beginResetModel();
    m_hits.clear();
    m_hits.shrink_to_fit();
    endResetModel();
}

void SearchResultsModel::append(std::vector<search::Hit> &&hits)
{
    if (hits.empty()) {
        return;
    }
    ++m_generation;
    const int first = int(m_hits.size());
    beginInsertRows(QModelIndex(), first, first + int(hits.size()) - 1);
    m_hits.insert(m_hits.end(), std::make_move_iterator(hits.begin()), std::make_move_iterator(hits.end()));
    endInsertRows();
}

void SearchResultsModel::setSizeUnits(core::SizeUnits units)
{
    if (m_units != units) {
        m_units = units;
        if (!m_hits.empty()) {
            emit dataChanged(index(0, SizeColumn), index(count() - 1, SizeColumn), {Qt::DisplayRole});
        }
    }
}

int SearchResultsModel::rowOfPath(const QString &path) const
{
    for (size_t row = 0; row < m_hits.size(); ++row) {
        if (m_hits[row].path() == path) {
            return int(row);
        }
    }
    return -1;
}

int SearchResultsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

int SearchResultsModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(ColumnCount);
}

QString SearchResultsModel::folderText(const search::Hit &hit) const
{
    // Relativa a la carpeta donde se buscó: es más corta y dice lo mismo.
    if (!m_root.isEmpty() && hit.directory == m_root) {
        return tr("(esta carpeta)");
    }
    const QString prefix = m_root.endsWith(QLatin1Char('/')) ? m_root : m_root + QLatin1Char('/');
    if (!m_root.isEmpty() && hit.directory.startsWith(prefix)) {
        return QDir::toNativeSeparators(hit.directory.mid(prefix.size()));
    }
    return QDir::toNativeSeparators(hit.directory);
}

QVariant SearchResultsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= count()) {
        return {};
    }
    const search::Hit &hit = m_hits[size_t(index.row())];
    const fs::Entry &entry = hit.entry;
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case NameColumn:
            return entry.name;
        case FolderColumn:
            return folderText(hit);
        case SizeColumn:
            return (entry.isDirectory || entry.statFailed) ? QString() : core::formatSize(entry.size, m_units);
        case TypeColumn:
            return m_types.infoFor(entry).description;
        case ModifiedColumn:
            return entry.modifiedMs == 0
                ? QString()
                : QLocale().toString(QDateTime::fromMSecsSinceEpoch(entry.modifiedMs), QLocale::ShortFormat);
        case OriginColumn:
            return m_fromIndex ? tr("Índice") : tr("Disco");
        default:
            break;
        }
        break;
    case Qt::DecorationRole:
        if (index.column() == NameColumn) {
            return m_types.infoFor(entry).icon;
        }
        break;
    case Qt::TextAlignmentRole:
        if (index.column() == SizeColumn) {
            return int(Qt::AlignRight | Qt::AlignVCenter);
        }
        break;
    case Qt::ToolTipRole:
        if (index.column() == SizeColumn && !entry.isDirectory) {
            return tr("%1 bytes").arg(QLocale().toString(entry.size));
        }
        if (index.column() == OriginColumn) {
            return m_fromIndex ? tr("Del índice: no se ha comprobado que siga en el disco")
                               : tr("Encontrado recorriendo el disco");
        }
        return QDir::toNativeSeparators(hit.path());
    default:
        break;
    }
    return {};
}

QVariant SearchResultsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case NameColumn:     return tr("Nombre");
    case FolderColumn:   return tr("Carpeta");
    case SizeColumn:     return tr("Tamaño");
    case TypeColumn:     return tr("Tipo");
    case ModifiedColumn: return tr("Modificado");
    case OriginColumn:   return tr("Origen");
    default:             return {};
    }
}

Qt::ItemFlags SearchResultsModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
}

QStringList SearchResultsModel::mimeTypes() const
{
    return {QStringLiteral("text/uri-list")};
}

QMimeData *SearchResultsModel::mimeData(const QModelIndexList &indexes) const
{
    QStringList paths;
    for (const QModelIndex &index : indexes) {
        if (index.column() == NameColumn && index.row() < count()) {
            paths.append(filePath(index.row()));
        }
    }
    return paths.isEmpty() ? nullptr : makeFileMimeData(paths, false);
}

Qt::DropActions SearchResultsModel::supportedDragActions() const
{
    return Qt::CopyAction | Qt::MoveAction;
}

void SearchResultsModel::sort(int column, Qt::SortOrder order)
{
    m_sortColumn = column;
    m_sortOrder = order;
    resort();
}

void SearchResultsModel::resort()
{
    if (m_sortColumn < 0 || m_hits.size() < 2) {
        return;
    }
    const QPointer<SearchResultsModel> self(this);
    m_scheduler.submit(tasks::Category::Listing, tasks::Priority::UserInteraction,
                       [self, hits = m_hits, generation = m_generation, column = m_sortColumn,
                        order = m_sortOrder](const tasks::CancelToken &cancel) mutable {
        sortHits(hits, column, order, cancel);
        if (cancel.isCancelled()) {
            return;
        }
        QMetaObject::invokeMethod(qApp, [self, hits = std::move(hits), generation, column, order]() mutable {
            if (!self || self->m_generation != generation || self->m_sortColumn != column
                || self->m_sortOrder != order) {
                return;   // llegaron más resultados o se pidió otro orden: este ya no vale
            }
            self->beginResetModel();
            self->m_hits = std::move(hits);
            self->endResetModel();
            emit self->contentReplaced();
        }, Qt::QueuedConnection);
    });
}

void SearchResultsModel::pruneMissing()
{
    if (m_hits.empty()) {
        return;
    }
    QStringList paths;
    paths.reserve(qsizetype(m_hits.size()));
    for (const search::Hit &hit : m_hits) {
        paths.append(hit.path());
    }
    const QPointer<SearchResultsModel> self(this);
    m_scheduler.submit(tasks::Category::Listing, tasks::Priority::Background,
                       [self, paths, generation = m_generation](const tasks::CancelToken &cancel) {
        QSet<QString> missing;
        for (const QString &path : paths) {
            if (cancel.isCancelled()) {
                return;
            }
            // Un enlace roto existe como enlace aunque su destino no.
            const QFileInfo info(path);
            if (!info.exists() && !info.isSymLink()) {
                missing.insert(path);
            }
        }
        if (missing.isEmpty()) {
            return;
        }
        QMetaObject::invokeMethod(qApp, [self, missing, generation] {
            if (!self || self->m_generation != generation) {
                return;
            }
            ++self->m_generation;
            self->beginResetModel();
            std::erase_if(self->m_hits, [&](const search::Hit &hit) { return missing.contains(hit.path()); });
            self->endResetModel();
            emit self->contentReplaced();
        }, Qt::QueuedConnection);
    });
}

} // namespace eu::ui
