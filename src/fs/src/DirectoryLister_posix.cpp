// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
//
// Listado para Linux y macOS: readdir + una consulta de datos por elemento
// relativa al descriptor de la carpeta (statx en Linux, fstatat en macOS).
// Medido en la Fase 0: unas 3,8 veces más rápido que QDirIterator.

#include "BatchSender.h"

#include <QCoreApplication>
#include <QFile>

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef Q_OS_LINUX
#include <sys/sysmacros.h>
#endif

namespace eu::fs {

namespace {

/// Cierra la carpeta al salir del ámbito, pase lo que pase (RAII).
struct DirCloser
{
    DIR *dir;
    ~DirCloser() { closedir(dir); }
};

bool isDotOrDotDot(const char *name)
{
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

/// Rellena los datos de `entry` consultando al sistema. Devuelve false si falla.
bool fillFromSystem(int directoryFd, const char *name, Entry &entry)
{
#ifdef Q_OS_LINUX
    // statx da además la fecha de creación cuando el sistema de archivos la guarda.
    struct statx info {};
    if (statx(directoryFd, name, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT,
              STATX_BASIC_STATS | STATX_BTIME, &info) != 0) {
        return false;
    }
    entry.size = qint64(info.stx_size);
    entry.modifiedMs = qint64(info.stx_mtime.tv_sec) * 1000 + info.stx_mtime.tv_nsec / 1'000'000;
    if (info.stx_mask & STATX_BTIME) {
        entry.createdMs = qint64(info.stx_btime.tv_sec) * 1000 + info.stx_btime.tv_nsec / 1'000'000;
    }
    entry.fileId = info.stx_ino;
    entry.deviceId = makedev(info.stx_dev_major, info.stx_dev_minor);
    entry.isDirectory = S_ISDIR(info.stx_mode);
    entry.isSymlink = S_ISLNK(info.stx_mode);
#else
    struct stat info {};
    if (fstatat(directoryFd, name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        return false;
    }
    entry.size = qint64(info.st_size);
    entry.modifiedMs = qint64(info.st_mtimespec.tv_sec) * 1000 + info.st_mtimespec.tv_nsec / 1'000'000;
    entry.createdMs = qint64(info.st_birthtimespec.tv_sec) * 1000 + info.st_birthtimespec.tv_nsec / 1'000'000;
    entry.fileId = std::uint64_t(info.st_ino);
    entry.deviceId = std::uint64_t(info.st_dev);
    entry.isDirectory = S_ISDIR(info.st_mode);
    entry.isSymlink = S_ISLNK(info.st_mode);
#endif

    if (entry.isSymlink) {
        // Se mira el destino solo para saber si es carpeta (y si existe).
        // Nunca se sigue el enlace para nada más.
        struct stat target {};
        if (fstatat(directoryFd, name, &target, 0) == 0) {
            entry.isDirectory = S_ISDIR(target.st_mode);
        } else {
            entry.isBrokenSymlink = true;
        }
    }
    return true;
}

} // namespace

core::Status listDirectory(const QString &path, const ListOptions &options,
                           const tasks::CancelToken &cancel, const BatchCallback &onBatch)
{
    DIR *dir = opendir(QFile::encodeName(path).constData());
    if (dir == nullptr) {
        return core::errorFromErrno(errno, path);
    }
    const DirCloser closer{dir};
    const int directoryFd = dirfd(dir);

    BatchSender sender(options, onBatch);
    int sinceCancelCheck = 0;

    while (true) {
        if (++sinceCancelCheck >= 256) {
            sinceCancelCheck = 0;
            if (cancel.isCancelled()) {
                return core::Error{core::ErrorCode::Cancelled,
                                   QCoreApplication::translate("eu::fs", "Listado cancelado."), path, 0};
            }
        }

        errno = 0;   // readdir devuelve nullptr tanto al terminar como al fallar
        const dirent *item = readdir(dir);
        if (item == nullptr) {
            if (errno != 0) {
                sender.flush();   // lo leído hasta el fallo se entrega igualmente
                return core::errorFromErrno(errno, path);
            }
            break;
        }
        if (isDotOrDotDot(item->d_name)) {
            continue;
        }

        Entry entry;
        entry.name = QFile::decodeName(item->d_name);
        entry.isHidden = (item->d_name[0] == '.');
        if (!fillFromSystem(directoryFd, item->d_name, entry)) {
            // El elemento existe (apareció en el listado) pero no se dejó leer:
            // se muestra igualmente, marcado, en vez de ocultarlo.
            entry.statFailed = true;
#ifdef DT_DIR
            entry.isDirectory = (item->d_type == DT_DIR);
#endif
        }
        sender.add(std::move(entry));
    }

    sender.flush();
    return {};
}

} // namespace eu::fs
