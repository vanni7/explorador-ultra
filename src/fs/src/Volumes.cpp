// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/fs/Volumes.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStorageInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace eu::fs {

namespace {

/// Sistemas de archivos que no son discos del usuario.
bool isInternalFileSystem(const QString &type)
{
    static const QSet<QString> internal{
        QStringLiteral("proc"), QStringLiteral("sysfs"), QStringLiteral("devtmpfs"), QStringLiteral("devpts"),
        QStringLiteral("tmpfs"), QStringLiteral("cgroup"), QStringLiteral("cgroup2"), QStringLiteral("securityfs"),
        QStringLiteral("pstore"), QStringLiteral("bpf"), QStringLiteral("debugfs"), QStringLiteral("tracefs"),
        QStringLiteral("configfs"), QStringLiteral("fusectl"), QStringLiteral("mqueue"), QStringLiteral("hugetlbfs"),
        QStringLiteral("autofs"), QStringLiteral("binfmt_misc"), QStringLiteral("squashfs"), QStringLiteral("overlay"),
        QStringLiteral("efivarfs"), QStringLiteral("ramfs"), QStringLiteral("nsfs"), QStringLiteral("rpc_pipefs"),
        QStringLiteral("fuse.portal"), QStringLiteral("fuse.gvfsd-fuse"), QStringLiteral("fuse.snapfuse"),
        QStringLiteral("devfs"),
    };
    return internal.contains(type);
}

bool isNetworkFileSystem(const QString &type)
{
    static const QSet<QString> network{
        QStringLiteral("nfs"), QStringLiteral("nfs4"), QStringLiteral("cifs"), QStringLiteral("smb3"),
        QStringLiteral("smbfs"), QStringLiteral("afpfs"), QStringLiteral("webdav"), QStringLiteral("fuse.sshfs"),
        QStringLiteral("fuse.rclone"), QStringLiteral("9p"),
    };
    return network.contains(type);
}

#ifdef Q_OS_LINUX
/// Lee un archivo pequeño de /sys y devuelve su contenido sin espacios.
QString readSysFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromLatin1(file.readAll()).trimmed() : QString();
}

/// Deduce el tipo de dispositivo a partir de /sys/class/block.
DeviceKind linuxDeviceKind(const QString &device)
{
    // /dev/sda1 -> sda1 ; /sys/class/block/sda1 es un enlace a .../block/sda/sda1
    const QString partition = QFileInfo(device).fileName();
    const QString resolved = QFileInfo(QStringLiteral("/sys/class/block/") + partition).canonicalFilePath();
    if (resolved.isEmpty()) {
        return DeviceKind::Unknown;
    }
    // Si es una partición, los datos del disco están en la carpeta de arriba.
    QString diskPath = resolved;
    if (QFile::exists(resolved + QStringLiteral("/partition"))) {
        diskPath = QFileInfo(resolved).absolutePath();
    }
    if (QFileInfo(diskPath).fileName().startsWith(QLatin1String("sr"))) {
        return DeviceKind::Optical;
    }
    if (QFileInfo(diskPath).fileName().startsWith(QLatin1String("loop"))) {
        return DeviceKind::Virtual;
    }
    // "removable" solo cubre lectores de tarjetas; los discos USB se reconocen
    // porque su ruta en /sys pasa por un controlador usb.
    if (readSysFile(diskPath + QStringLiteral("/removable")) == QLatin1String("1")
        || QFileInfo(diskPath).canonicalFilePath().contains(QLatin1String("/usb"))) {
        return DeviceKind::Removable;
    }
    const QString rotational = readSysFile(diskPath + QStringLiteral("/queue/rotational"));
    if (rotational == QLatin1String("1")) {
        return DeviceKind::Hdd;
    }
    if (rotational == QLatin1String("0")) {
        return DeviceKind::Ssd;
    }
    return DeviceKind::Unknown;
}
#endif

DeviceKind detectKind(const QStorageInfo &storage)
{
    const QString type = QString::fromLatin1(storage.fileSystemType()).toLower();
    if (isNetworkFileSystem(type)) {
        return DeviceKind::Network;
    }
#if defined(Q_OS_LINUX)
    return linuxDeviceKind(QString::fromLocal8Bit(storage.device()));
#elif defined(Q_OS_WIN)
    switch (GetDriveTypeW(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(storage.rootPath()).utf16()))) {
    case DRIVE_REMOVABLE: return DeviceKind::Removable;
    case DRIVE_REMOTE:    return DeviceKind::Network;
    case DRIVE_CDROM:     return DeviceKind::Optical;
    case DRIVE_RAMDISK:   return DeviceKind::Virtual;
    default:              return DeviceKind::Unknown;   // SSD/HDD: se afinará en la Fase 2
    }
#else
    return DeviceKind::Unknown;   // macOS: se afinará en la Fase 2 (IOKit)
#endif
}

QString chooseDisplayName(const QStorageInfo &storage)
{
    if (!storage.name().isEmpty()) {
        return storage.name();
    }
    if (storage.isRoot()) {
        return QCoreApplication::translate("eu::fs", "Sistema");
    }
    const QString folderName = QFileInfo(storage.rootPath()).fileName();
    return folderName.isEmpty() ? QDir::toNativeSeparators(storage.rootPath()) : folderName;
}

Volume toVolume(const QStorageInfo &storage)
{
    Volume volume;
    volume.rootPath = storage.rootPath();
    volume.displayName = chooseDisplayName(storage);
    volume.device = QString::fromLocal8Bit(storage.device());
    volume.fileSystem = QString::fromLatin1(storage.fileSystemType());
    volume.totalBytes = storage.bytesTotal();
    volume.freeBytes = storage.bytesAvailable();
    volume.readOnly = storage.isReadOnly();
    volume.kind = detectKind(storage);
    return volume;
}

bool shouldShow(const QStorageInfo &storage)
{
    if (!storage.isValid() || !storage.isReady()) {
        return false;
    }
    const QString type = QString::fromLatin1(storage.fileSystemType()).toLower();
    if (isInternalFileSystem(type)) {
        return false;
    }
#ifdef Q_OS_UNIX
    const QString root = storage.rootPath();
    // Particiones de arranque y montajes internos del sistema: no son del usuario.
    static const QStringList systemPrefixes{
        QStringLiteral("/boot"), QStringLiteral("/snap/"), QStringLiteral("/var/lib/"),
        QStringLiteral("/System/Volumes/"), QStringLiteral("/private/"),
    };
    for (const QString &prefix : systemPrefixes) {
        if (root.startsWith(prefix)) {
            return false;
        }
    }
    // /run es del sistema, salvo /run/media, donde se montan las unidades extraíbles.
    if (root.startsWith(QLatin1String("/run/")) && !root.startsWith(QLatin1String("/run/media/"))) {
        return false;
    }
#endif
    return true;
}

} // namespace

QList<Volume> listVolumes()
{
    QList<Volume> volumes;
    QSet<QString> seenRoots;
    const QList<QStorageInfo> mounted = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &storage : mounted) {
        if (!shouldShow(storage) || seenRoots.contains(storage.rootPath())) {
            continue;
        }
        seenRoots.insert(storage.rootPath());
        volumes.append(toVolume(storage));
    }
    return volumes;
}

Volume volumeForPath(const QString &path)
{
    const QStorageInfo storage(path);
    if (!storage.isValid()) {
        return {};
    }
    return toVolume(storage);
}

bool isSameVolume(const QString &pathA, const QString &pathB)
{
    const QStorageInfo a(pathA);
    const QStorageInfo b(pathB);
    return a.isValid() && b.isValid() && a.rootPath() == b.rootPath() && a.device() == b.device();
}

VolumeMonitor::VolumeMonitor(QObject *parent)
    : QObject(parent), m_volumes(listVolumes())
{
    m_timer.setInterval(2000);
    connect(&m_timer, &QTimer::timeout, this, &VolumeMonitor::refresh);
    m_timer.start();
}

void VolumeMonitor::setIntervalMs(int milliseconds)
{
    m_timer.setInterval(milliseconds);
}

void VolumeMonitor::refresh()
{
    const QList<Volume> current = listVolumes();
    if (current == m_volumes) {
        return;
    }
    const QList<Volume> previous = m_volumes;
    m_volumes = current;

    for (const Volume &volume : previous) {
        const bool stillThere = std::any_of(current.begin(), current.end(), [&](const Volume &other) {
            return other.rootPath == volume.rootPath && other.device == volume.device;
        });
        if (!stillThere) {
            emit volumeRemoved(volume);
        }
    }
    for (const Volume &volume : current) {
        const bool wasThere = std::any_of(previous.begin(), previous.end(), [&](const Volume &other) {
            return other.rootPath == volume.rootPath && other.device == volume.device;
        });
        if (!wasThere) {
            emit volumeAdded(volume);
        }
    }
    emit volumesChanged(m_volumes);
}

} // namespace eu::fs
