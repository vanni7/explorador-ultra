// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/thumbs/ThumbnailCache.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSaveFile>

#include <algorithm>
#include <vector>

namespace eu::thumbs {

namespace {

/// Lo que cuesta guardar una entrada en memoria. Los fallos ocupan poco pero
/// algo, para que no se acumulen sin límite.
qsizetype costOf(const ThumbnailCache::Entry &entry)
{
    return entry.image.isNull() ? 64 : std::max<qsizetype>(64, entry.image.sizeInBytes());
}

/// QCache cuenta el coste en int (qsizetype en Qt 6): se usa en kB para que
/// un tope de varios GB no desborde.
qsizetype toCacheUnits(qint64 bytes)
{
    return qsizetype(std::max<qint64>(1, bytes / 1024));
}

} // namespace

ThumbnailCache::ThumbnailCache(QString directory, qint64 memoryBudgetBytes) : m_directory(std::move(directory))
{
    m_memory.setMaxCost(toCacheUnits(memoryBudgetBytes));
    if (!m_directory.isEmpty()) {
        QDir().mkpath(m_directory);
    }
}

QString ThumbnailCache::keyOf(const ThumbnailRequest &request)
{
    const QString text = QDir::cleanPath(request.path) + QLatin1Char('\n') + QString::number(request.size)
        + QLatin1Char('\n') + QString::number(request.modifiedMs) + QLatin1Char('\n')
        + QString::number(request.maxSide) + (request.isDirectory ? QStringLiteral("\nd") : QString());
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString ThumbnailCache::diskPath(const QString &key, const QString &suffix) const
{
    // Dos niveles de carpetas para no tener cientos de miles de archivos en una.
    return m_directory + QLatin1Char('/') + key.left(2) + QLatin1Char('/') + key + suffix;
}

void ThumbnailCache::insertMemory(const QString &key, const Entry &entry)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_memory.insert(key, new Entry(entry), std::max<qsizetype>(1, costOf(entry) / 1024));
}

ThumbnailCache::Entry ThumbnailCache::lookupMemory(const ThumbnailRequest &request) const
{
    const QString key = keyOf(request);
    const std::lock_guard<std::mutex> lock(m_mutex);
    const Entry *entry = m_memory.object(key);
    return entry != nullptr ? *entry : Entry{};
}

ThumbnailCache::Entry ThumbnailCache::load(const ThumbnailRequest &request)
{
    const QString key = keyOf(request);
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (const Entry *entry = m_memory.object(key)) {
            return *entry;
        }
    }
    if (m_directory.isEmpty()) {
        return {};
    }
    Entry entry;
    if (QFile::exists(diskPath(key, QStringLiteral(".fallo")))) {
        entry.state = State::Failed;
    } else {
        const QString path = diskPath(key, QStringLiteral(".png"));
        QImageReader reader(path, "png");
        const QImage image = reader.read();
        if (image.isNull()) {
            return {};   // no está (o el archivo de la caché se estropeó: se regenera)
        }
        // Al recortar la caché se borra lo que lleva más tiempo sin verse: cada
        // lectura renueva la fecha (como mucho una vez al día, para no escribir
        // en el disco cada vez que se pinta).
        const QDateTime now = QDateTime::currentDateTime();
        if (QFileInfo(path).lastModified().secsTo(now) > 24 * 3600) {
            QFile file(path);
            if (file.open(QIODevice::ReadWrite)) {
                file.setFileTime(now, QFileDevice::FileModificationTime);
            }
        }
        entry.state = State::Ready;
        entry.image = image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied
                                                                    : QImage::Format_RGB32);
    }
    insertMemory(key, entry);
    return entry;
}

void ThumbnailCache::store(const ThumbnailRequest &request, const QImage &image)
{
    const QString key = keyOf(request);
    insertMemory(key, Entry{State::Ready, image});
    if (m_directory.isEmpty()) {
        return;
    }
    const QString path = diskPath(key, QStringLiteral(".png"));
    QDir().mkpath(QFileInfo(path).absolutePath());
    // QSaveFile: o queda entera o no queda; una caché a medio escribir no se lee.
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && image.save(&file, "png", 70)) {
        file.commit();
    }
}

void ThumbnailCache::storeFailure(const ThumbnailRequest &request)
{
    const QString key = keyOf(request);
    insertMemory(key, Entry{State::Failed, {}});
    if (m_directory.isEmpty()) {
        return;
    }
    const QString path = diskPath(key, QStringLiteral(".fallo"));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.close();
    }
}

void ThumbnailCache::setMemoryBudget(qint64 bytes)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_memory.setMaxCost(toCacheUnits(bytes));
}

qint64 ThumbnailCache::memoryBudget() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return qint64(m_memory.maxCost()) * 1024;
}

qint64 ThumbnailCache::memoryUsed() const
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return qint64(m_memory.totalCost()) * 1024;
}

void ThumbnailCache::clearMemory()
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_memory.clear();
}

int ThumbnailCache::pruneDisk(qint64 maxBytes)
{
    if (m_directory.isEmpty()) {
        return 0;
    }
    struct File
    {
        QString path;
        qint64 size;
        qint64 usedMs;
    };
    std::vector<File> files;
    qint64 total = 0;
    QDirIterator it(m_directory, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        files.push_back({info.filePath(), info.size(), info.lastModified().toMSecsSinceEpoch()});
        total += info.size();
    }
    if (total <= maxBytes) {
        return 0;
    }
    std::sort(files.begin(), files.end(), [](const File &a, const File &b) { return a.usedMs < b.usedMs; });
    int removed = 0;
    for (const File &file : files) {
        if (total <= maxBytes) {
            break;
        }
        if (QFile::remove(file.path)) {
            total -= file.size;
            ++removed;
        }
    }
    return removed;
}

} // namespace eu::thumbs
