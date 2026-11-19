// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/archive/Archive.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QScopeGuard>
#include <QSet>

#ifdef EU_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

#include <memory>

namespace eu::archive {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::archive", text);
}

[[maybe_unused]] core::Error notAvailable(const QString &path)
{
    return core::Error{core::ErrorCode::NotSupported, tr("Esta compilación no maneja archivos comprimidos"), path, 0};
}

core::Error cancelled(const QString &path)
{
    return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), path, 0};
}

/// Extensiones de los comprimidos de un solo archivo (sin TAR dentro).
bool isSingleFileCompression(const QString &lowerName)
{
    for (const char *suffix : {".gz", ".bz2", ".xz", ".zst", ".lz", ".lzma"}) {
        const QString text = QString::fromLatin1(suffix);
        if (lowerName.endsWith(text) && !lowerName.endsWith(QStringLiteral(".tar") + text)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool isArchiveName(const QString &fileName)
{
    const QString name = fileName.toLower();
    static const QStringList suffixes{
        QStringLiteral(".zip"),    QStringLiteral(".7z"),      QStringLiteral(".rar"),   QStringLiteral(".tar"),
        QStringLiteral(".tar.gz"), QStringLiteral(".tgz"),     QStringLiteral(".tar.bz2"), QStringLiteral(".tbz2"),
        QStringLiteral(".tar.xz"), QStringLiteral(".txz"),     QStringLiteral(".tar.zst"), QStringLiteral(".gz"),
        QStringLiteral(".bz2"),    QStringLiteral(".xz"),      QStringLiteral(".iso"),   QStringLiteral(".cab"),
        QStringLiteral(".cpio"),   QStringLiteral(".jar"),     QStringLiteral(".apk"),   QStringLiteral(".cbz"),
        QStringLiteral(".cbr"),    QStringLiteral(".cb7"),
    };
    for (const QString &suffix : suffixes) {
        if (name.endsWith(suffix) && name.size() > suffix.size()) {
            return true;
        }
    }
    return false;
}

QString extensionOf(Format format)
{
    switch (format) {
    case Format::Zip:      return QStringLiteral(".zip");
    case Format::SevenZip: return QStringLiteral(".7z");
    case Format::Tar:      return QStringLiteral(".tar");
    case Format::TarGz:    return QStringLiteral(".tar.gz");
    }
    return QString();
}

QString safeRelativePath(const QString &insidePath)
{
    QString path = insidePath;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));   // un ZIP hecho en Windows puede traer "\"
    if (path.startsWith(QLatin1Char('/'))) {
        return {};   // ruta absoluta
    }
    if (path.size() >= 2 && path[1] == QLatin1Char(':')) {
        return {};   // "C:…": letra de unidad
    }
    QStringList parts;
    const QStringList pieces = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &piece : pieces) {
        if (piece == QLatin1String(".")) {
            continue;
        }
        if (piece == QLatin1String("..")) {
            return {};   // nunca se sube, aunque se quedara dentro
        }
        parts.append(piece);
    }
    return parts.join(QLatin1Char('/'));
}

core::Result<QString> extractEntryToTemp(const QString &archivePath, const QString &inner,
                                         const tasks::CancelToken &cancel)
{
    const QFileInfo info(archivePath);
    const QString key = archivePath + QLatin1Char('\n') + QString::number(info.size()) + QLatin1Char('\n')
        + QString::number(info.lastModified().toMSecsSinceEpoch()) + QLatin1Char('\n') + inner;
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/explorador-ultra-comprimidos/")
        + QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    const QString safe = safeRelativePath(inner);
    if (safe.isEmpty()) {
        return core::Error{core::ErrorCode::InvalidName, tr("Ruta no válida dentro del comprimido"), inner, 0};
    }
    // Solo el nombre del archivo, sin sus carpetas de dentro.
    const QString target = folder + QLatin1Char('/') + safe.section(QLatin1Char('/'), -1);
    if (QFileInfo(target).isFile()) {
        return target;
    }
    QDir().mkpath(folder);
    ExtractOptions options;
    options.only = QStringList{inner};
    const qsizetype slash = inner.lastIndexOf(QLatin1Char('/'));
    if (slash > 0) {
        options.stripPrefix = inner.left(slash);
    }
    const core::Result<ExtractResult> extracted = extractArchive(archivePath, folder, options, cancel);
    if (!extracted.ok()) {
        return extracted.error();
    }
    if (!QFileInfo(target).isFile()) {
        return core::Error{core::ErrorCode::NotFound,
                           extracted.value().skipped.isEmpty() ? tr("No está en el comprimido")
                                                               : extracted.value().skipped.first(),
                           inner, 0};
    }
    return target;
}

#ifndef EU_HAVE_LIBARCHIVE

bool isAvailable()
{
    return false;
}

core::Result<QList<ArchiveEntry>> listArchive(const QString &archivePath, const tasks::CancelToken &, const Limits &)
{
    return notAvailable(archivePath);
}

core::Result<ExtractResult> extractArchive(const QString &archivePath, const QString &, const ExtractOptions &,
                                           const tasks::CancelToken &, const ProgressCallback &)
{
    return notAvailable(archivePath);
}

core::Status createArchive(const QString &outputPath, Format, const QStringList &, const tasks::CancelToken &,
                           const ProgressCallback &)
{
    return notAvailable(outputPath);
}

#else

namespace {

/// Tipo de una entrada (AE_IFREG, AE_IFDIR…). Windows no tiene mode_t: libarchive
/// usa allí otro tipo entero, así que se toma el que devuelve la propia función.
using EntryType = decltype(archive_entry_filetype(nullptr));

struct ReadDeleter
{
    void operator()(struct archive *handle) const { archive_read_free(handle); }
};
struct WriteDeleter
{
    void operator()(struct archive *handle) const { archive_write_free(handle); }
};
using ReadHandle = std::unique_ptr<struct archive, ReadDeleter>;
using WriteHandle = std::unique_ptr<struct archive, WriteDeleter>;

core::Error archiveError(struct archive *handle, const QString &path, core::ErrorCode code = core::ErrorCode::Corrupted)
{
    const char *text = archive_error_string(handle);
    return core::Error{code, text != nullptr ? QString::fromUtf8(text) : tr("Archivo comprimido dañado"), path, 0};
}

/// Abre el comprimido para leer, con todos los formatos y filtros. Los
/// comprimidos de un solo archivo (.gz sin TAR dentro) se leen como "raw".
ReadHandle openForReading(const QString &archivePath, core::Error *error)
{
    ReadHandle handle(archive_read_new());
    archive_read_support_filter_all(handle.get());
    archive_read_support_format_all(handle.get());
    if (isSingleFileCompression(archivePath.toLower())) {
        archive_read_support_format_raw(handle.get());
    }
#ifdef Q_OS_WIN
    const int opened = archive_read_open_filename_w(handle.get(),
                                                    reinterpret_cast<const wchar_t *>(archivePath.utf16()), 64 * 1024);
#else
    const int opened = archive_read_open_filename(handle.get(), QFile::encodeName(archivePath).constData(), 64 * 1024);
#endif
    if (opened != ARCHIVE_OK) {
        *error = archiveError(handle.get(), archivePath);
        return nullptr;
    }
    return handle;
}

/// La ruta de una entrada tal como viene, con "/" y sin "./" ni "/" final.
QString rawPathOf(struct archive_entry *entry, const QString &archivePath)
{
    // En Windows primero la versión ancha: con el locale "C" de un programa,
    // libarchive no sabe pasar a 8 bits lo que no es ASCII.
    QString path;
#ifdef Q_OS_WIN
    if (const wchar_t *wide = archive_entry_pathname_w(entry)) {
        path = QString::fromWCharArray(wide);
    } else
#endif
    if (const char *utf8 = archive_entry_pathname_utf8(entry)) {
        path = QString::fromUtf8(utf8);
    } else if (const char *local = archive_entry_pathname(entry)) {
        path = QString::fromLocal8Bit(local);
    }
    // El formato "raw" (un .gz suelto) llama "data" a su único contenido.
    if (path == QLatin1String("data") && isSingleFileCompression(archivePath.toLower())) {
        path = QFileInfo(archivePath).completeBaseName();
    }
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.startsWith(QLatin1String("./"))) {
        path.remove(0, 2);
    }
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    return path;
}

QString linkTargetOf(struct archive_entry *entry)
{
#ifdef Q_OS_WIN
    if (const wchar_t *wide = archive_entry_symlink_w(entry)) {
        return QString::fromWCharArray(wide);
    }
#endif
    if (const char *utf8 = archive_entry_symlink_utf8(entry)) {
        return QString::fromUtf8(utf8);
    }
    if (const char *local = archive_entry_symlink(entry)) {
        return QString::fromLocal8Bit(local);
    }
    return {};
}

qint64 modifiedMsOf(struct archive_entry *entry)
{
    return archive_entry_mtime_is_set(entry)
        ? qint64(archive_entry_mtime(entry)) * 1000 + archive_entry_mtime_nsec(entry) / 1000000
        : 0;
}

/// `path` está dentro de `root` (o es `root`), comparando rutas ya limpias.
bool isInside(const QString &path, const QString &root)
{
    const QString cleanPath = QDir::cleanPath(path);
    const QString cleanRoot = QDir::cleanPath(root);
    return cleanPath == cleanRoot || cleanPath.startsWith(cleanRoot + QLatin1Char('/'));
}

/// Alguna carpeta entre `root` y `path` es un enlace: escribir a través de
/// ella podría acabar fuera del destino.
bool passesThroughLink(const QString &path, const QString &root)
{
    QString current = QFileInfo(path).path();
    const QString cleanRoot = QDir::cleanPath(root);
    while (current.size() > cleanRoot.size()) {
        if (QFileInfo(current).isSymLink()) {
            return true;
        }
        current = QFileInfo(current).path();
    }
    return false;
}

} // namespace

bool isAvailable()
{
    return true;
}

core::Result<QList<ArchiveEntry>> listArchive(const QString &archivePath, const tasks::CancelToken &cancel,
                                              const Limits &limits)
{
    core::Error error;
    ReadHandle handle = openForReading(archivePath, &error);
    if (!handle) {
        return error;
    }
    QList<ArchiveEntry> entries;
    QSet<QString> seen;
    QSet<QString> folders;
    struct archive_entry *entry = nullptr;
    while (true) {
        const int status = archive_read_next_header(handle.get(), &entry);
        if (status == ARCHIVE_EOF) {
            break;
        }
        if (status == ARCHIVE_FATAL) {
            return archiveError(handle.get(), archivePath);
        }
        if (cancel.isCancelled()) {
            return cancelled(archivePath);
        }
        if (qint64(entries.size()) >= limits.maxEntries) {
            return core::Error{core::ErrorCode::NotSupported,
                               tr("El comprimido tiene más de %1 elementos").arg(limits.maxEntries), archivePath, 0};
        }
        ArchiveEntry item;
        item.path = rawPathOf(entry, archivePath);
        if (item.path.isEmpty()) {
            archive_read_data_skip(handle.get());
            continue;
        }
        const EntryType type = archive_entry_filetype(entry);
        item.isDirectory = type == AE_IFDIR;
        item.isSymlink = type == AE_IFLNK;
        item.linkTarget = item.isSymlink ? linkTargetOf(entry) : QString();
        item.size = item.isDirectory ? 0 : qint64(archive_entry_size(entry));
        item.modifiedMs = modifiedMsOf(entry);
        item.encrypted = archive_entry_is_encrypted(entry) != 0;
        // Las carpetas que contienen a esta, aunque el comprimido no las liste.
        for (qsizetype slash = item.path.indexOf(QLatin1Char('/')); slash > 0;
             slash = item.path.indexOf(QLatin1Char('/'), slash + 1)) {
            folders.insert(item.path.left(slash));
        }
        if (!seen.contains(item.path)) {
            seen.insert(item.path);
            entries.append(item);
        }
        archive_read_data_skip(handle.get());
    }
    for (const QString &folder : std::as_const(folders)) {
        if (!seen.contains(folder)) {
            ArchiveEntry implicit;
            implicit.path = folder;
            implicit.isDirectory = true;
            entries.append(implicit);
        }
    }
    return entries;
}

core::Result<ExtractResult> extractArchive(const QString &archivePath, const QString &destination,
                                           const ExtractOptions &options, const tasks::CancelToken &cancel,
                                           const ProgressCallback &progress)
{
    const QString root = QDir::cleanPath(QFileInfo(destination).absoluteFilePath());
    if (!QFileInfo(root).isDir()) {
        return core::Error{core::ErrorCode::NotFound, tr("La carpeta de destino no existe"), root, 0};
    }
    core::Error error;
    ReadHandle reader = openForReading(archivePath, &error);
    if (!reader) {
        return error;
    }
    WriteHandle writer(archive_write_disk_new());
    // Defensas propias de libarchive, además de las de aquí: no escribir a
    // través de enlaces, no aceptar "..", no sobrescribir. Sin permisos ni
    // dueño del comprimido: lo extraído es del usuario, con sus permisos.
    archive_write_disk_set_options(writer.get(), ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_SYMLINKS
                                                     | ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_NO_OVERWRITE);
    archive_write_disk_set_standard_lookup(writer.get());

    const qint64 archiveSize = std::max<qint64>(1, QFileInfo(archivePath).size());
    ExtractResult result;
    qint64 entriesSeen = 0;
    const auto wanted = [&](const QString &path) {
        if (options.only.isEmpty()) {
            return true;
        }
        for (const QString &only : options.only) {
            if (path == only || path.startsWith(only + QLatin1Char('/'))) {
                return true;
            }
        }
        return false;
    };
    const auto skip = [&](const QString &path, const QString &why) {
        result.skipped.append(QStringLiteral("%1: %2").arg(path, why));
    };

    struct archive_entry *entry = nullptr;
    while (true) {
        const int status = archive_read_next_header(reader.get(), &entry);
        if (status == ARCHIVE_EOF) {
            break;
        }
        if (status == ARCHIVE_FATAL) {
            return archiveError(reader.get(), archivePath);
        }
        if (cancel.isCancelled()) {
            return cancelled(archivePath);
        }
        if (++entriesSeen > options.limits.maxEntries) {
            return core::Error{core::ErrorCode::NotSupported,
                               tr("El comprimido tiene más de %1 elementos: se detuvo la extracción")
                                   .arg(options.limits.maxEntries),
                               archivePath, 0};
        }
        const QString raw = rawPathOf(entry, archivePath);
        if (raw.isEmpty() || !wanted(raw)) {
            continue;
        }
        QString relative = raw;
        if (!options.stripPrefix.isEmpty()) {
            if (!raw.startsWith(options.stripPrefix + QLatin1Char('/'))) {
                continue;   // la propia carpeta que se quita, o algo de fuera de ella
            }
            relative = raw.mid(options.stripPrefix.size() + 1);
        }
        const QString safe = safeRelativePath(relative);
        if (safe.isEmpty()) {
            skip(raw, tr("su ruta sale de la carpeta de destino"));
            continue;
        }
        const QString target = root + QLatin1Char('/') + safe;
        if (!isInside(target, root) || passesThroughLink(target, root)) {
            skip(raw, tr("su ruta sale de la carpeta de destino"));
            continue;
        }
        if (archive_entry_is_encrypted(entry)) {
            skip(raw, tr("está cifrado (necesita contraseña)"));
            continue;
        }
        // Un enlace duro apunta a otra entrada ya extraída: tiene que ser una
        // ruta segura (y entonces cuenta como archivo).
#ifdef Q_OS_WIN
        const wchar_t *hardlink = archive_entry_hardlink_w(entry);
        const QString hardlinkName = hardlink != nullptr ? QString::fromWCharArray(hardlink) : QString();
#else
        const char *hardlink = archive_entry_hardlink(entry);
        const QString hardlinkName = hardlink != nullptr ? QString::fromLocal8Bit(hardlink) : QString();
#endif
        if (hardlink != nullptr) {
            const QString linked = safeRelativePath(hardlinkName);
            if (linked.isEmpty()) {
                skip(raw, tr("es un enlace que apunta fuera de la carpeta de destino"));
                continue;
            }
#ifdef Q_OS_WIN
            archive_entry_copy_hardlink_w(
                entry, reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(root + QLatin1Char('/') + linked).utf16()));
#else
            const QByteArray absolute = QFile::encodeName(root + QLatin1Char('/') + linked);
            archive_entry_copy_hardlink(entry, absolute.constData());
#endif
        }
        const EntryType type = hardlink != nullptr ? EntryType(AE_IFREG) : archive_entry_filetype(entry);
        if (type != AE_IFREG && type != AE_IFDIR && type != AE_IFLNK) {
            skip(raw, tr("no es un archivo, carpeta ni enlace"));
            continue;
        }
        const QFileInfo existing(target);
        if (type == AE_IFDIR && existing.isDir() && !existing.isSymLink()) {
            continue;   // la carpeta ya está: se sigue con lo de dentro
        }
        if (existing.exists() || existing.isSymLink()) {
            skip(raw, tr("ya existe en el destino (no se sobrescribe)"));
            continue;
        }
        if (type == AE_IFLNK) {
            // Un enlace solo si apunta a algo de dentro del destino.
            const QString link = linkTargetOf(entry);
            const QString resolved = QDir::cleanPath(QFileInfo(target).path() + QLatin1Char('/') + link);
            if (link.isEmpty() || QDir::isAbsolutePath(link) || link.contains(QLatin1Char('\\'))
                || !isInside(resolved, root)) {
                skip(raw, tr("es un enlace que apunta fuera de la carpeta de destino"));
                continue;
            }
        }
        // libarchive escribe las rutas relativas respecto de la carpeta de
        // trabajo del proceso: se le da la ruta absoluta ya comprobada.
#ifdef Q_OS_WIN
        archive_entry_copy_pathname_w(entry, reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(target).utf16()));
#else
        archive_entry_copy_pathname(entry, QFile::encodeName(target).constData());
#endif
        archive_entry_set_perm(entry, type == AE_IFDIR ? 0755 : 0644);
        if (archive_write_header(writer.get(), entry) != ARCHIVE_OK) {
            skip(raw, archiveError(writer.get(), target).message);
            continue;
        }
        if (type == AE_IFREG) {
            const void *block = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;
            core::Error stop;
            bool failed = false;
            while (true) {
                const int read = archive_read_data_block(reader.get(), &block, &size, &offset);
                if (read == ARCHIVE_EOF) {
                    break;
                }
                if (read < ARCHIVE_WARN) {
                    stop = archiveError(reader.get(), archivePath);
                    failed = true;
                    break;
                }
                result.bytes += qint64(size);
                // Topes contra bombas: tamaño total y proporción.
                if (result.bytes > options.limits.maxTotalBytes) {
                    stop = core::Error{core::ErrorCode::NoSpace,
                                       tr("Lo extraído pasaría de %1 bytes: se detuvo (posible bomba de descompresión)")
                                           .arg(options.limits.maxTotalBytes),
                                       archivePath, 0};
                    failed = true;
                    break;
                }
                if (result.bytes > options.limits.ratioCheckFromBytes
                    && result.bytes / archiveSize > options.limits.maxRatio) {
                    stop = core::Error{core::ErrorCode::NoSpace,
                                       tr("Lo extraído es más de %1 veces mayor que el comprimido: se detuvo "
                                          "(posible bomba de descompresión)")
                                           .arg(options.limits.maxRatio),
                                       archivePath, 0};
                    failed = true;
                    break;
                }
                if (cancel.isCancelled()) {
                    stop = cancelled(archivePath);
                    failed = true;
                    break;
                }
                cancel.yieldToInteractive();
                if (archive_write_data_block(writer.get(), block, size, offset) < ARCHIVE_OK) {
                    stop = archiveError(writer.get(), target, core::ErrorCode::IoError);
                    failed = true;
                    break;
                }
                if (progress) {
                    progress(result.bytes);
                }
            }
            archive_write_finish_entry(writer.get());
            if (failed) {
                QFile::remove(target);   // nada a medias
                return stop;
            }
            ++result.files;
        } else {
            archive_write_finish_entry(writer.get());
            if (type == AE_IFDIR) {
                ++result.folders;
            } else {
                ++result.files;
            }
        }
    }
    archive_write_close(writer.get());
    return result;
}

core::Status createArchive(const QString &outputPath, Format format, const QStringList &sources,
                           const tasks::CancelToken &cancel, const ProgressCallback &progress)
{
    const QString output = QDir::cleanPath(QFileInfo(outputPath).absoluteFilePath());
    if (QFileInfo::exists(output) || QFileInfo(output).isSymLink()) {
        return core::Error{core::ErrorCode::AlreadyExists, tr("Ya existe un archivo con ese nombre"), output, 0};
    }
    // Se escribe en un temporal junto al destino y se renombra al final.
    const QString partial = QFileInfo(output).path() + QStringLiteral("/.") + QFileInfo(output).fileName()
        + QStringLiteral(".parcial");
    QFile::remove(partial);

    WriteHandle writer(archive_write_new());
    switch (format) {
    case Format::Zip:
        archive_write_set_format_zip(writer.get());
        archive_write_set_options(writer.get(), "hdrcharset=UTF-8");   // nombres con acentos
        break;
    case Format::SevenZip:
        archive_write_set_format_7zip(writer.get());
        break;
    case Format::Tar:
        archive_write_set_format_pax_restricted(writer.get());
        break;
    case Format::TarGz:
        archive_write_set_format_pax_restricted(writer.get());
        archive_write_add_filter_gzip(writer.get());
        break;
    }
#ifdef Q_OS_WIN
    const int opened = archive_write_open_filename_w(writer.get(), reinterpret_cast<const wchar_t *>(partial.utf16()));
#else
    const int opened = archive_write_open_filename(writer.get(), QFile::encodeName(partial).constData());
#endif
    if (opened != ARCHIVE_OK) {
        return archiveError(writer.get(), output, core::ErrorCode::IoError);
    }
    ReadHandle disk(archive_read_disk_new());
    archive_read_disk_set_standard_lookup(disk.get());

    qint64 written = 0;
    const auto fail = [&](core::Error error) -> core::Status {
        writer.reset();   // cierra el temporal antes de borrarlo
        QFile::remove(partial);
        return error;
    };
    const auto addOne = [&](const QString &path, const QString &inside) -> core::Status {
        struct archive_entry *entry = archive_entry_new();
        const auto freeEntry = qScopeGuard([entry] { archive_entry_free(entry); });
#ifdef Q_OS_WIN
        archive_entry_copy_sourcepath_w(entry, reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(path).utf16()));
#else
        archive_entry_copy_sourcepath(entry, QFile::encodeName(path).constData());
#endif
        // Datos del archivo (tipo, tamaño, fechas, destino si es enlace), sin seguir enlaces.
        if (archive_read_disk_entry_from_file(disk.get(), entry, -1, nullptr) != ARCHIVE_OK) {
            return archiveError(disk.get(), path, core::ErrorCode::IoError);
        }
#ifdef Q_OS_WIN
        // Ancha: el 7z guarda los nombres en UTF-16 y, desde UTF-8, libarchive
        // los pasaría por la página de códigos (con "?" en lo que no sea ASCII).
        archive_entry_copy_pathname_w(entry, reinterpret_cast<const wchar_t *>(inside.utf16()));
#else
        archive_entry_set_pathname_utf8(entry, inside.toUtf8().constData());
#endif
        archive_entry_set_uname(entry, "");
        archive_entry_set_gname(entry, "");
        if (archive_write_header(writer.get(), entry) < ARCHIVE_WARN) {
            return archiveError(writer.get(), path, core::ErrorCode::IoError);
        }
        if (archive_entry_filetype(entry) == AE_IFREG) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                return core::Error{core::ErrorCode::PermissionDenied, file.errorString(), path, 0};
            }
            QByteArray buffer(256 * 1024, Qt::Uninitialized);
            while (true) {
                cancel.yieldToInteractive();
                if (cancel.isCancelled()) {
                    return cancelled(output);
                }
                const qint64 read = file.read(buffer.data(), buffer.size());
                if (read < 0) {
                    return core::Error{core::ErrorCode::IoError, file.errorString(), path, 0};
                }
                if (read == 0) {
                    break;
                }
                if (archive_write_data(writer.get(), buffer.constData(), size_t(read)) < 0) {
                    return archiveError(writer.get(), output, core::ErrorCode::IoError);
                }
                written += read;
                if (progress) {
                    progress(written);
                }
            }
        }
        return {};
    };

    for (const QString &source : sources) {
        const QFileInfo info(source);
        const QString base = info.fileName();
        if (!info.exists() && !info.isSymLink()) {
            return fail(core::Error{core::ErrorCode::NotFound, tr("No existe"), source, 0});
        }
        core::Status status = addOne(info.absoluteFilePath(), base);
        if (!status.ok()) {
            return fail(status.error());
        }
        if (info.isDir() && !info.isSymLink()) {
            // Sin seguir enlaces a carpetas: se guardan como enlaces.
            QDirIterator it(info.absoluteFilePath(),
                            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                            QDirIterator::Subdirectories);
            const QString prefix = info.absoluteFilePath() + QLatin1Char('/');
            while (it.hasNext()) {
                const QString path = it.next();
                status = addOne(path, base + QLatin1Char('/') + path.mid(prefix.size()));
                if (!status.ok()) {
                    return fail(status.error());
                }
            }
        }
    }
    if (archive_write_close(writer.get()) != ARCHIVE_OK) {
        return fail(archiveError(writer.get(), output, core::ErrorCode::IoError));
    }
    writer.reset();
    // QFile::rename no pisa un archivo existente: si alguien lo creó mientras
    // tanto, falla en vez de sobrescribirlo.
    if (!QFile::rename(partial, output)) {
        QFile::remove(partial);
        return core::Error{core::ErrorCode::AlreadyExists, tr("No se pudo dar el nombre definitivo"), output, 0};
    }
    return {};
}

#endif

} // namespace eu::archive
