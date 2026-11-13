// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/IndexService.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QPointer>

#include <chrono>

namespace eu::index {

/// Lo que se guarda de cada carpeta indexada.
struct IndexService::Root
{
    explicit Root(Index openedIndex) : index(std::move(openedIndex)) {}

    Index index;
    std::unique_ptr<TreeWatcher> watcher;
    RootStatus status;
    QSet<QString> dirty;        ///< Carpetas de las que llegó un aviso.
    bool wholePending = false;  ///< Hay que recorrerlo entero.
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
    root->wholePending = true;   // construirlo, o ponerlo al día de lo que cambió con la aplicación cerrada
    root->watcher = TreeWatcher::create(clean);
    Root *raw = root.get();
    connect(root->watcher.get(), &TreeWatcher::folderChanged, this, [this, raw](const QString &folder) {
        raw->dirty.insert(folder);
        schedule();
    });
    connect(root->watcher.get(), &TreeWatcher::overflowed, this, [this, raw] {
        raw->wholePending = true;
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
        if (root->running || root->wholePending || !root->dirty.isEmpty()) {
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
        if (root->wholePending) {
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
    const bool whole = root.wholePending || root.dirty.size() > 5000;
    QStringList folders;
    if (!whole) {
        folders = QStringList(root.dirty.constBegin(), root.dirty.constEnd());
    }
    root.dirty.clear();
    root.wholePending = false;
    root.running = true;
    m_running = true;
    if (whole) {
        root.status.state = root.status.info.complete ? RootStatus::State::Updating : RootStatus::State::Building;
        root.status.progress = {};
        root.status.error.clear();
        emit changed();
    }

    const QPointer<IndexService> self(this);
    const quint64 generation = root.generation;
    const QString path = root.status.root;
    TreeWatcher *watcher = root.watcher.get();   // vive hasta que la tarea termina (ver stopTask)
    root.task = m_scheduler.submit(
        tasks::Category::Indexing, tasks::Priority::Background,
        [self, generation, path, whole, folders, watcher, index = root.index, gate = m_gate,
         excluded = m_excludedNames](const tasks::CancelToken &cancel) {
            SyncOptions options;
            options.wholeTree = whole;
            options.excludedNames = excluded;
            options.pause = gate.get();
            options.onFolder = [watcher](const QString &folder) { watcher->addFolder(folder); };
            if (whole) {
                options.onProgress = [self, generation, path](const SyncProgress &progress) {
                    QMetaObject::invokeMethod(qApp, [self, generation, path, progress] {
                        if (self) {
                            self->onProgress(path, generation, progress);
                        }
                    }, Qt::QueuedConnection);
                };
            }
            core::Status status;
            const QStringList targets = whole ? QStringList{path} : folders;
            for (const QString &folder : targets) {
                const core::Result<SyncProgress> synced = index.sync(folder, options, cancel);
                if (!synced.ok()) {
                    status = synced.error();
                    break;
                }
            }
            const IndexInfo info = index.info();
            QMetaObject::invokeMethod(qApp, [self, generation, path, status, info] {
                if (self) {
                    self->onFinished(path, generation, status, info);
                }
            }, Qt::QueuedConnection);
        });
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
                              const IndexInfo &info)
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
