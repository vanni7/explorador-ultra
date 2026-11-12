// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/DirectoryModel.h"

#include "eu/fs/DirectoryLister.h"
#include "eu/ui/FileClipboard.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QLocale>
#include <QMimeDatabase>
#include <QStyle>

#include <chrono>

namespace eu::ui {

namespace {

/// Clave de la caché de tipos para lo que no es un archivo corriente.
const QString folderKey = QStringLiteral("/carpeta");
const QString linkBrokenKey = QStringLiteral("/enlace-roto");

} // namespace

DirectoryModel::DirectoryModel(tasks::Scheduler &scheduler, QObject *parent)
    : QAbstractTableModel(parent), m_scheduler(scheduler)
{
    m_revealTimer.setSingleShot(true);
    m_revealTimer.setInterval(120);
    connect(&m_revealTimer, &QTimer::timeout, this, &DirectoryModel::revealPending);
}

DirectoryModel::~DirectoryModel()
{
    // La tarea captura `this`: hay que asegurarse de que terminó antes de
    // destruir el objeto. La cancelación surte efecto en pocos milisegundos.
    cancelRunningTask();
}

void DirectoryModel::cancelRunningTask()
{
    retireCurrentTask();
    stopMeasuringFolders();
    for (const tasks::TaskHandle &task : m_retiredTasks) {
        task.waitFor(std::chrono::seconds(30));
    }
    m_retiredTasks.clear();
}

/// Cancela la tarea vigente sin esperarla (esperar bloquearía la interfaz) y la
/// apunta para que el destructor sí la espere.
void DirectoryModel::retireCurrentTask()
{
    // De paso se olvidan las que ya terminaron.
    std::erase_if(m_retiredTasks, [](const tasks::TaskHandle &task) { return task.isFinished(); });
    if (m_task.isValid()) {
        m_task.cancel();
        if (!m_task.isFinished()) {
            m_retiredTasks.push_back(m_task);
        }
        m_task = tasks::TaskHandle();
    }
}

void DirectoryModel::setPath(const QString &path)
{
    m_path = QDir::cleanPath(path);
    m_filter = search::Query();   // el filtro era para la carpeta anterior
    // Los tamaños calculados eran de la carpeta anterior.
    stopMeasuringFolders();
    m_folderSizes.clear();
    startLoading(false);
}

void DirectoryModel::refresh()
{
    if (!m_path.isEmpty()) {
        startLoading(m_state == State::Ready);
    }
}

void DirectoryModel::startLoading(bool keepCurrentContent)
{
    // No se espera a la tarea anterior: se cancela y sus resultados se
    // reconocen por la generación y se descartan. Así cambiar de carpeta
    // mientras otra carga nunca bloquea la interfaz.
    retireCurrentTask();
    ++m_generation;
    m_pending.clear();
    m_progressive = false;
    m_keepCurrentContent = keepCurrentContent;

    if (!keepCurrentContent) {
        beginResetModel();
        m_entries.clear();
        m_unfiltered.clear();
        m_hiddenCount = 0;
        endResetModel();
        m_revealTimer.start();
    }
    setState(State::Loading);

    const quint64 generation = m_generation;
    const QString path = m_path;
    const Column sortColumn = m_sortColumn;
    const Qt::SortOrder sortOrder = m_sortOrder;

    m_task = m_scheduler.submit(tasks::Category::Listing, tasks::Priority::VisibleNavigation,
                                [this, generation, path, sortColumn, sortOrder](const tasks::CancelToken &cancel) {
        std::vector<fs::Entry> all;
        const core::Status status = fs::listDirectory(path, fs::ListOptions(), cancel,
                                                      [&](std::vector<fs::Entry> &&batch) {
            all.insert(all.end(), batch.begin(), batch.end());
            QMetaObject::invokeMethod(this, [this, generation, batch = std::move(batch)]() mutable {
                onBatch(generation, std::move(batch));
            }, Qt::QueuedConnection);
        });
        if (cancel.isCancelled()) {
            return;   // nadie espera ya este resultado
        }
        // Se ordena aquí, en segundo plano: con 100 000 nombres tarda ~250 ms,
        // demasiado para el hilo de interfaz.
        sortEntries(all, sortColumn, sortOrder);
        QMetaObject::invokeMethod(this, [this, generation, all = std::move(all), status]() mutable {
            onFinished(generation, std::move(all), status);
        }, Qt::QueuedConnection);
    });
}

void DirectoryModel::onBatch(quint64 generation, std::vector<fs::Entry> batch)
{
    if (generation != m_generation || m_keepCurrentContent) {
        return;
    }
    if (!m_progressive) {
        m_pending.insert(m_pending.end(), std::make_move_iterator(batch.begin()),
                         std::make_move_iterator(batch.end()));
        return;
    }
    int hidden = 0;
    std::vector<fs::Entry> visible = withoutHidden(std::move(batch), m_showHidden, &hidden);
    m_hiddenCount += hidden;
    if (!m_filter.isEmpty()) {
        // Carga progresiva con filtro: se guarda todo y se enseña lo que lo cumple.
        m_unfiltered.insert(m_unfiltered.end(), visible.begin(), visible.end());
        visible = filtered(visible);
    }
    if (visible.empty()) {
        return;
    }
    const int first = int(m_entries.size());
    beginInsertRows(QModelIndex(), first, first + int(visible.size()) - 1);
    m_entries.insert(m_entries.end(), std::make_move_iterator(visible.begin()),
                     std::make_move_iterator(visible.end()));
    endInsertRows();
}

void DirectoryModel::revealPending()
{
    if (m_state != State::Loading || m_keepCurrentContent || m_progressive) {
        return;
    }
    // La carga va lenta: a partir de ahora se muestra lo que vaya llegando.
    m_progressive = true;
    std::vector<fs::Entry> pending;
    pending.swap(m_pending);
    onBatch(m_generation, std::move(pending));
}

void DirectoryModel::onFinished(quint64 generation, std::vector<fs::Entry> sorted, core::Status status)
{
    if (generation != m_generation) {
        return;
    }
    m_revealTimer.stop();
    m_pending.clear();

    if (!status.ok() && sorted.empty()) {
        m_lastError = status.error();
        replaceEntries({});
        setState(State::Failed);
        return;
    }
    // Si hubo un error a mitad (p. ej. se desconectó la unidad) se muestra lo
    // que se alcanzó a leer y el error queda disponible.
    m_lastError = status.ok() ? core::Error() : status.error();

    int hidden = 0;
    std::vector<fs::Entry> visible = withoutHidden(std::move(sorted), m_showHidden, &hidden);
    m_hiddenCount = hidden;
    replaceEntries(std::move(visible));
    setState(State::Ready);
    startMeasuringFolders();
}

void DirectoryModel::setFolderSizesEnabled(bool enabled)
{
    if (m_folderSizesEnabled == enabled) {
        return;
    }
    m_folderSizesEnabled = enabled;
    if (enabled) {
        startMeasuringFolders();
    } else {
        stopMeasuringFolders();
        m_folderSizes.clear();
        if (!m_entries.empty()) {
            emit dataChanged(index(0, int(Column::Size)), index(rowCount() - 1, int(Column::Size)));
        }
    }
}

void DirectoryModel::recalculateFolderSizes()
{
    stopMeasuringFolders();
    m_folderSizes.clear();
    if (!m_entries.empty()) {
        emit dataChanged(index(0, int(Column::Size)), index(rowCount() - 1, int(Column::Size)));
    }
    startMeasuringFolders();
}

const DirectoryModel::FolderSize *DirectoryModel::folderSizeAt(int row) const
{
    const auto it = m_folderSizes.constFind(m_entries[size_t(row)].name);
    return it == m_folderSizes.constEnd() ? nullptr : &it.value();
}

void DirectoryModel::stopMeasuringFolders()
{
    ++m_sizeGeneration;   // lo que llegue de la tarea anterior se descarta
    m_sizeRestartPending = false;
    if (m_sizeTask.isValid()) {
        m_sizeTask.cancel();
        if (!m_sizeTask.isFinished()) {
            m_retiredTasks.push_back(m_sizeTask);   // el destructor la espera
        }
        m_sizeTask = tasks::TaskHandle();
        emit folderSizesChanged();
    }
}

void DirectoryModel::startMeasuringFolders()
{
    if (!m_folderSizesEnabled || m_state != State::Ready) {
        return;
    }
    if (isMeasuringFolders()) {
        // La carpeta se refrescó mientras se medía (pasa a menudo en Descargas).
        // No se empieza de cero, o una carpeta grande no terminaría nunca: se
        // deja acabar y luego se miden las que falten.
        m_sizeRestartPending = true;
        return;
    }
    stopMeasuringFolders();
    // Solo se miden las carpetas que aún no tienen un tamaño definitivo: al
    // refrescarse la vista no se repite el trabajo ya hecho.
    QStringList names;
    for (const fs::Entry &entry : m_entries) {
        if (entry.isDirectory && !entry.isSymlink) {
            const auto known = m_folderSizes.constFind(entry.name);
            if (known == m_folderSizes.constEnd() || !known->finished) {
                names.append(entry.name);
            }
        }
    }
    if (names.isEmpty()) {
        return;
    }
    const quint64 generation = m_sizeGeneration;
    const QString path = m_path;
    // Una sola tarea las mide de una en una: varias a la vez harían ir y
    // venir el cabezal de un disco mecánico y todo iría más lento.
    m_sizeTask = m_scheduler.submit(tasks::Category::Metadata, tasks::Priority::Background,
                                    [this, generation, path, names](const tasks::CancelToken &cancel) {
        using Clock = std::chrono::steady_clock;
        for (const QString &name : names) {
            if (cancel.isCancelled()) {
                return;
            }
            Clock::time_point lastSent = Clock::now();
            const fs::TreeSize size = fs::measureTree(QDir(path).filePath(name), cancel,
                                                      [&](const fs::TreeSize &partial) {
                // El avance se enseña como mucho cinco veces por segundo.
                if (Clock::now() - lastSent < std::chrono::milliseconds(200)) {
                    return;
                }
                lastSent = Clock::now();
                QMetaObject::invokeMethod(this, [this, generation, name, partial] {
                    onFolderSize(generation, name, partial, false);
                }, Qt::QueuedConnection);
            });
            if (size.cancelled) {
                return;
            }
            QMetaObject::invokeMethod(this, [this, generation, name, size] {
                onFolderSize(generation, name, size, true);
            }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(this, [this, generation] {
            if (generation != m_sizeGeneration) {
                return;
            }
            m_sizeTask = tasks::TaskHandle();
            emit folderSizesChanged();
            if (m_sizeRestartPending) {
                m_sizeRestartPending = false;
                startMeasuringFolders();
            }
        }, Qt::QueuedConnection);
    });
    emit folderSizesChanged();
}

void DirectoryModel::onFolderSize(quint64 generation, const QString &name, const fs::TreeSize &size, bool finished)
{
    if (generation != m_sizeGeneration) {
        return;
    }
    FolderSize &known = m_folderSizes[name];
    known.size = size;
    known.finished = finished;
    const int row = rowOfName(name);
    if (row >= 0) {
        emit dataChanged(index(row, int(Column::Size)), index(row, int(Column::Size)));
    }
}

void DirectoryModel::replaceEntries(std::vector<fs::Entry> &&entries)
{
    beginResetModel();
    if (m_filter.isEmpty()) {
        m_entries = std::move(entries);
        m_unfiltered.clear();
    } else {
        m_unfiltered = std::move(entries);
        m_entries = filtered(m_unfiltered);
    }
    endResetModel();
    m_progressive = false;
    m_keepCurrentContent = false;
    emit contentReplaced();
}

std::vector<fs::Entry> DirectoryModel::withoutHidden(std::vector<fs::Entry> entries, bool showHidden,
                                                     int *hiddenCount)
{
    if (showHidden) {
        *hiddenCount = 0;
        return entries;
    }
    const auto firstHidden = std::remove_if(entries.begin(), entries.end(),
                                            [](const fs::Entry &entry) { return entry.isHidden; });
    *hiddenCount = int(entries.end() - firstHidden);
    entries.erase(firstHidden, entries.end());
    return entries;
}

std::vector<fs::Entry> DirectoryModel::filtered(const std::vector<fs::Entry> &entries) const
{
    std::vector<fs::Entry> result;
    for (const fs::Entry &entry : entries) {
        if (m_filter.matches(entry)) {
            result.push_back(entry);
        }
    }
    return result;
}

void DirectoryModel::setFilter(const search::Query &filter)
{
    if (filter.isEmpty() && m_filter.isEmpty()) {
        return;
    }
    // Se parte siempre de la lista completa, que está en un sitio u otro
    // según hubiera filtro o no.
    std::vector<fs::Entry> all = m_filter.isEmpty() ? std::move(m_entries) : std::move(m_unfiltered);
    m_filter = filter;
    replaceEntries(std::move(all));
}

void DirectoryModel::setState(State state)
{
    if (m_state != state) {
        m_state = state;
        emit stateChanged(state);
    }
}

void DirectoryModel::setShowHidden(bool show)
{
    if (m_showHidden != show) {
        m_showHidden = show;
        refresh();
    }
}

void DirectoryModel::setSizeUnits(core::SizeUnits units)
{
    if (m_sizeUnits != units && !m_entries.empty()) {
        m_sizeUnits = units;
        emit dataChanged(index(0, int(Column::Size)), index(rowCount() - 1, int(Column::Size)));
    }
    m_sizeUnits = units;
}

int DirectoryModel::rowOfName(const QString &name) const
{
    for (size_t row = 0; row < m_entries.size(); ++row) {
        if (m_entries[row].name == name) {
            return int(row);
        }
    }
    return -1;
}

QString DirectoryModel::filePath(int row) const
{
    return QDir(m_path).filePath(m_entries[size_t(row)].name);
}

int DirectoryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_entries.size());
}

int DirectoryModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(Column::Count);
}

const DirectoryModel::TypeInfo &DirectoryModel::typeInfoFor(const fs::Entry &entry) const
{
    QString key;
    if (entry.isBrokenSymlink) {
        key = linkBrokenKey;
    } else if (entry.isDirectory) {
        key = folderKey;
    } else {
        key = suffixOf(entry.name);
    }

    auto it = m_typeCache.find(key);
    if (it != m_typeCache.end()) {
        return it.value();
    }

    // Se deduce el tipo solo por el nombre (sin abrir el archivo): no hay E/S.
    // Icono: el del tema del escritorio si lo tiene; si no, el genérico del
    // estilo de Qt, que existe siempre. Así ninguna fila queda sin icono.
    const auto iconFor = [](const QStringList &themeNames, QStyle::StandardPixmap fallback) {
        for (const QString &name : themeNames) {
            if (!name.isEmpty() && QIcon::hasThemeIcon(name)) {
                return QIcon::fromTheme(name);
            }
        }
        return QApplication::style()->standardIcon(fallback);
    };
    TypeInfo info;
    if (key == folderKey) {
        info.description = tr("Carpeta");
        info.icon = iconFor({QStringLiteral("folder"), QStringLiteral("inode-directory")}, QStyle::SP_DirIcon);
    } else if (key == linkBrokenKey) {
        info.description = tr("Enlace roto");
        info.icon = iconFor({QStringLiteral("emblem-unreadable"), QStringLiteral("dialog-warning")},
                            QStyle::SP_MessageBoxWarning);
    } else {
        static const QMimeDatabase mimeDatabase;
        const QMimeType mime = mimeDatabase.mimeTypeForFile(entry.name, QMimeDatabase::MatchExtension);
        info.description = mime.isDefault() ? (key.isEmpty() ? tr("Archivo") : tr("Archivo %1").arg(key.toUpper()))
                                            : mime.comment();
        info.icon = iconFor({mime.iconName(), mime.genericIconName(), QStringLiteral("text-x-generic")},
                            QStyle::SP_FileIcon);
    }
    return m_typeCache.insert(key, info).value();
}

QVariant DirectoryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= int(m_entries.size())) {
        return {};
    }
    const fs::Entry &entry = m_entries[size_t(index.row())];
    const Column column = Column(index.column());

    switch (role) {
    case Qt::DisplayRole:
        switch (column) {
        case Column::Name:
            return entry.name;
        case Column::Size:
            if (entry.isDirectory) {
                // El tamaño de una carpeta solo se conoce si se pidió calcularlo.
                const FolderSize *folder = folderSizeAt(index.row());
                if (folder == nullptr) {
                    return QString();
                }
                QString text = core::formatSize(folder->size.bytes, m_sizeUnits);
                if (!folder->finished) {
                    text += QStringLiteral("…");   // sigue sumando
                } else if (folder->size.incomplete) {
                    text = tr("≥ %1").arg(text);   // hubo subcarpetas ilegibles
                }
                return text;
            }
            if (entry.statFailed) {
                return QString();
            }
            return core::formatSize(entry.size, m_sizeUnits);
        case Column::Type:
            return typeInfoFor(entry).description;
        case Column::Modified:
            if (entry.modifiedMs == 0) {
                return QString();
            }
            return QLocale().toString(QDateTime::fromMSecsSinceEpoch(entry.modifiedMs), QLocale::ShortFormat);
        case Column::Count:
            break;
        }
        break;
    case Qt::DecorationRole:
        if (column == Column::Name) {
            return typeInfoFor(entry).icon;
        }
        break;
    case Qt::TextAlignmentRole:
        if (column == Column::Size) {
            return int(Qt::AlignRight | Qt::AlignVCenter);
        }
        break;
    case Qt::ToolTipRole:
        if (entry.statFailed) {
            return tr("No se pudieron leer los datos de este elemento.");
        }
        if (column == Column::Size && !entry.isDirectory) {
            return tr("%1 bytes").arg(QLocale().toString(entry.size));
        }
        if (column == Column::Size && entry.isDirectory) {
            if (const FolderSize *folder = folderSizeAt(index.row())) {
                QString tip = tr("%1 bytes en %2 archivos y %3 carpetas")
                                  .arg(QLocale().toString(folder->size.bytes), QLocale().toString(folder->size.files),
                                       QLocale().toString(folder->size.folders));
                if (!folder->finished) {
                    tip += tr(" (calculando…)");
                } else if (folder->size.incomplete) {
                    tip += tr("\nHay subcarpetas que no se pudieron leer: el tamaño real es mayor.");
                }
                return tip;
            }
        }
        break;
    case Qt::EditRole:
        if (column == Column::Name) {
            return entry.name;
        }
        break;
    default:
        break;
    }
    return {};
}

QVariant DirectoryModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (Column(section)) {
    case Column::Name:     return tr("Nombre");
    case Column::Size:     return tr("Tamaño");
    case Column::Type:     return tr("Tipo");
    case Column::Modified: return tr("Modificado");
    case Column::Count:    break;
    }
    return {};
}

Qt::ItemFlags DirectoryModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    Qt::ItemFlags result = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    if (index.column() == int(Column::Name)) {
        result |= Qt::ItemIsEditable;   // renombrado en línea (SPEC §8.2)
    }
    return result;
}

QStringList DirectoryModel::mimeTypes() const
{
    return {QStringLiteral("text/uri-list")};
}

QMimeData *DirectoryModel::mimeData(const QModelIndexList &indexes) const
{
    // La vista pasa un índice por celda: cada fila aparece varias veces.
    QStringList paths;
    int lastRow = -1;
    for (const QModelIndex &index : indexes) {
        if (index.isValid() && index.row() != lastRow && index.column() == int(Column::Name)) {
            paths.append(filePath(index.row()));
            lastRow = index.row();
        }
    }
    if (paths.isEmpty()) {
        for (const QModelIndex &index : indexes) {
            const QString path = filePath(index.row());
            if (!paths.contains(path)) {
                paths.append(path);
            }
        }
    }
    return paths.isEmpty() ? nullptr : makeFileMimeData(paths, false);
}

Qt::DropActions DirectoryModel::supportedDragActions() const
{
    return Qt::CopyAction | Qt::MoveAction;
}

bool DirectoryModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || role != Qt::EditRole || index.column() != int(Column::Name)
        || index.row() >= int(m_entries.size())) {
        return false;
    }
    const QString newName = value.toString();
    if (!newName.isEmpty() && newName != m_entries[size_t(index.row())].name) {
        emit renameRequested(filePath(index.row()), newName);
    }
    return false;
}

void DirectoryModel::sort(int column, Qt::SortOrder order)
{
    if (column < 0 || column >= int(Column::Count)) {
        return;
    }
    m_sortColumn = Column(column);
    m_sortOrder = order;

    if (m_state == State::Loading) {
        // La carga en curso se lanzó con el orden anterior: se reinicia.
        startLoading(m_keepCurrentContent);
        return;
    }
    if (m_state != State::Ready) {
        return;
    }
    if (m_entries.size() <= 5000) {
        // Carpetas normales: ordenar aquí cuesta pocos milisegundos.
        std::vector<fs::Entry> sorted = allEntries();
        sortEntries(sorted, m_sortColumn, m_sortOrder);
        replaceEntries(std::move(sorted));
        return;
    }
    // Carpetas enormes: en segundo plano, y lo visible se mantiene mientras tanto.
    retireCurrentTask();
    ++m_generation;
    const quint64 generation = m_generation;
    m_task = m_scheduler.submit(tasks::Category::Listing, tasks::Priority::UserInteraction,
                                [this, generation, entries = allEntries(), sortColumn = m_sortColumn,
                                 sortOrder = m_sortOrder](const tasks::CancelToken &cancel) mutable {
        sortEntries(entries, sortColumn, sortOrder);
        if (cancel.isCancelled()) {
            return;
        }
        QMetaObject::invokeMethod(this, [this, generation, entries = std::move(entries)]() mutable {
            if (generation == m_generation) {
                replaceEntries(std::move(entries));
            }
        }, Qt::QueuedConnection);
    });
}

} // namespace eu::ui
