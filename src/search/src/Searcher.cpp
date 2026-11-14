// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/search/Searcher.h"

#include "eu/fs/DirectoryLister.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>

#include <chrono>
#include <deque>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace eu::search {

namespace {

qint64 steadyNowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/// Dispositivo de una carpeta, con el mismo valor que da el listado para sus
/// elementos. 0 si el sistema no lo informa.
std::uint64_t deviceOf(const QString &path)
{
#ifdef Q_OS_UNIX
    struct stat info;
    if (::stat(QFile::encodeName(path).constData(), &info) == 0) {
        return std::uint64_t(info.st_dev);
    }
#else
    Q_UNUSED(path);
#endif
    return 0;
}

QString childPath(const QString &directory, const QString &name)
{
    return directory.endsWith(QLatin1Char('/')) ? directory + name : directory + QLatin1Char('/') + name;
}

/// Lee un archivo como texto la primera vez que la consulta lo pide.
class FileContent : public ContentSource
{
public:
    FileContent(QString path, qint64 maxBytes) : m_path(std::move(path)), m_maxBytes(maxBytes) {}

    const QString *text() override
    {
        if (!m_loaded) {
            m_loaded = true;
            m_ok = load();
        }
        return m_ok ? &m_text : nullptr;
    }

    bool wasOpened() const { return m_opened; }

private:
    bool load()
    {
        // Solo archivos corrientes: abrir una tubería o un dispositivo
        // podría dejar la búsqueda esperando para siempre.
        const QFileInfo info(m_path);
        if (!info.isFile() || info.size() > m_maxBytes) {
            return false;
        }
        QFile file(m_path);
        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }
        m_opened = true;
        const QByteArray data = file.read(m_maxBytes + 1);
        if (data.size() > m_maxBytes) {
            return false;   // creció mientras se leía
        }
        const bool utf16 = data.startsWith("\xff\xfe") || data.startsWith("\xfe\xff");
        if (utf16) {
            m_text = QStringDecoder(QStringDecoder::Utf16).decode(data);
            return true;
        }
        // Un byte nulo al principio delata un binario (PDF, imagen, ZIP…).
        if (data.left(8192).contains('\0')) {
            return false;
        }
        QStringDecoder utf8(QStringDecoder::Utf8);
        m_text = utf8.decode(data);
        if (utf8.hasError()) {
            m_text = QString::fromLatin1(data);   // texto antiguo de Windows
        }
        return true;
    }

    QString m_path;
    qint64 m_maxBytes;
    QString m_text;
    bool m_loaded = false;
    bool m_ok = false;
    bool m_opened = false;
};

struct PendingFolder
{
    QString path;
    std::uint64_t device = 0;
};

} // namespace

QString Hit::path() const
{
    return childPath(directory, entry.name);
}

void PauseGate::setPaused(bool paused)
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_paused = paused;
    }
    m_changed.notify_all();
}

bool PauseGate::isPaused() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_paused;
}

void PauseGate::waitWhilePaused(const tasks::CancelToken &cancel) const
{
    std::unique_lock<std::mutex> lock(m_mutex);
    // La cancelación no despierta la variable de condición: se mira cada
    // poco, lo bastante a menudo para cancelar en mucho menos de 500 ms.
    while (m_paused && !cancel.isCancelled()) {
        m_changed.wait_for(lock, std::chrono::milliseconds(50));
    }
}

SearchSummary searchTree(const QStringList &roots, const Query &query, const SearchOptions &options,
                         const tasks::CancelToken &cancel, const PauseGate *pause, const HitsCallback &onHits)
{
    SearchSummary summary;
    SearchProgress &progress = summary.progress;
    std::vector<Hit> batch;
    qint64 lastDeliveryMs = steadyNowMs();

    const auto deliver = [&](bool force) {
        const qint64 now = steadyNowMs();
        if (!force && qint64(batch.size()) < options.batchSize && now - lastDeliveryMs < options.maxBatchDelayMs) {
            return;
        }
        lastDeliveryMs = now;
        std::vector<Hit> ready;
        ready.swap(batch);
        onHits(std::move(ready), progress);
    };

    // Recorrido a lo ancho: primero lo más cercano a la carpeta de partida,
    // que es donde suele estar lo que se busca.
    std::deque<PendingFolder> pending;
    for (const QString &root : roots) {
        const QString clean = QDir::cleanPath(root);
        pending.push_back({clean, deviceOf(clean)});
    }

    fs::ListOptions listOptions;
    listOptions.firstBatchSize = 2000;
    listOptions.batchSize = 2000;

    const bool readsContent = query.usesContent();

    while (!pending.empty() && !summary.truncated) {
        if (pause != nullptr) {
            pause->waitWhilePaused(cancel);
        }
        if (cancel.isCancelled()) {
            break;
        }
        const PendingFolder folder = pending.front();
        pending.pop_front();
        progress.currentFolder = folder.path;

        const core::Status status = fs::listDirectory(folder.path, listOptions, cancel,
                                                      [&](std::vector<fs::Entry> &&entries) {
            for (fs::Entry &entry : entries) {
                if (summary.truncated) {
                    return;
                }
                if (entry.isHidden && !options.includeHidden) {
                    continue;
                }
                ++progress.itemsExamined;
                // Se baja a las subcarpetas reales; nunca a través de un enlace.
                if (options.recursive && entry.isDirectory && !entry.isSymlink) {
                    const bool otherDevice = folder.device != 0 && entry.deviceId != 0
                        && entry.deviceId != folder.device;
                    if (options.crossDevices || !otherDevice) {
                        pending.push_back({childPath(folder.path, entry.name),
                                           entry.deviceId != 0 ? entry.deviceId : folder.device});
                    }
                }
                bool matched = false;
                if (readsContent) {
                    // Leer archivos es lento: se atiende la pausa y la
                    // cancelación en cada uno, y el avance sigue saliendo.
                    if (pause != nullptr) {
                        pause->waitWhilePaused(cancel);
                    }
                    if (cancel.isCancelled()) {
                        return;
                    }
                    FileContent content(childPath(folder.path, entry.name), options.maxContentBytes);
                    matched = query.matches(itemFrom(entry), &content);
                    if (content.wasOpened()) {
                        ++progress.filesRead;
                        deliver(false);
                    }
                } else {
                    matched = query.matches(entry);
                }
                if (matched) {
                    ++progress.hits;
                    batch.push_back({folder.path, std::move(entry)});
                    if (progress.hits >= options.maxHits) {
                        summary.truncated = true;
                    }
                }
            }
            deliver(false);
            if (pause != nullptr) {
                pause->waitWhilePaused(cancel);
            }
        });
        if (cancel.isCancelled()) {
            break;
        }
        ++progress.foldersScanned;
        if (!status.ok()) {
            ++progress.unreadableFolders;
        }
        deliver(false);
    }

    summary.cancelled = cancel.isCancelled();
    progress.currentFolder.clear();
    deliver(true);
    return summary;
}

} // namespace eu::search
