// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ops/FileOps.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimeZone>

#include <cerrno>
#include <filesystem>
#include <system_error>
#include <vector>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef Q_OS_LINUX
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

#ifdef Q_OS_MACOS
#include <stdio.h>
#endif

namespace eu::ops {

namespace {

std::filesystem::path toStdPath(const QString &path)
{
    return std::filesystem::path(path.toStdU16String());
}

/// Convierte un error de std::filesystem en uno nuestro. En POSIX el valor es
/// un errno; en Windows, un código de GetLastError.
core::Error fromStdError(const std::error_code &code, const QString &path)
{
#ifdef Q_OS_WIN
    return core::errorFromWindows(static_cast<unsigned long>(code.value()), path);
#else
    return core::errorFromErrno(code.value(), path);
#endif
}

core::Error lastSystemError(const QString &path)
{
#ifdef Q_OS_WIN
    return core::errorFromWindows(GetLastError(), path);
#else
    return core::errorFromErrno(errno, path);
#endif
}

/// Error a partir de un QFile que falló, cuando no hay código del sistema.
core::Error fromQFile(const QFile &file, const QString &path)
{
    core::ErrorCode code = core::ErrorCode::IoError;
    switch (file.error()) {
    case QFileDevice::PermissionsError: code = core::ErrorCode::PermissionDenied; break;
    case QFileDevice::OpenError:        code = QFileInfo::exists(path) ? core::ErrorCode::PermissionDenied
                                                                       : core::ErrorCode::NotFound; break;
    case QFileDevice::ResourceError:    code = core::ErrorCode::NoSpace; break;
    default: break;
    }
    return core::Error{code, file.errorString(), path, 0};
}

/// Obliga al sistema a llevar los datos del archivo al disco.
bool syncToDisk(QFile &file)
{
    if (!file.flush()) {
        return false;
    }
#ifdef Q_OS_WIN
    return FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()))) != 0;
#else
    return fsync(file.handle()) == 0;
#endif
}

} // namespace

FileStamp stampOf(const QString &path)
{
    FileStamp stamp;
    std::error_code code;
    // symlink_status no sigue el enlace: describe el enlace mismo.
    const std::filesystem::file_status status = std::filesystem::symlink_status(toStdPath(path), code);
    if (code || status.type() == std::filesystem::file_type::not_found) {
        return stamp;
    }
    stamp.exists = true;
    stamp.isSymlink = (status.type() == std::filesystem::file_type::symlink);
    stamp.isDirectory = (status.type() == std::filesystem::file_type::directory);
    if (status.type() == std::filesystem::file_type::regular) {
        const QFileInfo info(path);
        stamp.size = info.size();
        // En UTC: pasar por la hora local puede desplazar la fecha (ver copyFile).
        stamp.modifiedMs = info.lastModified(QTimeZone::UTC).toMSecsSinceEpoch();
    }
    return stamp;
}

core::Status makeDirectory(const QString &path)
{
    std::error_code code;
    if (std::filesystem::create_directory(toStdPath(path), code)) {
        return {};
    }
    if (!code) {
        // create_directory devuelve false sin error cuando ya existía.
        return core::errorFromErrno(EEXIST, path);
    }
    return fromStdError(code, path);
}

core::Status createEmptyFile(const QString &path)
{
    QFile file(path);
    // NewOnly: falla si ya existe, en vez de vaciarlo.
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (QFileInfo::exists(path)) {
            return core::errorFromErrno(EEXIST, path);
        }
        return fromQFile(file, path);
    }
    return {};
}

core::Status renameNoReplace(const QString &source, const QString &target)
{
#if defined(Q_OS_LINUX)
    const QByteArray from = QFile::encodeName(source);
    const QByteArray to = QFile::encodeName(target);
    if (syscall(SYS_renameat2, AT_FDCWD, from.constData(), AT_FDCWD, to.constData(), RENAME_NOREPLACE) == 0) {
        return {};
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return core::errorFromErrno(errno, errno == EEXIST ? target : source);
    }
    // El sistema de archivos no admite "sin reemplazar" (algunos FUSE, FAT…):
    // se comprueba antes y se renombra. Queda una ventana mínima entre ambas
    // cosas, inevitable en esos sistemas.
    if (stampOf(target).exists) {
        return core::errorFromErrno(EEXIST, target);
    }
    if (rename(from.constData(), to.constData()) == 0) {
        return {};
    }
    return core::errorFromErrno(errno, source);
#elif defined(Q_OS_MACOS)
    const QByteArray from = QFile::encodeName(source);
    const QByteArray to = QFile::encodeName(target);
    if (renamex_np(from.constData(), to.constData(), RENAME_EXCL) == 0) {
        return {};
    }
    if (errno != ENOTSUP) {
        return core::errorFromErrno(errno, errno == EEXIST ? target : source);
    }
    if (stampOf(target).exists) {
        return core::errorFromErrno(EEXIST, target);
    }
    if (rename(from.constData(), to.constData()) == 0) {
        return {};
    }
    return core::errorFromErrno(errno, source);
#elif defined(Q_OS_WIN)
    // Sin MOVEFILE_REPLACE_EXISTING falla si el destino existe; sin
    // MOVEFILE_COPY_ALLOWED falla entre volúmenes en vez de copiar a escondidas.
    const std::wstring from = QDir::toNativeSeparators(source).toStdWString();
    const std::wstring to = QDir::toNativeSeparators(target).toStdWString();
    if (MoveFileExW(from.c_str(), to.c_str(), 0) != 0) {
        return {};
    }
    const DWORD lastError = GetLastError();
    return core::errorFromWindows(lastError, lastError == ERROR_ALREADY_EXISTS || lastError == ERROR_FILE_EXISTS
                                                 ? target : source);
#else
    if (stampOf(target).exists) {
        return core::errorFromErrno(EEXIST, target);
    }
    if (rename(QFile::encodeName(source).constData(), QFile::encodeName(target).constData()) == 0) {
        return {};
    }
    return core::errorFromErrno(errno, source);
#endif
}

QString partialSuffix()
{
    return QStringLiteral(".eu-parcial");
}

core::Status copyFile(const QString &source, const QString &target, const tasks::CancelToken &cancel,
                      const CopyProgress &progress)
{
    if (stampOf(target).exists) {
        return core::errorFromErrno(EEXIST, target);
    }

    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) {
        return fromQFile(input, source);
    }

    const QString partialPath = target + partialSuffix();
    QFile::remove(partialPath);   // resto de una copia anterior interrumpida
    QFile output(partialPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return fromQFile(output, partialPath);
    }

    // Si algo sale mal a partir de aquí, el temporal no debe quedar en el disco.
    const auto fail = [&](core::Error error) -> core::Status {
        output.close();
        QFile::remove(partialPath);
        return error;
    };

    std::vector<char> buffer(1024 * 1024);
    while (true) {
        if (cancel.isCancelled()) {
            return fail(core::Error{core::ErrorCode::Cancelled,
                                    QCoreApplication::translate("eu::ops", "Copia cancelada."), source, 0});
        }
        const qint64 read = input.read(buffer.data(), qint64(buffer.size()));
        if (read == 0) {
            break;
        }
        if (read < 0) {
            return fail(fromQFile(input, source));
        }
        if (output.write(buffer.data(), read) != read) {
            return fail(fromQFile(output, partialPath));
        }
        if (progress) {
            progress(read);
        }
    }

    // Los datos deben estar en el disco antes de darle el nombre definitivo.
    if (!syncToDisk(output)) {
        return fail(lastSystemError(partialPath));
    }
    output.close();

    // Se conservan permisos y fecha de modificación del original. La fecha
    // sirve además para reconocer la copia al deshacer.
    QFile::setPermissions(partialPath, input.permissions());
    QFile stamped(partialPath);
    if (stamped.open(QIODevice::ReadWrite)) {
        // La fecha se lee y se escribe en UTC. Con la hora local, en Windows y en
        // zonas que tuvieron horario de verano y ya no (México), cada conversión
        // movía una hora las fechas de aquellos veranos.
        stamped.setFileTime(QFileInfo(source).lastModified(QTimeZone::UTC), QFileDevice::FileModificationTime);
        stamped.close();
    }

    const core::Status renamed = renameNoReplace(partialPath, target);
    if (!renamed.ok()) {
        QFile::remove(partialPath);
        return renamed;
    }
    return {};
}

core::Status copySymlink(const QString &source, const QString &target)
{
    std::error_code code;
    std::filesystem::copy_symlink(toStdPath(source), toStdPath(target), code);
    if (code) {
        return fromStdError(code, target);
    }
    return {};
}

core::Status removeFile(const QString &path)
{
    const FileStamp stamp = stampOf(path);
    if (!stamp.exists) {
        return core::errorFromErrno(ENOENT, path);
    }
    if (stamp.isDirectory) {
        // Protección: esta función nunca borra carpetas.
        return core::errorFromErrno(EISDIR, path);
    }
    std::error_code code;
    std::filesystem::remove(toStdPath(path), code);
    if (code) {
        return fromStdError(code, path);
    }
    return {};
}

core::Status removeEmptyDirectory(const QString &path)
{
    const FileStamp stamp = stampOf(path);
    if (!stamp.exists) {
        return core::errorFromErrno(ENOENT, path);
    }
    if (!stamp.isDirectory) {
        return core::errorFromErrno(ENOTDIR, path);
    }
    std::error_code code;
    std::filesystem::remove(toStdPath(path), code);   // remove() no borra carpetas con contenido
    if (code) {
        return fromStdError(code, path);
    }
    return {};
}

QString uniqueName(const QString &directory, const QString &name)
{
    const QDir dir(directory);
    if (!stampOf(dir.filePath(name)).exists) {
        return name;
    }
    // La extensión se conserva: "foto.jpg" -> "foto (2).jpg". Para nombres
    // ocultos como ".bashrc" no hay extensión que separar.
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    const bool hasSuffix = dot > 0;
    const QString stem = hasSuffix ? name.left(dot) : name;
    const QString suffix = hasSuffix ? name.mid(dot) : QString();
    for (int number = 2;; ++number) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(stem).arg(number).arg(suffix);
        if (!stampOf(dir.filePath(candidate)).exists) {
            return candidate;
        }
    }
}

} // namespace eu::ops
