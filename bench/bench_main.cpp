// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
//
// eu-bench: herramienta de medición reproducible.
//
//   eu-bench mktree  <carpeta> <cantidad>   crea <cantidad> archivos vacíos de prueba
//   eu-bench listing <carpeta>              compara formas de listar una carpeta
//   eu-bench sort    <carpeta>              ordenamiento natural y filtrado en memoria
//   eu-bench copy    <origen> <carpetaDestino>   compara formas de copiar un archivo grande
//
// Cada medición imprime una línea "nombre;milisegundos;detalle" fácil de pegar
// en docs/BENCHMARKS.md. Aquí nunca se escribe un valor que no se haya medido.

#include "eu/core/NaturalCompare.h"
#include "eu/fs/DirectoryLister.h"
#include "eu/fs/Volumes.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <system_error>
#include <vector>

#ifdef Q_OS_UNIX
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

void report(const QString &name, qint64 nanoseconds, const QString &detail)
{
    out() << name << ';' << QString::number(double(nanoseconds) / 1e6, 'f', 2) << ';' << detail << '\n';
    out().flush();
}

/// Memoria residente del proceso en MB (solo Linux; en otros sistemas devuelve -1).
double residentMemoryMb()
{
#ifdef Q_OS_LINUX
    QFile statm(QStringLiteral("/proc/self/statm"));
    if (statm.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> fields = statm.readAll().split(' ');
        if (fields.size() > 1) {
            return fields[1].toDouble() * double(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
        }
    }
#endif
    return -1.0;
}

/// Un elemento listado con los datos que muestra la vista detallada.
struct Entry
{
    QString name;
    qint64 size = 0;
    qint64 modifiedSecs = 0;
    bool isDir = false;
};

int commandMktree(const QString &folder, int count)
{
    QDir().mkpath(folder);
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < count; ++i) {
        // Nombres variados: con números (orden natural) y con caracteres no ASCII.
        const QString name = (i % 10 == 0)
            ? QStringLiteral("%1/ñandú_fotografía %2.jpg").arg(folder).arg(i)
            : QStringLiteral("%1/archivo%2.txt").arg(folder).arg(i);
        QFile file(name);
        if (!file.open(QIODevice::WriteOnly)) {
            out() << "ERROR: no se pudo crear " << name << '\n';
            return 1;
        }
    }
    report(QStringLiteral("mktree"), timer.nsecsElapsed(), QStringLiteral("%1 archivos").arg(count));
    return 0;
}

/// Ejecuta `listFunction`, que debe llamar a `onEntry` por cada elemento, y
/// mide el tiempo hasta los primeros 200 elementos y hasta el final.
void measureListing(const QString &name,
                    const std::function<void(const std::function<void(Entry &&)> &)> &listFunction)
{
    std::vector<Entry> entries;
    const double memoryBefore = residentMemoryMb();
    qint64 firstBatchNs = -1;
    QElapsedTimer timer;
    timer.start();
    listFunction([&](Entry &&entry) {
        entries.push_back(std::move(entry));
        if (entries.size() == 200) {
            firstBatchNs = timer.nsecsElapsed();
        }
    });
    const qint64 totalNs = timer.nsecsElapsed();
    const double memoryAfter = residentMemoryMb();
    report(name, totalNs,
           QStringLiteral("%1 elementos; primeros 200 en %2 ms; +%3 MB RSS")
               .arg(entries.size())
               .arg(QString::number(double(firstBatchNs) / 1e6, 'f', 2))
               .arg(QString::number(memoryAfter - memoryBefore, 'f', 1)));
}

int commandVolumes()
{
    const QList<eu::fs::Volume> volumes = eu::fs::listVolumes();
    for (const eu::fs::Volume &volume : volumes) {
        out() << volume.rootPath << ';' << volume.displayName << ';' << volume.fileSystem << ";tipo="
              << int(volume.kind) << ";libre=" << volume.freeBytes / 1'000'000'000 << " GB\n";
    }
    return 0;
}

int commandListing(const QString &folder)
{
    // 0) El listador real de la aplicación (módulo fs).
    measureListing(QStringLiteral("eu_fs_listDirectory"), [&](const auto &onEntry) {
        eu::fs::listDirectory(folder, eu::fs::ListOptions(), eu::tasks::CancelToken(),
                              [&](std::vector<eu::fs::Entry> &&batch) {
            for (eu::fs::Entry &entry : batch) {
                onEntry(Entry{std::move(entry.name), entry.size, entry.modifiedMs / 1000, entry.isDirectory});
            }
        });
    });

#ifdef Q_OS_UNIX
    // 1) POSIX: readdir + fstatat. Es el piso de lo que cuesta en Linux/macOS.
    measureListing(QStringLiteral("posix_readdir_fstatat"), [&](const auto &onEntry) {
        DIR *dir = opendir(QFile::encodeName(folder).constData());
        if (dir == nullptr) {
            return;
        }
        const int dirFd = dirfd(dir);
        while (const dirent *item = readdir(dir)) {
            if (item->d_name[0] == '.' && (item->d_name[1] == '\0'
                || (item->d_name[1] == '.' && item->d_name[2] == '\0'))) {
                continue;
            }
            struct stat info {};
            if (fstatat(dirFd, item->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
                continue;
            }
            onEntry(Entry{QFile::decodeName(item->d_name), qint64(info.st_size),
                          qint64(info.st_mtime), S_ISDIR(info.st_mode)});
        }
        closedir(dir);
    });
#endif

    // 2) std::filesystem: portable, sin Qt.
    measureListing(QStringLiteral("std_filesystem"), [&](const auto &onEntry) {
        std::error_code error;
        const std::filesystem::path root = std::filesystem::path(folder.toStdU16String());
        for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            std::error_code ignored;
            const bool isDir = it->is_directory(ignored);
            const auto size = isDir ? 0 : it->file_size(ignored);
            const auto time = it->last_write_time(ignored);
            onEntry(Entry{QString::fromStdU16String(it->path().filename().u16string()), qint64(size),
                          qint64(std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count()),
                          isDir});
        }
    });

    // 3) QDirIterator + QFileInfo: la vía "cómoda" de Qt.
    measureListing(QStringLiteral("qt_qdiriterator"), [&](const auto &onEntry) {
        QDirIterator it(folder, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        while (it.hasNext()) {
            const QFileInfo info = it.nextFileInfo();
            onEntry(Entry{info.fileName(), info.size(), info.lastModified().toSecsSinceEpoch(), info.isDir()});
        }
    });

    // 4) QDir::entryInfoList: devuelve todo de golpe (no es progresivo).
    measureListing(QStringLiteral("qt_entryinfolist"), [&](const auto &onEntry) {
        const QFileInfoList list = QDir(folder).entryInfoList(
            QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Unsorted);
        for (const QFileInfo &info : list) {
            onEntry(Entry{info.fileName(), info.size(), info.lastModified().toSecsSinceEpoch(), info.isDir()});
        }
    });
    return 0;
}

int commandSort(const QString &folder)
{
    QStringList names = QDir(folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Unsorted);
    const QString count = QStringLiteral("%1 nombres").arg(names.size());

    QElapsedTimer timer;
    timer.start();
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        return eu::core::naturalCompare(a, b) < 0;
    });
    report(QStringLiteral("orden_natural"), timer.nsecsElapsed(), count);

    // Filtrado local (SPEC §6.6): subcadena sin distinguir mayúsculas, en un solo hilo.
    for (const QString &needle : {QStringLiteral("a"), QStringLiteral("foto"), QStringLiteral("9999"),
                                  QStringLiteral("no-existe")}) {
        timer.restart();
        qsizetype matches = 0;
        for (const QString &name : std::as_const(names)) {
            if (name.contains(needle, Qt::CaseInsensitive)) {
                ++matches;
            }
        }
        report(QStringLiteral("filtro_local"), timer.nsecsElapsed(),
               QStringLiteral("\"%1\": %2 coincidencias de %3").arg(needle).arg(matches).arg(count));
    }
    return 0;
}

#ifdef Q_OS_UNIX
/// Copia con un bucle read/write de 1 MiB y fsync final: el esquema base que
/// usará el módulo ops (permite progreso, pausa, cancelación y hash al vuelo).
bool copyWithLoop(const QString &source, const QString &target)
{
    const int in = open(QFile::encodeName(source).constData(), O_RDONLY);
    const int outFd = open(QFile::encodeName(target).constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in < 0 || outFd < 0) {
        return false;
    }
    std::vector<char> buffer(1024 * 1024);
    bool ok = true;
    while (true) {
        const ssize_t got = read(in, buffer.data(), buffer.size());
        if (got == 0) {
            break;
        }
        if (got < 0) {
            ok = false;
            break;
        }
        ssize_t written = 0;
        while (written < got) {
            const ssize_t step = write(outFd, buffer.data() + written, size_t(got - written));
            if (step < 0) {
                ok = false;
                break;
            }
            written += step;
        }
        if (!ok) {
            break;
        }
    }
    ok = ok && fsync(outFd) == 0;
    close(in);
    close(outFd);
    return ok;
}

void syncFile(const QString &path)
{
    const int fd = open(QFile::encodeName(path).constData(), O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}
#endif

int commandCopy(const QString &source, const QString &targetFolder)
{
    const qint64 bytes = QFileInfo(source).size();
    const QString target = targetFolder + QStringLiteral("/eu-bench-copia.tmp");
    const auto megabytesPerSecond = [bytes](qint64 ns) {
        return QStringLiteral("%1 MB/s (incluye fsync)").arg(QString::number(double(bytes) / 1e6 / (double(ns) / 1e9), 'f', 0));
    };
    QElapsedTimer timer;

#ifdef Q_OS_UNIX
    QFile::remove(target);
    timer.start();
    if (copyWithLoop(source, target)) {
        const qint64 ns = timer.nsecsElapsed();
        report(QStringLiteral("copia_bucle_1MiB"), ns, megabytesPerSecond(ns));
    }

    QFile::remove(target);
    timer.restart();
    std::error_code error;
    std::filesystem::copy_file(source.toStdString(), target.toStdString(), error);
    syncFile(target);
    if (!error) {
        const qint64 ns = timer.nsecsElapsed();
        report(QStringLiteral("copia_std_copy_file"), ns, megabytesPerSecond(ns));
    }

    QFile::remove(target);
    timer.restart();
    if (QFile::copy(source, target)) {
        syncFile(target);
        const qint64 ns = timer.nsecsElapsed();
        report(QStringLiteral("copia_qfile_copy"), ns, megabytesPerSecond(ns));
    }
    QFile::remove(target);
#else
    Q_UNUSED(megabytesPerSecond)
    Q_UNUSED(timer)
    out() << "copy: pendiente en esta plataforma\n";
#endif
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = QCoreApplication::arguments();
    const QString command = arguments.value(1);

    if (command == QStringLiteral("mktree") && arguments.size() == 4) {
        return commandMktree(arguments[2], arguments[3].toInt());
    }
    if (command == QStringLiteral("listing") && arguments.size() == 3) {
        return commandListing(arguments[2]);
    }
    if (command == QStringLiteral("volumes")) {
        return commandVolumes();
    }
    if (command == QStringLiteral("sort") && arguments.size() == 3) {
        return commandSort(arguments[2]);
    }
    if (command == QStringLiteral("copy") && arguments.size() == 4) {
        return commandCopy(arguments[2], arguments[3]);
    }
    out() << "Uso: eu-bench mktree <carpeta> <n> | volumes | listing <carpeta> | sort <carpeta> | copy <origen> <carpetaDestino>\n";
    return 2;
}
