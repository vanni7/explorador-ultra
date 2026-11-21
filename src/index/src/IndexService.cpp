// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/IndexService.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QPointer>

#include <chrono>

namespace eu::index {

namespace {

/// Propiedad del índice con hasta dónde está al día según el diario de cambios.
QString journalProperty()
{
    return QStringLiteral("diario");
}

/// Con más carpetas cambiadas que estas, sale más a cuenta recorrerlo todo.
constexpr int maxChangedFolders = 5000;

/// true si `folder` está dentro de una carpeta que no se indexa (la papelera).
bool insideExcluded(const QString &folder, const QString &root, const QStringList &excluded)
{
    const QString relative = QDir(root).relativeFilePath(folder);
    const QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (excluded.contains(part, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

} // namespace

/// Lo que se guarda de cada carpeta indexada.
struct IndexService::Root
{
    explicit Root(Index openedIndex) : index(std::move(openedIndex)) {}

    Index index;
    std::unique_ptr<TreeWatcher> watcher;
    RootStatus status;
    QSet<QString> dirty;        ///< Carpetas de las que llegó un aviso.
    bool wholePending = false;  ///< Hay que recorrerlo entero.
    /// Hay que ponerlo al día con el diario de cambios (al arrancar, o si se
    /// perdieron avisos); si el diario no sirve, se recorre entero.
    bool catchUpPending = false;
    QString volume;             ///< Unidad con diario de cambios ("C:"), o vacío.
    JournalPosition previousStart;  ///< Dónde estaba el diario al empezar la tarea anterior.
    bool running = false;
    tasks::TaskHandle task;
    quint64 generation = 0;
};

IndexService::IndexService(tasks::Scheduler &scheduler, QString dataDirectory, QObject *parent)
    : QObject(parent), m_scheduler(scheduler), m_dataDirectory(std::move(dataDirectory)),
      m_gate(std::make_shared<search::PauseGate>())
{
    // Una copia de mil archivos da mil avisos: se espera a que amainen.
    m_startTimer.setSingleShot(true);
    m_startTimer.setInterval(500);
    connect(&m_startTimer, &QTimer::timeout, this, &IndexService::startNext);

    // Donde el sistema no avisa de todo, el índice se repasa entero cada rato.
    m_reviewTimer.setInterval(30 * 60 * 1000);
    connect(&m_reviewTimer, &QTimer::timeout, this, [this] {
        for (auto &[path, root] : m_roots) {
            if (!root->watcher->isActive() || root->watcher->isLimited()) {
                root->wholePending = true;
            }
        }
        schedule();
    });
    m_reviewTimer.start();
}

IndexService::~IndexService()
{
    m_gate->setPaused(false);
    for (auto &[path, root] : m_roots) {
        stopTask(*root);
    }
}

QString IndexService::databasePathFor(const QString &root) const
{
    const QByteArray hash = QCryptographicHash::hash(QDir::cleanPath(root).toUtf8(), QCryptographicHash::Sha1);
    return m_dataDirectory + QLatin1Char('/') + QString::fromLatin1(hash.toHex().left(16)) + QStringLiteral(".db");
}

QStringList IndexService::roots() const
{
    QStringList paths;
    for (const auto &[path, root] : m_roots) {
        paths.append(path);
    }
    paths += m_failures.keys();
    return paths;
}

QList<RootStatus> IndexService::status() const
{
    QList<RootStatus> all;
    for (const auto &[path, root] : m_roots) {
        RootStatus status = root->status;
        status.watching = root->watcher && root->watcher->isActive();
        status.watchLimited = root->watcher && root->watcher->isLimited();
        status.watchedFolders = root->watcher ? root->watcher->watchedFolders() : 0;
        all.append(status);
    }
    for (auto failure = m_failures.constBegin(); failure != m_failures.constEnd(); ++failure) {
        RootStatus status;
        status.root = failure.key();
        status.state = RootStatus::State::Failed;
        status.error = failure.value();
        all.append(status);
    }
    return all;
}

std::optional<RootStatus> IndexService::statusOf(const QString &root) const
{
    const QString clean = QDir::cleanPath(root);
    for (const RootStatus &status : this->status()) {
        if (status.root == clean) {
            return status;
        }
    }
    return std::nullopt;
}

void IndexService::addRoot(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (clean.isEmpty() || m_roots.count(clean) != 0) {
        return;
    }
    m_failures.remove(clean);
    const core::Result<Index> opened = Index::open(databasePathFor(clean), clean);
    if (!opened.ok()) {
        // Queda en la lista, con su error, para que se vea y se pueda reintentar.
        m_failures.insert(clean, opened.error().message);
        emit changed();
        return;
    }
    auto root = std::make_unique<Root>(opened.value());
    root->status.root = clean;
    root->status.info = root->index.info();
    root->status.state = root->status.info.complete ? RootStatus::State::Updating : RootStatus::State::Building;
    // Construirlo, o ponerlo al día de lo que cambió con la aplicación
    // cerrada: si la unidad tiene diario de cambios y se sabe hasta dónde
    // llegó la última vez, se le pregunta en vez de recorrerla entera.
    root->volume = m_journal ? m_journal->volumeOf(clean) : QString();
    if (root->status.info.complete && !root->volume.isEmpty()
        && JournalPosition::fromString(root->index.property(journalProperty())).isValid()) {
        root->catchUpPending = true;
    } else {
        root->wholePending = true;
    }
    root->watcher = TreeWatcher::create(clean);
    Root *raw = root.get();
    connect(root->watcher.get(), &TreeWatcher::folderChanged, this, [this, raw](const QString &folder) {
        raw->dirty.insert(folder);
        schedule();
    });
    connect(root->watcher.get(), &TreeWatcher::overflowed, this, [this, raw] {
        // Se perdieron avisos: el diario sabe cuáles (si lo hay); si no, se recorre.
        if (raw->volume.isEmpty()) {
            raw->wholePending = true;
        } else {
            raw->catchUpPending = true;
        }
        schedule();
    });
    connect(root->watcher.get(), &TreeWatcher::limitReached, this, &IndexService::changed);
    m_roots.emplace(clean, std::move(root));
    emit changed();
    schedule();
}

void IndexService::stopTask(Root &root)
{
    if (root.running) {
        root.task.cancel();
        // Se espera: la tarea usa el vigilante de esta carpeta. Comprueba la
        // cancelación en cada carpeta, así que responde enseguida.
        root.task.waitFor(std::chrono::seconds(60));
        root.running = false;
        m_running = false;
    }
    ++root.generation;   // lo que la tarea hubiera dejado en camino se descarta
}

void IndexService::removeRoot(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (m_failures.remove(clean) > 0) {
        Index::remove(databasePathFor(clean));
        emit changed();
        return;
    }
    const auto found = m_roots.find(clean);
    if (found == m_roots.end()) {
        return;
    }
    stopTask(*found->second);
    const QString database = found->second->index.databasePath();
    m_roots.erase(found);
    Index::remove(database);
    emit changed();
    schedule();
}

void IndexService::refresh(const QString &path)
{
    const auto found = m_roots.find(QDir::cleanPath(path));
    if (found == m_roots.end()) {
        return;
    }
    found->second->wholePending = true;
    schedule();
}

void IndexService::rebuild(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (m_roots.count(clean) == 0 && !m_failures.contains(clean)) {
        return;
    }
    removeRoot(clean);
    addRoot(clean);
}

void IndexService::setPaused(bool paused)
{
    if (m_paused == paused) {
        return;
    }
    m_paused = paused;
    m_gate->setPaused(paused);
    emit changed();
    if (!paused) {
        schedule();
    }
}

bool IndexService::isBusy() const
{
    for (const auto &[path, root] : m_roots) {
        if (root->running || root->wholePending || root->catchUpPending || !root->dirty.isEmpty()) {
            return true;
        }
    }
    return false;
}

std::optional<Index> IndexService::indexFor(const QString &path) const
{
    const Root *best = nullptr;
    for (const auto &[rootPath, root] : m_roots) {
        if (root->status.info.complete && root->status.state != RootStatus::State::Failed
            && root->index.covers(path) && (best == nullptr || rootPath.size() > best->status.root.size())) {
            best = root.get();
        }
    }
    return best != nullptr ? std::optional<Index>(best->index) : std::nullopt;
}

void IndexService::schedule()
{
    if (!m_startTimer.isActive()) {
        m_startTimer.start();
    }
}

void IndexService::startNext()
{
    if (m_running || m_paused) {
        return;   // al terminar la tarea en curso, o al reanudar, se vuelve aquí
    }
    // Primero los recorridos completos (hay un índice sin construir); luego los avisos.
    Root *next = nullptr;
    for (auto &[path, root] : m_roots) {
        if (root->status.state == RootStatus::State::Failed) {
            continue;
        }
        if (root->wholePending || root->catchUpPending) {
            next = root.get();
            break;
        }
        if (next == nullptr && !root->dirty.isEmpty()) {
            next = root.get();
        }
    }
    if (next != nullptr) {
        startTask(*next);
    }
}

void IndexService::startTask(Root &root)
{
    // Con muchísimas carpetas cambiadas sale más a cuenta recorrerlo todo.
    const bool whole = root.wholePending || root.dirty.size() > maxChangedFolders;
    const bool catchUp = !whole && root.catchUpPending;
    QStringList folders;
    if (!whole) {
        folders = QStringList(root.dirty.constBegin(), root.dirty.constEnd());
    }
    root.dirty.clear();
    root.wholePending = false;
    root.catchUpPending = false;
    root.running = true;
    m_running = true;
    if (whole || catchUp) {
        root.status.state = root.status.info.complete ? RootStatus::State::Updating : RootStatus::State::Building;
        root.status.progress = {};
        root.status.error.clear();
        emit changed();
    }

    const QPointer<IndexService> self(this);
    const quint64 generation = root.generation;
    const QString path = root.status.root;
    TreeWatcher *watcher = root.watcher.get();   // vive hasta que la tarea termina (ver stopTask)
    // El diario de cambios, si la carpeta está en una unidad que lo tiene.
    const std::shared_ptr<ChangeJournal> journal = root.volume.isEmpty() ? nullptr : m_journal;
    const QString volume = root.volume;
    const bool building = !root.status.info.complete;
    // Los avisos del sistema solo dejan el índice "al día" si vigila el árbol entero.
    const bool watcherCoversAll = watcher->isActive() && !watcher->isLimited();
    // Si vigila carpeta a carpeta (Linux), al ponerse al día sin recorrer
    // hay que pedirle las que ya están anotadas.
    const bool watcherNeedsFolders = watcher->isActive() && !watcher->watchesWholeTree();
    const JournalPosition previousStart = root.previousStart;
    // De fondo y sobre su unidad: en un disco lento espera a que no haya
    // nadie más trabajando en él.
    root.task = m_scheduler.submitForPaths(
        tasks::Category::Indexing, tasks::Priority::Background, {path},
        [self, generation, path, whole, catchUp, building, folders, watcher, journal, volume, watcherCoversAll,
         watcherNeedsFolders, previousStart, index = root.index, gate = m_gate, excluded = m_excludedNames](const tasks::CancelToken &cancel) {
            // Dónde está el diario ahora, antes de leer nada: lo que cambie
            // después se verá la próxima vez.
            JournalPosition start;
            if (journal) {
                const core::Result<JournalPosition> position = journal->position(volume);
                if (position.ok()) {
                    start = position.value();
                }
            }

            bool wholeTree = whole;
            QStringList targets = whole ? QStringList{path} : folders;
            qint64 journalFolders = -1;
            if (catchUp) {
                // Solo las carpetas que cambiaron desde la última vez.
                const JournalPosition since = JournalPosition::fromString(index.property(journalProperty()));
                const core::Result<JournalChanges> changes =
                    journal != nullptr && since.isValid()
                        ? journal->changesSince(volume, since, maxChangedFolders)
                        : core::Result<JournalChanges>(core::Error{core::ErrorCode::NotFound,
                                                                   QStringLiteral("sin diario de cambios"), path, 0});
                if (changes.ok()) {
                    targets.clear();
                    for (const QString &folder : changes.value().folders) {
                        if (index.covers(folder) && !insideExcluded(folder, path, excluded)) {
                            targets.append(folder);
                        }
                    }
                    journalFolders = targets.size();
                    if (watcherNeedsFolders) {
                        index.forEachFolder([watcher](const QString &folder) { watcher->addFolder(folder); }, cancel);
                    }
                } else {
                    qInfo("Índice de %s: el diario de cambios no sirve (%s); se recorre entero", qPrintable(path),
                          qPrintable(changes.error().message));
                    wholeTree = true;
                    targets = QStringList{path};
                }
            }

            // La primera vez que se indexa una unidad entera, su tabla de
            // archivos dice cuántas entradas esperar (para el avance).
            if (wholeTree && building && journal && QDir::cleanPath(path).size() == 3) {
                const core::Result<VolumeCounts> counted = journal->counts(volume);
                if (counted.ok()) {
                    const qint64 entries = counted.value().files + counted.value().folders;
                    QMetaObject::invokeMethod(qApp, [self, generation, path, entries] {
                        if (self) {
                            self->onExpectedEntries(path, generation, entries);
                        }
                    }, Qt::QueuedConnection);
                }
            }

            SyncOptions options;
            options.wholeTree = wholeTree;
            options.excludedNames = excluded;
            options.pause = gate.get();
            options.onFolder = [watcher](const QString &folder) { watcher->addFolder(folder); };
            if (wholeTree) {
                options.onProgress = [self, generation, path](const SyncProgress &progress) {
                    QMetaObject::invokeMethod(qApp, [self, generation, path, progress] {
                        if (self) {
                            self->onProgress(path, generation, progress);
                        }
                    }, Qt::QueuedConnection);
                };
            }
            core::Status status;
            for (const QString &folder : std::as_const(targets)) {
                const core::Result<SyncProgress> synced = index.sync(folder, options, cancel);
                if (!synced.ok()) {
                    status = synced.error();
                    break;
                }
            }

            // Hasta dónde queda al día: lo recorrido o puesto al día con el
            // diario, desde `start`; con los avisos del sistema, desde donde
            // empezó la tarea anterior (lo que cambió antes de ese punto ya
            // llegó seguro, aunque un aviso tarde un instante).
            if (status.ok() && journal) {
                const JournalPosition reached =
                    (wholeTree || catchUp) ? start : (watcherCoversAll ? previousStart : JournalPosition{});
                if (reached.isValid()) {
                    index.setProperty(journalProperty(), reached.toString());
                }
            }
            const IndexInfo info = index.info();
            QMetaObject::invokeMethod(qApp, [self, generation, path, status, info, journalFolders, start] {
                if (self) {
                    self->onFinished(path, generation, status, info, journalFolders, start);
                }
            }, Qt::QueuedConnection);
        });
}

void IndexService::onExpectedEntries(const QString &path, quint64 generation, qint64 entries)
{
    const auto found = m_roots.find(path);
    if (found == m_roots.end() || found->second->generation != generation) {
        return;
    }
    found->second->status.expectedEntries = entries;
    emit changed();
}

void IndexService::onProgress(const QString &path, quint64 generation, const SyncProgress &progress)
{
    const auto found = m_roots.find(path);
    if (found == m_roots.end() || found->second->generation != generation) {
        return;
    }
    found->second->status.progress = progress;
    emit changed();
}

void IndexService::onFinished(const QString &path, quint64 generation, const core::Status &status,
                              const IndexInfo &info, qint64 journalFolders, const JournalPosition &start)
{
    const auto found = m_roots.find(path);
    if (found == m_roots.end() || found->second->generation != generation) {
        return;
    }
    Root &root = *found->second;
    root.running = false;
    m_running = false;
    root.status.info = info;
    root.status.progress = {};
    if (journalFolders >= 0) {
        root.status.journalFolders = journalFolders;
    }
    if (info.complete) {
        root.status.expectedEntries = 0;
    }
    if (status.ok()) {
        root.previousStart = start;
    }
    if (status.ok()) {
        root.status.state = info.complete ? RootStatus::State::Ready : RootStatus::State::Building;
        root.status.error.clear();
    } else if (status.error().code == core::ErrorCode::Disconnected) {
        root.status.state = RootStatus::State::Disconnected;
        root.status.error = status.error().message;
    } else if (status.error().code != core::ErrorCode::Cancelled) {
        root.status.state = RootStatus::State::Failed;
        root.status.error = status.error().message;
    }
    emit changed();
    startNext();
}

} // namespace eu::index
