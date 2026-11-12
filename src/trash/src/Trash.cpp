// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/trash/Trash.h"

#include "eu/fs/DirectoryLister.h"
#include "eu/fs/Volumes.h"
#include "eu/ops/FileOps.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace eu::trash {

namespace {

const QString infoSuffix = QStringLiteral(".info");

/// Crea la carpeta de la papelera si no existe. Devuelve false si no se puede.
bool ensureTrashDirectory(const QString &directory)
{
    if (ops::stampOf(directory).isDirectory) {
        return QFileInfo(directory).isWritable();
    }
    if (!ops::makeDirectory(directory).ok()) {
        return false;
    }
#ifdef Q_OS_WIN
    SetFileAttributesW(QDir::toNativeSeparators(directory).toStdWString().c_str(), FILE_ATTRIBUTE_HIDDEN);
#endif
    return true;
}

core::Error noTrashHere(const QString &path)
{
    return core::Error{core::ErrorCode::NotSupported,
                       QCoreApplication::translate("eu::trash",
                           "Esta unidad no admite papelera: solo se puede eliminar de forma permanente."),
                       path, 0};
}

bool readInfo(const QString &infoPath, const QString &trashDirectory, TrashItem &item)
{
    QFile file(infoPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    item.id = object.value(QLatin1String("id")).toString();
    item.originalPath = object.value(QLatin1String("originalPath")).toString();
    item.deletedMs = qint64(object.value(QLatin1String("deletedMs")).toDouble());
    item.size = qint64(object.value(QLatin1String("size")).toDouble());
    item.isDirectory = object.value(QLatin1String("isDirectory")).toBool();
    item.trashDirectory = trashDirectory;
    // El id debe coincidir con el nombre del archivo: protege de un .info
    // manipulado que apunte fuera de la papelera.
    return !item.originalPath.isEmpty() && item.id + infoSuffix == QFileInfo(infoPath).fileName();
}

} // namespace

QString trashFolderName()
{
    return QStringLiteral(".PapeleraExploradorUltra");
}

QString TrashItem::storedPath() const
{
    return trashDirectory + QLatin1Char('/') + id + QLatin1Char('/') + name();
}

QString TrashItem::infoPath() const
{
    return trashDirectory + QLatin1Char('/') + id + infoSuffix;
}

QString TrashItem::name() const
{
    return QFileInfo(originalPath).fileName();
}

core::Result<QString> trashDirectoryFor(const QString &path)
{
    // Se pregunta por la carpeta que contiene el elemento: si el elemento es
    // un punto de montaje o un enlace, lo que importa es dónde vive su nombre.
    const QString parent = QFileInfo(path).absolutePath();
    const fs::Volume volume = fs::volumeForPath(parent);
    if (volume.rootPath.isEmpty()) {
        return noTrashHere(path);
    }

    if (!volume.readOnly) {
        const QString atRoot = QDir(volume.rootPath).filePath(trashFolderName());
        if (ensureTrashDirectory(atRoot)) {
            return atRoot;
        }
        // No se puede escribir en la raíz (lo habitual en "/"): se prueba la
        // carpeta personal, pero solo si está en este mismo volumen.
        const QString atHome = QDir::home().filePath(trashFolderName());
        if (fs::isSameVolume(QDir::homePath(), parent) && ensureTrashDirectory(atHome)) {
            return atHome;
        }
    }
    return noTrashHere(path);
}

core::Result<TrashItem> prepareItem(const QString &path)
{
    const ops::FileStamp stamp = ops::stampOf(path);
    if (!stamp.exists) {
        return core::errorFromErrno(ENOENT, path);
    }
    if (isInsideTrash(path)) {
        return core::Error{core::ErrorCode::NotSupported,
                           QCoreApplication::translate("eu::trash", "Este elemento ya está en la papelera."),
                           path, 0};
    }
    const core::Result<QString> directory = trashDirectoryFor(path);
    if (!directory.ok()) {
        return directory.error();
    }

    TrashItem item;
    item.id = QUuid::createUuid().toString(QUuid::Id128);
    item.trashDirectory = directory.value();
    item.originalPath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    item.deletedMs = QDateTime::currentMSecsSinceEpoch();
    item.size = stamp.size > 0 ? stamp.size : 0;
    item.isDirectory = stamp.isDirectory;

    // Primero el .info (escrito de forma atómica y sincronizado): si la
    // aplicación muere justo después, queda un .info sin elemento, que
    // listTrash ignora. Al revés quedaría un elemento sin saber de dónde vino.
    QJsonObject object;
    object.insert(QLatin1String("id"), item.id);
    object.insert(QLatin1String("originalPath"), item.originalPath);
    object.insert(QLatin1String("deletedMs"), double(item.deletedMs));
    object.insert(QLatin1String("size"), double(item.size));
    object.insert(QLatin1String("isDirectory"), item.isDirectory);
    QSaveFile info(item.infoPath());
    if (!info.open(QIODevice::WriteOnly) || info.write(QJsonDocument(object).toJson()) < 0 || !info.commit()) {
        return core::Error{core::ErrorCode::IoError, info.errorString(), item.infoPath(), 0};
    }

    const core::Status holder = ops::makeDirectory(item.trashDirectory + QLatin1Char('/') + item.id);
    if (!holder.ok()) {
        QFile::remove(item.infoPath());
        return holder.error();
    }
    return item;
}

core::Status moveIntoTrash(const TrashItem &item)
{
    return ops::renameNoReplace(item.originalPath, item.storedPath());
}

core::Result<TrashItem> trashPath(const QString &path)
{
    core::Result<TrashItem> item = prepareItem(path);
    if (!item.ok()) {
        return item;
    }
    const core::Status moved = moveIntoTrash(item.value());
    if (!moved.ok()) {
        discardItemRecord(item.value());
        return moved.error();
    }
    return item;
}

void discardItemRecord(const TrashItem &item)
{
    // Solo se borra la carpeta contenedora si está vacía: nunca se pierde un
    // elemento por limpiar su registro.
    ops::removeEmptyDirectory(item.trashDirectory + QLatin1Char('/') + item.id);
    if (!ops::stampOf(item.storedPath()).exists) {
        QFile::remove(item.infoPath());
    }
}

core::Result<QList<TrashItem>> listTrash(const QString &trashDirectory)
{
    const core::Result<std::vector<fs::Entry>> entries = fs::listDirectoryAll(trashDirectory);
    if (!entries.ok()) {
        return entries.error();
    }
    QList<TrashItem> items;
    for (const fs::Entry &entry : entries.value()) {
        if (entry.isDirectory || !entry.name.endsWith(infoSuffix)) {
            continue;
        }
        TrashItem item;
        if (!readInfo(trashDirectory + QLatin1Char('/') + entry.name, trashDirectory, item)) {
            continue;   // .info ilegible o manipulado: se ignora
        }
        if (!ops::stampOf(item.storedPath()).exists) {
            continue;   // registro sin elemento (envío interrumpido)
        }
        items.append(item);
    }
    std::sort(items.begin(), items.end(), [](const TrashItem &a, const TrashItem &b) {
        return a.deletedMs > b.deletedMs;
    });
    return items;
}

core::Status restoreItem(const TrashItem &item, const QString &targetPath)
{
    const QString target = targetPath.isEmpty() ? item.originalPath : targetPath;
    if (!ops::stampOf(QFileInfo(target).absolutePath()).isDirectory) {
        return core::Error{core::ErrorCode::NotFound,
                           QCoreApplication::translate("eu::trash", "La carpeta original ya no existe."),
                           QFileInfo(target).absolutePath(), 0};
    }
    const core::Status moved = ops::renameNoReplace(item.storedPath(), target);
    if (!moved.ok()) {
        return moved;
    }
    discardItemRecord(item);
    return {};
}

core::Status removeTreePermanently(const QString &path, const tasks::CancelToken &cancel)
{
    const ops::FileStamp stamp = ops::stampOf(path);
    if (!stamp.exists) {
        return {};
    }
    if (!stamp.isDirectory) {
        return ops::removeFile(path);   // archivo o enlace: el enlace no se sigue
    }
    const core::Result<std::vector<fs::Entry>> entries = fs::listDirectoryAll(path);
    if (!entries.ok()) {
        return entries.error();
    }
    for (const fs::Entry &entry : entries.value()) {
        if (cancel.isCancelled()) {
            return core::Error{core::ErrorCode::Cancelled,
                               QCoreApplication::translate("eu::trash", "Eliminación cancelada."), path, 0};
        }
        const QString child = path + QLatin1Char('/') + entry.name;
        // Un enlace a carpeta se borra como enlace; solo se entra en carpetas reales.
        const core::Status removed = (entry.isDirectory && !entry.isSymlink)
            ? removeTreePermanently(child, cancel)
            : ops::removeFile(child);
        if (!removed.ok()) {
            return removed;
        }
    }
    return ops::removeEmptyDirectory(path);
}

core::Status purgeItem(const TrashItem &item, const tasks::CancelToken &cancel)
{
    const core::Status removed = removeTreePermanently(item.storedPath(), cancel);
    if (!removed.ok()) {
        return removed;
    }
    discardItemRecord(item);
    return {};
}

QStringList existingTrashDirectories()
{
    QStringList directories;
    const auto addIfExists = [&directories](const QString &candidate) {
        if (!directories.contains(candidate) && ops::stampOf(candidate).isDirectory) {
            directories.append(candidate);
        }
    };
    const QList<fs::Volume> volumes = fs::listVolumes();
    for (const fs::Volume &volume : volumes) {
        addIfExists(QDir(volume.rootPath).filePath(trashFolderName()));
    }
    addIfExists(QDir::home().filePath(trashFolderName()));
    return directories;
}

bool isInsideTrash(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    const QString marker = QLatin1Char('/') + trashFolderName();
    return clean.endsWith(marker) || clean.contains(marker + QLatin1Char('/'));
}

} // namespace eu::trash
