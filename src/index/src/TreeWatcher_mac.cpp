// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/TreeWatcher.h"

#include <QFileInfo>
#include <QSet>

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

namespace eu::index {

namespace {

/// FSEvents avisa de las carpetas que cambian en todo el árbol de la raíz.
class FsEventsTreeWatcher : public TreeWatcher
{
public:
    explicit FsEventsTreeWatcher(const QString &root) : TreeWatcher(root)
    {
        // FSEvents da rutas reales ("/private/var/…" en vez de "/var/…"):
        // se convierten a la forma con la que se pidió la raíz.
        m_canonicalRoot = QFileInfo(this->root()).canonicalFilePath();
        if (m_canonicalRoot.isEmpty()) {
            return;
        }
        const CFStringRef path = m_canonicalRoot.toCFString();
        const void *values[] = {path};
        const CFArrayRef paths = CFArrayCreate(nullptr, values, 1, &kCFTypeArrayCallBacks);
        FSEventStreamContext context{};
        context.info = this;
        m_stream = FSEventStreamCreate(nullptr, &FsEventsTreeWatcher::onEvents, &context, paths,
                                       kFSEventStreamEventIdSinceNow, 0.3, kFSEventStreamCreateFlagNoDefer);
        CFRelease(paths);
        CFRelease(path);
        if (m_stream == nullptr) {
            return;
        }
        m_queue = dispatch_queue_create("explorador-ultra.indice", DISPATCH_QUEUE_SERIAL);
        FSEventStreamSetDispatchQueue(m_stream, m_queue);
        if (!FSEventStreamStart(m_stream)) {
            FSEventStreamInvalidate(m_stream);
            FSEventStreamRelease(m_stream);
            m_stream = nullptr;
            dispatch_release(m_queue);
            m_queue = nullptr;
        }
    }

    ~FsEventsTreeWatcher() override
    {
        if (m_stream == nullptr) {
            return;
        }
        FSEventStreamStop(m_stream);
        FSEventStreamInvalidate(m_stream);
        // Espera a que termine un aviso que estuviera a medias.
        dispatch_sync_f(m_queue, nullptr, [](void *) {});
        FSEventStreamRelease(m_stream);
        dispatch_release(m_queue);
    }

    bool isActive() const override { return m_stream != nullptr; }
    void addFolder(const QString &) override {}   // el sistema ya vigila el árbol entero

private:
    static void onEvents(ConstFSEventStreamRef, void *info, size_t count, void *eventPaths,
                         const FSEventStreamEventFlags flags[], const FSEventStreamEventId[])
    {
        auto *self = static_cast<FsEventsTreeWatcher *>(info);
        const char *const *paths = static_cast<const char *const *>(eventPaths);
        QSet<QString> changed;
        bool overflow = false;
        for (size_t i = 0; i < count; ++i) {
            if (flags[i] & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped
                            | kFSEventStreamEventFlagKernelDropped)) {
                overflow = true;
                continue;
            }
            QString path = QString::fromUtf8(paths[i]);
            while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
                path.chop(1);
            }
            if (path == self->m_canonicalRoot) {
                changed.insert(self->root());
            } else if (path.startsWith(self->m_canonicalRoot + QLatin1Char('/'))) {
                changed.insert(self->root() + path.mid(self->m_canonicalRoot.size()));
            }
        }
        QMetaObject::invokeMethod(self, [self, changed, overflow] {
            for (const QString &folder : changed) {
                emit self->folderChanged(folder);
            }
            if (overflow) {
                emit self->overflowed();
            }
        }, Qt::QueuedConnection);
    }

    QString m_canonicalRoot;
    FSEventStreamRef m_stream = nullptr;
    dispatch_queue_t m_queue = nullptr;
};

} // namespace

std::unique_ptr<TreeWatcher> TreeWatcher::create(const QString &root)
{
    return std::unique_ptr<TreeWatcher>(new FsEventsTreeWatcher(root));
}

} // namespace eu::index
