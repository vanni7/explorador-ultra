// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ops/FileOps.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDirIterator>
#include <QTimeZone>

#include <array>
#include <atomic>
#include <optional>
#include <cerrno>
#include <cstring>
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

#ifdef EU_HAVE_LIBCRYPTO
#include <openssl/evp.h>
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

/// Pide al sistema que lo leído o escrito de este archivo no se quede en su
/// caché, para que releerlo vaya de verdad al disco.
void dropFromCache(QFile &file)
{
#if defined(Q_OS_LINUX)
    ::posix_fadvise(file.handle(), 0, 0, POSIX_FADV_DONTNEED);
#elif defined(Q_OS_MACOS)
    ::fcntl(file.handle(), F_NOCACHE, 1);
#else
    Q_UNUSED(file)   // Windows: sin forma sencilla con QFile; se relee de la caché
#endif
}

std::atomic<int> s_corruptCopies{0};

/// Tablas del CRC32 para procesar 8 bytes por vuelta ("slicing-by-8").
const std::array<std::array<quint32, 256>, 8> &crcTables()
{
    static const std::array<std::array<quint32, 256>, 8> tables = [] {
        std::array<std::array<quint32, 256>, 8> values{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
            }
            values[0][i] = crc;
        }
        for (size_t k = 1; k < 8; ++k) {
            for (size_t i = 0; i < 256; ++i) {
                values[k][i] = (values[k - 1][i] >> 8) ^ values[0][values[k - 1][i] & 0xFFu];
            }
        }
        return values;
    }();
    return tables;
}

quint32 load32(const char *data)
{
    const auto *bytes = reinterpret_cast<const quint8 *>(data);
    return quint32(bytes[0]) | (quint32(bytes[1]) << 8) | (quint32(bytes[2]) << 16) | (quint32(bytes[3]) << 24);
}

} // namespace

void corruptNextCopiesForTesting(int times)
{
    s_corruptCopies = times;
}

QString hashAlgorithmName(HashAlgorithm algorithm)
{
    switch (algorithm) {
    case HashAlgorithm::Sha256: return QStringLiteral("SHA-256");
    case HashAlgorithm::Sha1:   return QStringLiteral("SHA-1");
    case HashAlgorithm::Md5:    return QStringLiteral("MD5");
    case HashAlgorithm::Crc32:  return QStringLiteral("CRC32");
    }
    return {};
}

QString hashAlgorithmLimitation(HashAlgorithm algorithm)
{
    switch (algorithm) {
    case HashAlgorithm::Sha256:
        return {};
    case HashAlgorithm::Sha1:
    case HashAlgorithm::Md5:
        return QCoreApplication::translate("eu::ops", "Detecta errores de copia, pero no es seguro frente a "
                                                      "alguien que modifique el archivo a propósito.");
    case HashAlgorithm::Crc32:
        return QCoreApplication::translate("eu::ops", "Solo detecta errores accidentales, y no todos: es el más "
                                                      "rápido y el menos fiable.");
    }
    return {};
}

Hasher::Hasher(HashAlgorithm algorithm) : m_algorithm(algorithm)
{
#ifdef EU_HAVE_LIBCRYPTO
    const EVP_MD *digest = algorithm == HashAlgorithm::Sha256 ? EVP_sha256()
        : algorithm == HashAlgorithm::Sha1                   ? EVP_sha1()
        : algorithm == HashAlgorithm::Md5                    ? EVP_md5()
                                                             : nullptr;
    if (digest != nullptr) {
        EVP_MD_CTX *context = EVP_MD_CTX_new();
        if (context != nullptr && EVP_DigestInit_ex(context, digest, nullptr) == 1) {
            m_openssl = context;
            return;
        }
        EVP_MD_CTX_free(context);   // sin OpenSSL utilizable: el de Qt
    }
#endif
    switch (algorithm) {
    case HashAlgorithm::Sha256: m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256); break;
    case HashAlgorithm::Sha1:   m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha1); break;
    case HashAlgorithm::Md5:    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Md5); break;
    case HashAlgorithm::Crc32:  break;
    }
}

Hasher::~Hasher()
{
#ifdef EU_HAVE_LIBCRYPTO
    EVP_MD_CTX_free(static_cast<EVP_MD_CTX *>(m_openssl));
#endif
}

void Hasher::addData(const char *data, qint64 length)
{
#ifdef EU_HAVE_LIBCRYPTO
    if (m_openssl != nullptr) {
        EVP_DigestUpdate(static_cast<EVP_MD_CTX *>(m_openssl), data, size_t(length));
        return;
    }
#endif
    if (m_hash) {
        m_hash->addData(QByteArrayView(data, length));
        return;
    }
    const auto &t = crcTables();
    quint32 crc = m_crc;
    while (length >= 8) {
        const quint32 one = load32(data) ^ crc;
        const quint32 two = load32(data + 4);
        crc = t[7][one & 0xFFu] ^ t[6][(one >> 8) & 0xFFu] ^ t[5][(one >> 16) & 0xFFu] ^ t[4][one >> 24]
            ^ t[3][two & 0xFFu] ^ t[2][(two >> 8) & 0xFFu] ^ t[1][(two >> 16) & 0xFFu] ^ t[0][two >> 24];
        data += 8;
        length -= 8;
    }
    for (qint64 i = 0; i < length; ++i) {
        crc = t[0][(crc ^ static_cast<quint8>(data[i])) & 0xFFu] ^ (crc >> 8);
    }
    m_crc = crc;
}

QByteArray Hasher::result()
{
#ifdef EU_HAVE_LIBCRYPTO
    if (m_openssl != nullptr) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int size = 0;
        EVP_DigestFinal_ex(static_cast<EVP_MD_CTX *>(m_openssl), digest, &size);
        return QByteArray(reinterpret_cast<const char *>(digest), qsizetype(size));
    }
#endif
    if (m_hash) {
        return m_hash->result();
    }
    const quint32 value = m_crc ^ 0xFFFFFFFFu;
    QByteArray bytes(4, Qt::Uninitialized);
    bytes[0] = char((value >> 24) & 0xFF);
    bytes[1] = char((value >> 16) & 0xFF);
    bytes[2] = char((value >> 8) & 0xFF);
    bytes[3] = char(value & 0xFF);
    return bytes;
}

core::Result<QByteArray> hashFile(const QString &path, HashAlgorithm algorithm, const tasks::CancelToken &cancel)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fromQFile(file, path);
    }
    dropFromCache(file);
    Hasher hasher(algorithm);
    std::vector<char> buffer(1024 * 1024);
    while (true) {
        if (cancel.isCancelled()) {
            return core::Error{core::ErrorCode::Cancelled, QCoreApplication::translate("eu::ops", "Cancelado."), path, 0};
        }
        const qint64 read = file.read(buffer.data(), qint64(buffer.size()));
        if (read < 0) {
            return fromQFile(file, path);
        }
        if (read == 0) {
            return hasher.result();
        }
        hasher.addData(buffer.data(), read);
    }
}

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

QString checkpointSuffix()
{
    return QStringLiteral(".eu-punto");
}

void removePartialsUnder(const QString &target)
{
    const FileStamp stamp = stampOf(target);
    if (stamp.isDirectory && !stamp.isSymlink) {
        QDirIterator it(target, {QStringLiteral("*") + partialSuffix(), QStringLiteral("*") + checkpointSuffix()},
                        QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QFile::remove(it.next());
        }
        return;
    }
    QFile::remove(target + partialSuffix());
    QFile::remove(target + checkpointSuffix());
}

namespace {

core::Status copyFileOnce(const QString &source, const QString &target, const tasks::CancelToken &cancel,
                          const CopyProgress &progress, const CopyOptions &options, bool *verifyFailed);

} // namespace

core::Status copyFile(const QString &source, const QString &target, const tasks::CancelToken &cancel,
                      const CopyProgress &progress, const CopyOptions &options)
{
    bool verifyFailed = false;
    qint64 reported = 0;
    const CopyProgress counting = [&](qint64 bytes) {
        reported += bytes;
        if (progress) {
            progress(bytes);
        }
    };
    core::Status status = copyFileOnce(source, target, cancel, counting, options, &verifyFailed);
    if (verifyFailed) {
        // Un error de lectura o escritura pasajero: se repite una vez. El
        // avance de la primera vuelta se descuenta para no pasar del total.
        if (progress && reported > 0) {
            progress(-reported);
        }
        verifyFailed = false;
        status = copyFileOnce(source, target, cancel, progress, options, &verifyFailed);
    }
    return status;
}

namespace {

core::Status copyFileOnce(const QString &source, const QString &target, const tasks::CancelToken &cancel,
                          const CopyProgress &progress, const CopyOptions &options, bool *verifyFailed)
{
    if (stampOf(target).exists) {
        return core::errorFromErrno(EEXIST, target);
    }

    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) {
        return fromQFile(input, source);
    }

    const QString partialPath = target + partialSuffix();
    const QString checkpointPath = target + checkpointSuffix();
    const qint64 sourceSize = input.size();
    const qint64 sourceMs = QFileInfo(source).lastModified(QTimeZone::UTC).toMSecsSinceEpoch();
    const bool resumable = options.resumable && sourceSize >= kResumableFromBytes;

    // ¿Hay una copia anterior cortada que se pueda seguir? Solo si su punto
    // de control es de este mismo original, sin cambios, y el temporal tiene
    // al menos lo anotado (lo que haya después del punto no se da por bueno).
    qint64 resumeAt = 0;
    if (resumable) {
        QFile checkpoint(checkpointPath);
        if (checkpoint.open(QIODevice::ReadOnly)) {
            const QJsonObject saved = QJsonDocument::fromJson(checkpoint.readAll()).object();
            const qint64 bytes = saved.value(QStringLiteral("bytes")).toInteger();
            if (saved.value(QStringLiteral("origen")).toString() == source
                && saved.value(QStringLiteral("tamano")).toInteger() == sourceSize
                && saved.value(QStringLiteral("fecha")).toInteger() == sourceMs && bytes > 0 && bytes <= sourceSize
                && QFileInfo(partialPath).size() >= bytes) {
                resumeAt = bytes;
            }
        }
    }
    QFile output(partialPath);
    if (resumeAt > 0 && !(output.open(QIODevice::ReadWrite) && output.resize(resumeAt) && output.seek(resumeAt)
                          && input.seek(resumeAt))) {
        output.close();
        resumeAt = 0;
    }
    if (resumeAt == 0) {
        QFile::remove(partialPath);   // resto de una copia anterior que no sirve
        QFile::remove(checkpointPath);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            return fromQFile(output, partialPath);
        }
    }
    qint64 confirmed = resumeAt;   // bytes sincronizados y anotados en el punto de control

    // Si algo sale mal a partir de aquí, el temporal no debe quedar en el
    // disco… salvo en una copia reanudable con algo ya confirmado, si el
    // fallo no es de la copia misma (cancelar, el disco, el cable).
    const auto failKeeping = [&](core::Error error, bool keep) -> core::Status {
        output.close();
        if (!(keep && resumable && confirmed > 0)) {
            QFile::remove(partialPath);
            QFile::remove(checkpointPath);
        }
        return error;
    };
    const auto fail = [&](core::Error error) -> core::Status { return failKeeping(std::move(error), true); };
    const auto saveCheckpoint = [&]() -> bool {
        if (!syncToDisk(output)) {
            return false;
        }
        const QJsonObject saved{{QStringLiteral("origen"), source},
                                {QStringLiteral("tamano"), sourceSize},
                                {QStringLiteral("fecha"), sourceMs},
                                {QStringLiteral("bytes"), output.pos()}};
        QSaveFile file(checkpointPath);
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(saved).toJson(QJsonDocument::Compact)) < 0
            || !file.commit()) {
            return false;
        }
        confirmed = output.pos();
        return true;
    };

    QElapsedTimer sinceCheckpoint;
    sinceCheckpoint.start();
    std::optional<Hasher> sourceHash;
    if (options.verify) {
        sourceHash.emplace(options.algorithm);
    }
    if (resumeAt > 0) {
        // Lo que ya estaba copiado cuenta en el avance y, si se comprueba la
        // copia, en el hash del original (se lee esa parte, no se reescribe).
        if (sourceHash) {
            QFile prefix(source);
            if (!prefix.open(QIODevice::ReadOnly)) {
                return fail(fromQFile(prefix, source));
            }
            std::vector<char> chunk(1024 * 1024);
            qint64 left = resumeAt;
            while (left > 0) {
                const qint64 read = prefix.read(chunk.data(), std::min<qint64>(left, qint64(chunk.size())));
                if (read <= 0) {
                    return fail(fromQFile(prefix, source));
                }
                sourceHash->addData(chunk.data(), read);
                left -= read;
            }
        }
        if (progress) {
            progress(resumeAt);
        }
    }
    std::vector<char> buffer(1024 * 1024);
    while (true) {
        cancel.yieldToInteractive();   // en un disco mecánico, abrir una carpeta pasa delante
        if (options.pausePoint) {
            options.pausePoint(cancel);
        }
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
        if (sourceHash) {
            sourceHash->addData(buffer.data(), read);
        }
        if (s_corruptCopies.load() > 0 && output.pos() == 0) {
            --s_corruptCopies;
            buffer[0] = char(buffer[0] ^ 0x01);   // prueba: un bit cambiado al escribir
        }
        if (output.write(buffer.data(), read) != read) {
            return fail(fromQFile(output, partialPath));
        }
        if (resumable && output.pos() - confirmed >= kCheckpointEveryBytes
            && sinceCheckpoint.elapsed() >= options.checkpointEveryMs) {
            if (!saveCheckpoint()) {
                return fail(lastSystemError(checkpointPath));
            }
            sinceCheckpoint.restart();
        }
        if (progress) {
            progress(read);
        }
    }

    // Los datos deben estar en el disco antes de darle el nombre definitivo.
    if (!syncToDisk(output)) {
        return fail(lastSystemError(partialPath));
    }
    if (options.verify) {
        dropFromCache(output);   // ya está en el disco: releerla debe ir a buscarla allí
    }
    output.close();

    if (sourceHash) {
        const core::Result<QByteArray> written = hashFile(partialPath, options.algorithm, cancel);
        if (!written.ok()) {
            return fail(written.error());
        }
        if (written.value() != sourceHash->result()) {
            *verifyFailed = true;
            return failKeeping(core::Error{core::ErrorCode::Corrupted,
                                    QCoreApplication::translate("eu::ops", "La copia no es igual al original "
                                                                           "(comprobado con %1); no se ha dejado.")
                                        .arg(hashAlgorithmName(options.algorithm)),
                                    target, 0},
                               false);   // una copia mala no se conserva para seguirla
        }
    }

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
        QFile::remove(checkpointPath);
        return renamed;
    }
    QFile::remove(checkpointPath);
    return {};
}

} // namespace

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

core::Result<bool> sameContent(const QString &a, const QString &b, Comparison comparison,
                               const tasks::CancelToken &cancel)
{
    const FileStamp first = stampOf(a);
    const FileStamp second = stampOf(b);
    if (!first.exists || !second.exists) {
        return core::errorFromErrno(ENOENT, first.exists ? b : a);
    }
    if (first.isSymlink || second.isSymlink) {
        return first.isSymlink && second.isSymlink && QFile::symLinkTarget(a) == QFile::symLinkTarget(b);
    }
    if (first.isDirectory || second.isDirectory || first.size != second.size) {
        return false;
    }
    QFile left(a);
    QFile right(b);
    if (!left.open(QIODevice::ReadOnly)) {
        return fromQFile(left, a);
    }
    if (!right.open(QIODevice::ReadOnly)) {
        return fromQFile(right, b);
    }
    const auto cancelled = [&] {
        return core::Error{core::ErrorCode::Cancelled, QCoreApplication::translate("eu::ops", "Cancelado."), a, 0};
    };
    std::vector<char> bufferA(1024 * 1024);
    std::vector<char> bufferB(comparison == Comparison::Bytes ? bufferA.size() : 0);
    if (comparison == Comparison::Bytes) {
        while (true) {
            if (cancel.isCancelled()) {
                return cancelled();
            }
            const qint64 readA = left.read(bufferA.data(), qint64(bufferA.size()));
            const qint64 readB = right.read(bufferB.data(), qint64(bufferB.size()));
            if (readA < 0) {
                return fromQFile(left, a);
            }
            if (readB < 0) {
                return fromQFile(right, b);
            }
            if (readA != readB || std::memcmp(bufferA.data(), bufferB.data(), size_t(readA)) != 0) {
                return false;
            }
            if (readA == 0) {
                return true;
            }
        }
    }
    // Por hash: cada archivo de una pasada (en discos distintos no se alternan lecturas).
    const auto digest = [&](QFile &file, const QString &path) -> core::Result<QByteArray> {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (true) {
            if (cancel.isCancelled()) {
                return cancelled();
            }
            const qint64 read = file.read(bufferA.data(), qint64(bufferA.size()));
            if (read < 0) {
                return fromQFile(file, path);
            }
            if (read == 0) {
                return hash.result();
            }
            hash.addData(QByteArrayView(bufferA.data(), read));
        }
    };
    const core::Result<QByteArray> digestA = digest(left, a);
    if (!digestA.ok()) {
        return digestA.error();
    }
    const core::Result<QByteArray> digestB = digest(right, b);
    if (!digestB.ok()) {
        return digestB.error();
    }
    return digestA.value() == digestB.value();
}

QString nameWithDateTime(const QString &name, qint64 ms)
{
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    const bool hasSuffix = dot > 0;
    const QString stem = hasSuffix ? name.left(dot) : name;
    const QString suffix = hasSuffix ? name.mid(dot) : QString();
    // Con puntos y no dos puntos: ":" no vale en nombres de Windows ni de macOS.
    const QString stamp = QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd HH.mm.ss"));
    return stem + QLatin1Char(' ') + stamp + suffix;
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
