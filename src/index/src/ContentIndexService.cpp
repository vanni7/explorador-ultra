// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/ContentIndexService.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QPointer>

#include <chrono>

namespace eu::index {

struct ContentIndexService::Root
{
    explicit Root(ContentIndex openedIndex) : index(std::move(openedIndex)) {}

    ContentIndex index;
    ContentRootStatus status;
    bool pending = false;   ///< Hay que repasarla.
    bool running = false;
    tasks::TaskHandle task;
    quint64 generation = 0;
};

ContentIndexService::ContentIndexService(tasks::Scheduler &scheduler, QString dataDirectory, QObject *parent)
    : QObject(parent), m_scheduler(scheduler), m_dataDirectory(std::move(dataDirectory)),
      m_gate(std::make_shared<search::PauseGate>())
{
    m_startTimer.setSingleShot(true);
    m_startTimer.setInterval(500);
    connect(&m_startTimer, &QTimer::timeout, this, &ContentIndexService::startNext);
    // Sin vigilancia propia: se repasa cada rato. Mientras tanto, lo que
    // cambió se lee del disco al buscar (el índice nunca da resultados viejos).
    m_reviewTimer.setInterval(30 * 60 * 1000);
    connect(&m_reviewTimer, &QTimer::timeout, this, [this] {
        for (auto &[path, root] : m_roots) {
            root->pending = true;
        }
        schedule();
    });
    m_reviewTimer.start();
}

ContentIndexService::~ContentIndexService()
{
    m_gate->setPaused(false);
    for (auto &[path, root] : m_roots) {
        stopTask(*root);
    }
}

QString ContentIndexService::databasePathFor(const QString &root) const
{
    const QByteArray hash = QCryptographicHash::hash(QDir::cleanPath(root).toUtf8(), QCryptographicHash::Sha1);
    return m_dataDirectory + QStringLiteral("/contenido-") + QString::fromLatin1(hash.toHex().left(16))
        + QStringLiteral(".db");
}

QStringList ContentIndexService::roots() const
{
    QStringList paths;
    for (const auto &[path, root] : m_roots) {
        paths.append(path);
    }
    paths += m_failures.keys();
    return paths;
}

QList<ContentRootStatus> ContentIndexService::status() const
{
    QList<ContentRootStatus> all;
    for (const auto &[path, root] : m_roots) {
        all.append(root->status);
    }
    for (auto failure = m_failures.constBegin(); failure != m_failures.constEnd(); ++failure) {
        ContentRootStatus status;
        status.root = failure.key();
        status.state = ContentRootStatus::State::Failed;
        status.error = failure.value();
        all.append(status);
    }
    return all;
}

void ContentIndexService::addRoot(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (clean.isEmpty() || m_roots.count(clean) != 0) {
        return;
    }
    m_failures.remove(clean);
    const core::Result<ContentIndex> opened = ContentIndex::open(databasePathFor(clean), clean);
    if (!opened.ok()) {
        m_failures.insert(clean, opened.error().message);
        emit changed();
        return;
    }
    auto root = std::make_unique<Root>(opened.value());
    root->status.root = clean;
    root->status.info = root->index.info();
    root->status.state = ContentRootStatus::State::Waiting;
    root->pending = true;   // construirlo, o ver qué cambió con la aplicación cerrada
    m_roots.emplace(clean, std::move(root));
    emit changed();
    schedule();
}

void ContentIndexService::stopTask(Root &root)
{
    if (root.running) {
        root.task.cancel();
        // Cancelar corta también la lectura del documento en curso.
        root.task.waitFor(std::chrono::seconds(60));
        root.running = false;
        m_running = false;
    }
    ++root.generation;
}

void ContentIndexService::removeRoot(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (m_failures.remove(clean) > 0) {
        ContentIndex::remove(databasePathFor(clean));
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
    ContentIndex::remove(database);
    emit changed();
    schedule();
}

void ContentIndexService::refresh(const QString &path)
{
    const auto found = m_roots.find(QDir::cleanPath(path));
    if (found == m_roots.end()) {
        return;
    }
    found->second->pending = true;
    if (!found->second->running) {
        found->second->status.state = ContentRootStatus::State::Waiting;
        emit changed();
    }
    schedule();
}

void ContentIndexService::rebuild(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (m_roots.count(clean) == 0 && !m_failures.contains(clean)) {
        return;
    }
    removeRoot(clean);
    addRoot(clean);
}

void ContentIndexService::setPaused(bool paused)
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

bool ContentIndexService::isBusy() const
{
    for (const auto &[path, root] : m_roots) {
        if (root->running || root->pending) {
            return true;
        }
    }
    return false;
}

std::function<std::unique_ptr<search::ContentCache>()>
ContentIndexService::cacheFactory(const QStringList &searchRoots) const
{
    // Los índices que cubren alguna raíz de la búsqueda o están dentro de ella.
    QList<ContentIndex> useful;
    for (const auto &[path, root] : m_roots) {
        if (root->status.state == ContentRootStatus::State::Failed || root->status.info.documents == 0) {
            continue;
        }
        for (const QString &searchRoot : searchRoots) {
            const QString clean = QDir::cleanPath(searchRoot);
            const QString prefix = clean.endsWith(QLatin1Char('/')) ? clean : clean + QLatin1Char('/');
            if (root->index.covers(clean) || path.startsWith(prefix)) {
                useful.append(root->index);
                break;
            }
        }
    }
    if (useful.isEmpty()) {
        return {};
    }
    return [useful] { return ContentIndex::cacheFor(useful); };
}

void ContentIndexService::schedule()
{
    if (!m_startTimer.isActive()) {
        m_startTimer.start();
    }
}

void ContentIndexService::startNext()
{
    if (m_running || m_paused) {
        return;
    }
    Root *next = nullptr;
    for (auto &[path, root] : m_roots) {
        if (root->pending) {
            next = root.get();
            break;
        }
    }
    if (next == nullptr) {
        return;
    }
    Root &root = *next;
    root.pending = false;
    root.running = true;
    m_running = true;
    root.status.state = root.status.info.complete ? ContentRootStatus::State::Updating
                                                  : ContentRootStatus::State::Building;
    root.status.progress = {};
    root.status.error.clear();
    emit changed();

    const QPointer<ContentIndexService> self(this);
    const quint64 generation = root.generation;
    const QString path = root.status.root;
    root.task = m_scheduler.submitForPaths(
        tasks::Category::Indexing, tasks::Priority::Background, {path},
        [self, generation, path, index = root.index, gate = m_gate, provider = m_textProvider,
         excluded = m_excludedNames](const tasks::CancelToken &cancel) {
            ContentSyncOptions options;
            options.textProvider = provider;
            options.excludedNames = excluded;
            options.pause = gate.get();
            options.onProgress = [self, generation, path](const ContentSyncProgress &progress) {
                QMetaObject::invokeMethod(qApp, [self, generation, path, progress] {
                    if (self) {
                        self->onProgress(path, generation, progress);
                    }
                }, Qt::QueuedConnection);
            };
            const core::Result<ContentSyncProgress> synced = index.sync(options, cancel);
            const core::Status status = synced.ok() ? core::Status{} : core::Status{synced.error()};
            const ContentIndexInfo info = index.info();
            QMetaObject::invokeMethod(qApp, [self, generation, path, status, info] {
                if (self) {
                    self->onFinished(path, generation, status, info);
                }
            }, Qt::QueuedConnection);
        });
}

void ContentIndexService::onProgress(const QString &path, quint64 generation, const ContentSyncProgress &progress)
{
    const auto found = m_roots.find(path);
    if (found == m_roots.end() || found->second->generation != generation) {
        return;
    }
    found->second->status.progress = progress;
    emit changed();
}

void ContentIndexService::onFinished(const QString &path, quint64 generation, const core::Status &status,
                                     const ContentIndexInfo &info)
{
    const auto found = m_roots.find(path);
    if (found == m_roots.end() || found->second->generation != generation) {
        return;
    }
    Root &root = *found->second;
    root.running = false;
    m_running = false;
    root.status.info = info;
    if (status.ok()) {
        root.status.state = ContentRootStatus::State::Ready;
        root.status.error.clear();
    } else if (status.error().code == core::ErrorCode::Disconnected) {
        root.status.state = ContentRootStatus::State::Disconnected;
        root.status.error = status.error().message;
    } else if (status.error().code == core::ErrorCode::Cancelled) {
        root.status.state = ContentRootStatus::State::Waiting;
    } else {
        root.status.state = ContentRootStatus::State::Failed;
        root.status.error = status.error().message;
    }
    root.status.progress = {};
    emit changed();
    startNext();
}

} // namespace eu::index
