// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/thumbs/ThumbnailService.h"

#include <QCoreApplication>
#include <QPointer>

namespace eu::thumbs {

ThumbnailService::ThumbnailService(tasks::Scheduler &scheduler, ThumbnailCache &cache, QObject *parent)
    : QObject(parent), m_scheduler(scheduler), m_cache(cache)
{
}

ThumbnailService::~ThumbnailService()
{
    cancelAll();
}

ThumbnailCache::Entry ThumbnailService::request(const ThumbnailRequest &request)
{
    ThumbnailCache::Entry entry = m_cache.lookupMemory(request);
    if (entry.state != ThumbnailCache::State::Missing) {
        return entry;
    }
    if (thumbnailSourceFor(request) == ThumbnailSource::None) {
        return ThumbnailCache::Entry{ThumbnailCache::State::Failed, {}};
    }
    const QString key = ThumbnailCache::keyOf(request);
    if (m_pending.contains(key)) {
        return entry;
    }
    const QPointer<ThumbnailService> self(this);
    const quint64 id = ++m_lastId;
    ThumbnailCache *cache = &m_cache;   // vive más que el servicio (lo crea quien crea el servicio)
    // Disco y decodificación en una tarea; en un disco lento cede el paso a
    // abrir carpetas, como el resto del trabajo de fondo.
    // Lo lento (documentos que pasan por LibreOffice, videos) va detrás de
    // las fotos, que se generan en milisegundos.
    const ThumbnailSource source = thumbnailSourceFor(request);
    const tasks::Priority priority = (source == ThumbnailSource::Office || source == ThumbnailSource::Video)
        ? tasks::Priority::Background
        : tasks::Priority::VisibleThumbnail;
    tasks::TaskHandle task = m_scheduler.submitForPaths(
        tasks::Category::Thumbnails, priority, {request.path},
        [self, cache, request, key, id](const tasks::CancelToken &cancel) {
            if (cache->load(request).state == ThumbnailCache::State::Missing) {
                const core::Result<QImage> image = renderThumbnail(request, cancel);
                if (image.ok()) {
                    cache->store(request, image.value());
                } else if (image.error().code != core::ErrorCode::Cancelled) {
                    cache->storeFailure(request);
                }
            }
            QMetaObject::invokeMethod(qApp, [self, key, id] {
                if (self) {
                    self->onFinished(key, id);
                }
            }, Qt::QueuedConnection);
        });
    m_pending.insert(key, Pending{request.path, task, id});
    return entry;
}

void ThumbnailService::onFinished(const QString &key, quint64 id)
{
    const auto found = m_pending.constFind(key);
    if (found == m_pending.constEnd() || found->id != id) {
        return;   // se canceló entre tanto (y quizá se volvió a pedir)
    }
    const QString path = found->path;
    m_pending.erase(found);
    emit ready(path);
}

void ThumbnailService::retainOnly(const QSet<QString> &paths)
{
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (!paths.contains(it->path)) {
            it->task.cancel();
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
}

void ThumbnailService::cancelAll()
{
    for (Pending &pending : m_pending) {
        pending.task.cancel();
    }
    m_pending.clear();
}

} // namespace eu::thumbs
