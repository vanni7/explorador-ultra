// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
//
// Listado para Windows: FindFirstFileExW entrega nombre, tamaño, fechas y
// atributos de cada elemento en la misma llamada, sin consultas adicionales.

#include "BatchSender.h"

#include <QCoreApplication>
#include <QDir>

#include <windows.h>

namespace eu::fs {

namespace {

/// Cierra el identificador de búsqueda al salir del ámbito (RAII).
struct FindCloser
{
    HANDLE handle;
    ~FindCloser() { FindClose(handle); }
};

/// Convierte una ruta en su forma extendida (\\?\C:\... o \\?\UNC\servidor\...),
/// que admite más de 260 caracteres (SPEC §8.3).
std::wstring toExtendedPath(const QString &path)
{
    QString native = QDir::toNativeSeparators(QDir(path).absolutePath());
    if (native.startsWith(QLatin1String("\\\\?\\"))) {
        return native.toStdWString();
    }
    if (native.startsWith(QLatin1String("\\\\"))) {
        return (QStringLiteral("\\\\?\\UNC\\") + native.mid(2)).toStdWString();
    }
    return (QStringLiteral("\\\\?\\") + native).toStdWString();
}

/// FILETIME cuenta intervalos de 100 ns desde 1601; se pasa a ms desde 1970.
qint64 fileTimeToMs(const FILETIME &time)
{
    const qint64 ticks = (qint64(time.dwHighDateTime) << 32) | qint64(time.dwLowDateTime);
    if (ticks == 0) {
        return 0;
    }
    return (ticks - 116444736000000000LL) / 10000;
}

} // namespace

core::Status listDirectory(const QString &path, const ListOptions &options,
                           const tasks::CancelToken &cancel, const BatchCallback &onBatch)
{
    std::wstring pattern = toExtendedPath(path);
    if (!pattern.empty() && pattern.back() != L'\\') {
        pattern += L'\\';
    }
    pattern += L'*';

    WIN32_FIND_DATAW data;
    // FindExInfoBasic evita calcular el nombre corto 8.3; LARGE_FETCH pide
    // bloques grandes al sistema: ambas cosas aceleran las carpetas enormes.
    const HANDLE handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                           FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD lastError = GetLastError();
        if (lastError == ERROR_FILE_NOT_FOUND) {
            return {};   // carpeta raíz vacía: no es un error
        }
        return core::errorFromWindows(lastError, path);
    }
    const FindCloser closer{handle};

    BatchSender sender(options, onBatch);
    int sinceCancelCheck = 0;

    do {
        if (++sinceCancelCheck >= 256) {
            sinceCancelCheck = 0;
            if (cancel.isCancelled()) {
                return core::Error{core::ErrorCode::Cancelled,
                                   QCoreApplication::translate("eu::fs", "Listado cancelado."), path, 0};
            }
        }

        const wchar_t *name = data.cFileName;
        if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'))) {
            continue;
        }

        Entry entry;
        entry.name = QString::fromWCharArray(name);
        entry.size = (qint64(data.nFileSizeHigh) << 32) | qint64(data.nFileSizeLow);
        entry.modifiedMs = fileTimeToMs(data.ftLastWriteTime);
        entry.createdMs = fileTimeToMs(data.ftCreationTime);
        entry.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.isHidden = (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
        // Enlaces simbólicos y puntos de unión: se marcan para no seguirlos al borrar.
        entry.isSymlink = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
            && (data.dwReserved0 == IO_REPARSE_TAG_SYMLINK || data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT);
        sender.add(std::move(entry));
    } while (FindNextFileW(handle, &data) != 0);

    const DWORD lastError = GetLastError();
    sender.flush();
    if (lastError != ERROR_NO_MORE_FILES) {
        return core::errorFromWindows(lastError, path);
    }
    return {};
}

} // namespace eu::fs
