// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/TreeWatcher.h"

#include <QDir>
#include <QSet>

#include <thread>
#include <vector>

#include <windows.h>

namespace eu::index {

namespace {

/// Un hilo espera los avisos de ReadDirectoryChangesW sobre la raíz, con todo
/// el árbol, y los pasa al hilo del objeto.
class WindowsTreeWatcher : public TreeWatcher
{
public:
    explicit WindowsTreeWatcher(const QString &root) : TreeWatcher(root)
    {
        const std::wstring native = QDir::toNativeSeparators(this->root()).toStdWString();
        m_directory = CreateFileW(native.c_str(), FILE_LIST_DIRECTORY,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (m_directory == INVALID_HANDLE_VALUE) {
            return;
        }
        m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_thread = std::thread([this] { run(); });
        // Hasta que la primera petición está hecha no se ve ningún cambio.
        WaitForSingleObject(m_ready, 5000);
    }

    ~WindowsTreeWatcher() override
    {
        if (m_directory == INVALID_HANDLE_VALUE) {
            return;
        }
        SetEvent(m_stop);
        CancelIoEx(m_directory, nullptr);
        if (m_thread.joinable()) {
            m_thread.join();
        }
        CloseHandle(m_directory);
        CloseHandle(m_stop);
        CloseHandle(m_ready);
    }

    bool isActive() const override { return m_directory != INVALID_HANDLE_VALUE; }
    void addFolder(const QString &) override {}   // el sistema ya vigila el árbol entero

private:
    void run()
    {
        std::vector<DWORD> buffer(64 * 1024 / sizeof(DWORD));   // alineado a DWORD, como pide la API
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE
            | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES;
        while (true) {
            ResetEvent(overlapped.hEvent);
            const BOOL requested = ReadDirectoryChangesW(m_directory, buffer.data(),
                                                         DWORD(buffer.size() * sizeof(DWORD)), TRUE, filter, nullptr,
                                                         &overlapped, nullptr);
            SetEvent(m_ready);
            if (!requested) {
                break;
            }
            const HANDLE handles[2] = {overlapped.hEvent, m_stop};
            const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (waited != WAIT_OBJECT_0) {
                CancelIoEx(m_directory, &overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(m_directory, &overlapped, &ignored, TRUE);
                break;
            }
            DWORD bytes = 0;
            if (!GetOverlappedResult(m_directory, &overlapped, &bytes, FALSE)) {
                if (GetLastError() == ERROR_NOTIFY_ENUM_DIR) {
                    QMetaObject::invokeMethod(this, [this] { emit overflowed(); }, Qt::QueuedConnection);
                    continue;
                }
                break;
            }
            if (bytes == 0) {
                // Hubo más cambios de los que cabían: no se sabe cuáles.
                QMetaObject::invokeMethod(this, [this] { emit overflowed(); }, Qt::QueuedConnection);
                continue;
            }
            QSet<QString> changed;
            const auto *base = reinterpret_cast<const BYTE *>(buffer.data());
            DWORD offset = 0;
            while (true) {
                const auto *info = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(base + offset);
                QString relative = QString::fromWCharArray(info->FileName, int(info->FileNameLength / sizeof(WCHAR)));
                relative.replace(QLatin1Char('\\'), QLatin1Char('/'));
                // Cambia la carpeta que contiene al elemento nombrado.
                const qsizetype slash = relative.lastIndexOf(QLatin1Char('/'));
                const QString prefix = root().endsWith(QLatin1Char('/')) ? root() : root() + QLatin1Char('/');
                changed.insert(slash < 0 ? root() : prefix + relative.left(slash));
                if (info->NextEntryOffset == 0) {
                    break;
                }
                offset += info->NextEntryOffset;
            }
            QMetaObject::invokeMethod(this, [this, changed] {
                for (const QString &folder : changed) {
                    emit folderChanged(folder);
                }
            }, Qt::QueuedConnection);
        }
        CloseHandle(overlapped.hEvent);
    }

    HANDLE m_directory = INVALID_HANDLE_VALUE;
    HANDLE m_stop = nullptr;
    HANDLE m_ready = nullptr;
    std::thread m_thread;
};

} // namespace

std::unique_ptr<TreeWatcher> TreeWatcher::create(const QString &root)
{
    return std::unique_ptr<TreeWatcher>(new WindowsTreeWatcher(root));
}

} // namespace eu::index
