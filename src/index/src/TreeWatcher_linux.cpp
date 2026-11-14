// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/TreeWatcher.h"

#include <QFile>
#include <QHash>
#include <QSet>
#include <QSocketNotifier>

#include <atomic>
#include <cerrno>
#include <mutex>

#include <sys/inotify.h>
#include <unistd.h>

namespace eu::index {

namespace {

/// Vigilancias que usan todos los índices de este proceso.
std::atomic<qint64> s_watchesInUse{0};

/// Cuántas vigilancias se permite usar a la aplicación. El límite del sistema
/// (fs.inotify.max_user_watches) es por usuario y lo comparten todos sus
/// programas: agotarlo dejaría sin avisos a editores, sincronizadores, etc.
/// Se usa como mucho el 60 %.
qint64 watchBudget()
{
    static const qint64 budget = [] {
        qint64 systemLimit = 8192;
        QFile file(QStringLiteral("/proc/sys/fs/inotify/max_user_watches"));
        if (file.open(QIODevice::ReadOnly)) {
            const qint64 value = file.readAll().trimmed().toLongLong();
            if (value > 0) {
                systemLimit = value;
            }
        }
        return systemLimit * 6 / 10;
    }();
    return budget;
}

class InotifyTreeWatcher : public TreeWatcher
{
public:
    explicit InotifyTreeWatcher(const QString &root) : TreeWatcher(root)
    {
        m_descriptor = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_descriptor < 0) {
            return;
        }
        m_notifier = new QSocketNotifier(m_descriptor, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, [this] { readEvents(); });
    }

    ~InotifyTreeWatcher() override
    {
        if (m_descriptor >= 0) {
            delete m_notifier;
            ::close(m_descriptor);   // cierra también todas las vigilancias
            s_watchesInUse -= m_folders.size();
        }
    }

    bool isActive() const override { return m_descriptor >= 0; }
    bool isLimited() const override { return m_limited.load(); }

    qint64 watchedFolders() const override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_folders.size();
    }

    void addFolder(const QString &folder) override
    {
        if (m_descriptor < 0) {
            return;
        }
        // Lo que cambia la lista de una carpeta: crear, borrar, renombrar y
        // terminar de escribir (que cambia el tamaño). IN_MODIFY se deja
        // fuera: avisaría en cada escritura de un archivo que se está copiando.
        if (s_watchesInUse.load() >= watchBudget()) {
            reportLimit();
            return;
        }
        const uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE_SELF
            | IN_ONLYDIR | IN_DONT_FOLLOW;
        const int watch = ::inotify_add_watch(m_descriptor, QFile::encodeName(folder).constData(), mask);
        if (watch < 0) {
            // ENOSPC: se agotó fs.inotify.max_user_watches. Lo demás (la
            // carpeta ya no existe, sin permiso) no tiene remedio ni importa.
            if (errno == ENOSPC) {
                reportLimit();
            }
            return;
        }
        // Si la carpeta se movió, el sistema devuelve la misma vigilancia y
        // aquí queda anotada con su ruta nueva.
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_folders.contains(watch)) {
            ++s_watchesInUse;
        }
        m_folders.insert(watch, folder);
    }

private:
    void reportLimit()
    {
        if (!m_limited.exchange(true)) {
            QMetaObject::invokeMethod(this, [this] { emit limitReached(); }, Qt::QueuedConnection);
        }
    }

    void readEvents()
    {
        alignas(inotify_event) char buffer[64 * 1024];
        QSet<QString> changed;
        bool overflow = false;
        while (true) {
            const ssize_t size = ::read(m_descriptor, buffer, sizeof(buffer));
            if (size <= 0) {
                break;   // EAGAIN: no queda nada por leer
            }
            const std::lock_guard<std::mutex> lock(m_mutex);
            for (ssize_t offset = 0; offset < size;) {
                const auto *event = reinterpret_cast<const inotify_event *>(buffer + offset);
                offset += ssize_t(sizeof(inotify_event)) + ssize_t(event->len);
                if (event->mask & IN_Q_OVERFLOW) {
                    overflow = true;
                    continue;
                }
                const auto folder = m_folders.constFind(event->wd);
                if (folder == m_folders.constEnd()) {
                    continue;
                }
                if (event->mask & IN_IGNORED) {
                    m_folders.erase(folder);   // el sistema dejó de vigilarla (se borró)
                    --s_watchesInUse;
                    continue;
                }
                changed.insert(*folder);
            }
        }
        for (const QString &folder : std::as_const(changed)) {
            emit folderChanged(folder);
        }
        if (overflow) {
            emit overflowed();
        }
    }

    int m_descriptor = -1;
    QSocketNotifier *m_notifier = nullptr;
    mutable std::mutex m_mutex;
    QHash<int, QString> m_folders;   ///< Vigilancia -> carpeta.
    std::atomic<bool> m_limited{false};
};

} // namespace

std::unique_ptr<TreeWatcher> TreeWatcher::create(const QString &root)
{
    return std::unique_ptr<TreeWatcher>(new InotifyTreeWatcher(root));
}

} // namespace eu::index
