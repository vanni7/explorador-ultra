// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
//
// eu-bench: herramienta de medición reproducible.
//
//   eu-bench mktree  <carpeta> <cantidad>   crea <cantidad> archivos vacíos de prueba
//   eu-bench listing <carpeta>              compara formas de listar una carpeta
//   eu-bench sort    <carpeta>              ordenamiento natural y filtrado en memoria
//   eu-bench copy    <origen> <carpetaDestino>   compara formas de copiar un archivo grande
//   eu-bench walk    <carpeta> [carpeta…]   recorre los árboles a la vez (un hilo cada uno)
//   eu-bench yield   <archivo-grande> <carpeta>   listar mientras se lee, cediendo o no el disco
//   eu-bench grep    <carpeta> <texto>  busca el texto dentro de los documentos (aislado)
//   eu-bench cindex  <carpeta> <texto>  crea el índice de contenido y busca con él
//   eu-bench thumbs  <carpeta> [lado]   miniaturas de una carpeta, en el proceso y aisladas
//   eu-bench index   <carpeta> <archivo.db> [consulta…]   construye el índice y busca en él
//
// Cada medición imprime una línea "nombre;milisegundos;detalle" fácil de pegar
// en docs/BENCHMARKS.md. Aquí nunca se escribe un valor que no se haya medido.

#include "eu/core/NaturalCompare.h"
#include "eu/fs/DirectoryLister.h"
#include "eu/fs/Volumes.h"
#include "eu/index/Index.h"
#include "eu/index/ContentIndex.h"
#include "eu/search/Searcher.h"
#include "eu/thumbs/Isolated.h"
#include "eu/thumbs/Thumbnail.h"
#include "eu/tasks/Scheduler.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <functional>
#include <system_error>
#include <thread>
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

/// Recorre varios árboles a la vez, un hilo por árbol, y dice cuánto tarda
/// cada uno y el conjunto. Con una sola carpeta mide el recorrido a secas.
/// Sirve para comparar, en un disco mecánico, dos recorridos simultáneos con
/// los mismos dos uno detrás de otro (SPEC §3.3).
int commandWalk(const QStringList &folders)
{
    QElapsedTimer total;
    total.start();
    std::vector<std::thread> threads;
    std::vector<qint64> entries(size_t(folders.size()), 0);
    std::vector<qint64> directories(size_t(folders.size()), 0);
    std::vector<qint64> elapsed(size_t(folders.size()), 0);
    for (qsizetype i = 0; i < folders.size(); ++i) {
        threads.emplace_back([&, i] {
            QElapsedTimer timer;
            timer.start();
            std::vector<QString> pending{folders[i]};
            while (!pending.empty()) {
                const QString folder = pending.back();
                pending.pop_back();
                ++directories[size_t(i)];
                eu::fs::listDirectory(folder, eu::fs::ListOptions(), {}, [&](std::vector<eu::fs::Entry> &&batch) {
                    for (const eu::fs::Entry &entry : batch) {
                        ++entries[size_t(i)];
                        if (entry.isDirectory && !entry.isSymlink) {
                            pending.push_back(folder + QLatin1Char('/') + entry.name);
                        }
                    }
                });
            }
            elapsed[size_t(i)] = timer.nsecsElapsed();
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    for (qsizetype i = 0; i < folders.size(); ++i) {
        report(QStringLiteral("recorrer ") + folders[i], elapsed[size_t(i)],
               QStringLiteral("%1 entradas, %2 carpetas").arg(entries[size_t(i)]).arg(directories[size_t(i)]));
    }
    report(QStringLiteral("recorrer: total"), total.nsecsElapsed(),
           QStringLiteral("%1 árboles a la vez").arg(folders.size()));
    return 0;
}

/// Vacía la caché de disco del sistema (solo Linux, con sudo sin contraseña).
/// Devuelve false si no se pudo: entonces la medición no es de disco frío.
bool dropCaches()
{
#ifdef Q_OS_LINUX
    return std::system("sync && echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null 2>&1") == 0;
#else
    return false;
#endif
}

/// Mide cuánto tarda recorrer `folder` (como lo haría la interfaz al abrir
/// carpetas) mientras una tarea de fondo lee `bigFile` del mismo disco, con
/// y sin ceder el disco (CancelToken::yieldToInteractive). El disco se trata
/// como mecánico.
int commandYield(const QString &bigFile, const QString &folder)
{
    using namespace std::chrono_literals;
    for (const bool yielding : {false, true, false, true}) {
        const bool cold = dropCaches();
        eu::tasks::Scheduler scheduler;
        const QString disk = QStringLiteral("disco");
        scheduler.setDeviceClass(disk, eu::tasks::DeviceClass::Hdd);
        std::atomic<qint64> readBytes{0};
        eu::tasks::TaskHandle reader = scheduler.submit(
            eu::tasks::Category::Transfer, eu::tasks::Priority::ActiveTransfer, {disk},
            [&](const eu::tasks::CancelToken &cancel) {
                QFile file(bigFile);
                if (!file.open(QIODevice::ReadOnly)) {
                    return;
                }
                std::vector<char> buffer(1024 * 1024);
                while (!cancel.isCancelled()) {
                    if (yielding) {
                        cancel.yieldToInteractive();
                    }
                    const qint64 read = file.read(buffer.data(), qint64(buffer.size()));
                    if (read <= 0) {
                        break;
                    }
                    readBytes += read;
                }
            });
        std::this_thread::sleep_for(2s);   // la lectura ya va lanzada
        QElapsedTimer timer;
        timer.start();
        qint64 entries = 0;
        eu::tasks::TaskHandle walker = scheduler.submit(
            eu::tasks::Category::Listing, eu::tasks::Priority::VisibleNavigation, {disk},
            [&](const eu::tasks::CancelToken &) {
                std::vector<QString> pending{folder};
                while (!pending.empty()) {
                    const QString current = pending.back();
                    pending.pop_back();
                    eu::fs::listDirectory(current, eu::fs::ListOptions(), {},
                                          [&](std::vector<eu::fs::Entry> &&batch) {
                        for (const eu::fs::Entry &entry : batch) {
                            ++entries;
                            if (entry.isDirectory && !entry.isSymlink) {
                                pending.push_back(current + QLatin1Char('/') + entry.name);
                            }
                        }
                    });
                }
            });
        walker.waitFor(600s);
        const qint64 walkNs = timer.nsecsElapsed();
        const qint64 readDuringWalk = readBytes;
        reader.cancel();
        reader.waitFor(600s);
        report(yielding ? QStringLiteral("listar mientras se lee, cediendo el disco")
                        : QStringLiteral("listar mientras se lee, sin ceder"),
               walkNs,
               QStringLiteral("%1 entradas; leídos %2 MB hasta terminar de listar; caché %3")
                   .arg(entries).arg(readDuringWalk / 1000000).arg(cold ? QStringLiteral("vaciada")
                                                                          : QStringLiteral("SIN vaciar")));
    }
    return 0;
}

/// Busca un texto dentro de los documentos de una carpeta y sus subcarpetas,
/// leyéndolos en el proceso auxiliar como hace la aplicación.
int commandGrep(const QString &folder, const QString &text)
{
    const auto query = eu::search::Query::parse(QStringLiteral("content:\"%1\"").arg(text));
    eu::search::SearchOptions options;
    options.textProvider = [](const QString &path, const eu::tasks::CancelToken &cancel) {
        return eu::thumbs::extractTextIsolated(path, cancel);
    };
    QElapsedTimer timer;
    timer.start();
    QStringList found;
    const eu::search::SearchSummary summary = eu::search::searchTree(
        {folder}, query.value(), options, {}, nullptr, [&](std::vector<eu::search::Hit> &&hits, const eu::search::SearchProgress &) {
            for (const eu::search::Hit &hit : hits) {
                found.append(hit.entry.name + QStringLiteral("  →  ") + hit.snippet);
            }
        });
    report(QStringLiteral("buscar en el contenido «%1»").arg(text), timer.nsecsElapsed(),
           QStringLiteral("%1 resultados; %2 archivos leídos; %3 carpetas")
               .arg(summary.progress.hits).arg(summary.progress.filesRead).arg(summary.progress.foldersScanned));
    for (const QString &line : std::as_const(found).mid(0, 5)) {
        out() << "    " << line << '\n';
    }
    out().flush();
    return 0;
}

/// Crea el índice de contenido de una carpeta (en una base temporal), dice
/// cuánto ocupa y busca con él, comparando con la búsqueda sin índice.
int commandContentIndex(const QString &folder, const QString &text)
{
    QTemporaryDir data;
    const QString database = data.filePath(QStringLiteral("contenido.db"));
    const auto opened = eu::index::ContentIndex::open(database, folder);
    if (!opened.ok()) {
        out() << "ERROR: " << opened.error().message << '\n';
        return 1;
    }
    const auto isolated = [](const QString &path, const eu::tasks::CancelToken &cancel) {
        return eu::thumbs::extractTextIsolated(path, cancel);
    };
    eu::index::ContentSyncOptions syncOptions;
    syncOptions.textProvider = isolated;
    QElapsedTimer timer;
    timer.start();
    const auto synced = opened.value().sync(syncOptions, {});
    if (!synced.ok()) {
        out() << "ERROR: " << synced.error().message << '\n';
        return 1;
    }
    qint64 bytes = 0;
    for (const QString &suffix : {QString(), QStringLiteral("-wal")}) {
        bytes += QFileInfo(database + suffix).size();
    }
    const eu::index::ContentIndexInfo info = opened.value().info();
    report(QStringLiteral("índice de contenido: crear"), timer.nsecsElapsed(),
           QStringLiteral("%1 documentos (%2 con texto, %3 sin); %4 millones de caracteres; base %5 MB")
               .arg(info.documents).arg(info.withText).arg(synced.value().failed)
               .arg(double(info.textChars) / 1e6, 0, 'f', 1).arg(double(bytes) / 1e6, 0, 'f', 1));
    timer.restart();
    const auto again = opened.value().sync(syncOptions, {});
    report(QStringLiteral("índice de contenido: repasar sin cambios"), timer.nsecsElapsed(),
           QStringLiteral("%1 documentos vistos, %2 leídos").arg(again.value().found).arg(again.value().read));

    const auto query = eu::search::Query::parse(QStringLiteral("content:\"%1\"").arg(text));
    for (const bool withIndex : {true, false}) {
        eu::search::SearchOptions options;
        options.textProvider = isolated;
        if (withIndex) {
            options.contentCache = [index = opened.value()] { return eu::index::ContentIndex::cacheFor({index}); };
        }
        timer.restart();
        qint64 firstNs = -1;
        const eu::search::SearchSummary summary = eu::search::searchTree(
            {folder}, query.value(), options, {}, nullptr,
            [&](std::vector<eu::search::Hit> &&hits, const eu::search::SearchProgress &) {
                if (firstNs < 0 && !hits.empty()) {
                    firstNs = timer.nsecsElapsed();
                }
            });
        report(QStringLiteral("buscar «%1» %2").arg(text, withIndex ? QStringLiteral("con índice") : QStringLiteral("sin índice")),
               timer.nsecsElapsed(),
               QStringLiteral("%1 resultados; %2 leídos del disco; %3 del índice")
                   .arg(summary.progress.hits).arg(summary.progress.filesRead).arg(summary.progress.textsFromIndex));
    }
    return 0;
}

/// Genera las miniaturas de los archivos de una carpeta dos veces: en este
/// proceso y en el auxiliar aislado, para saber cuánto cuesta aislar.
int commandThumbs(const QString &folder, int side)
{
    const auto listed = eu::fs::listDirectoryAll(folder);
    if (!listed.ok()) {
        out() << "No se pudo leer la carpeta\n";
        return 1;
    }
    std::vector<eu::thumbs::ThumbnailRequest> requests;
    for (const eu::fs::Entry &entry : listed.value()) {
        eu::thumbs::ThumbnailRequest request;
        request.path = folder + QLatin1Char('/') + entry.name;
        request.size = entry.size;
        request.modifiedMs = entry.modifiedMs;
        request.maxSide = side;
        request.isDirectory = entry.isDirectory;
        if (!entry.isDirectory && eu::thumbs::thumbnailSourceFor(request) != eu::thumbs::ThumbnailSource::None) {
            requests.push_back(request);
        }
    }
    for (const bool isolated : {false, true}) {
        int ok = 0;
        QElapsedTimer timer;
        timer.start();
        for (const auto &request : requests) {
            const auto image = isolated ? eu::thumbs::renderThumbnailIsolated(request, {})
                                        : eu::thumbs::renderThumbnail(request, {});
            ok += image.ok() ? 1 : 0;
        }
        report(isolated ? QStringLiteral("miniaturas aisladas") : QStringLiteral("miniaturas en el proceso"),
               timer.nsecsElapsed(),
               QStringLiteral("%1 de %2 archivos, lado %3; auxiliar %4").arg(ok).arg(requests.size()).arg(side)
                   .arg(eu::thumbs::hasThumbnailHelper() ? QStringLiteral("sí") : QStringLiteral("no")));
    }
    return 0;
}

/// Construye (o pone al día) el índice de una carpeta y mide búsquedas en él:
/// tiempo hasta el primer resultado y tiempo total.
int commandIndex(const QString &folder, const QString &databasePath, const QStringList &queries)
{
    const auto opened = eu::index::Index::open(databasePath, folder);
    if (!opened.ok()) {
        out() << "No se pudo abrir el índice: " << opened.error().message << '\n';
        return 1;
    }
    const eu::index::Index index = opened.value();
    QElapsedTimer timer;
    timer.start();
    const auto synced = index.sync(folder, {}, {});
    if (!synced.ok()) {
        out() << "No se pudo indexar: " << synced.error().message << '\n';
        return 1;
    }
    const eu::index::IndexInfo info = index.info();
    report(QStringLiteral("índice: recorrido completo"), timer.nsecsElapsed(),
           QStringLiteral("%1 entradas, %2 carpetas; añadidas %3, quitadas %4, cambiadas %5; ilegibles %6; "
                          "base %7 MB; RSS %8 MB")
               .arg(info.entries).arg(info.folders).arg(synced.value().added).arg(synced.value().removed)
               .arg(synced.value().updated).arg(synced.value().unreadableFolders)
               .arg(double(QFileInfo(databasePath).size()) / 1e6, 0, 'f', 1)
               .arg(residentMemoryMb(), 0, 'f', 1));

    for (const QString &text : queries) {
        const auto query = eu::search::Query::parse(text);
        if (!query.ok()) {
            out() << "Consulta no válida: " << text << '\n';
            continue;
        }
        eu::search::SearchOptions options;
        options.batchSize = 1;   // entregar en cuanto haya algo, para medir el primer resultado
        options.maxBatchDelayMs = 0;
        qint64 firstNs = -1;
        timer.restart();
        const eu::search::SearchSummary summary = index.search(
            {folder}, query.value(), options, {},
            [&](std::vector<eu::search::Hit> &&hits, const eu::search::SearchProgress &) {
                if (firstNs < 0 && !hits.empty()) {
                    firstNs = timer.nsecsElapsed();
                }
            });
        report(QStringLiteral("índice: buscar «%1»").arg(text), timer.nsecsElapsed(),
               QStringLiteral("%1 resultados; primer resultado a los %2 ms; filas examinadas %3")
                   .arg(summary.progress.hits)
                   .arg(firstNs < 0 ? QStringLiteral("—") : QString::number(double(firstNs) / 1e6, 'f', 2))
                   .arg(summary.progress.itemsExamined));
    }
    return 0;
}

int main(int argc, char *argv[])
{
    // QGuiApplication: las miniaturas de texto y la duración pintan letras.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);
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
    if (command == QStringLiteral("grep") && arguments.size() == 4) {
        return commandGrep(QDir::cleanPath(arguments[2]), arguments[3]);
    }
    if (command == QStringLiteral("cindex") && arguments.size() == 4) {
        return commandContentIndex(QDir::cleanPath(arguments[2]), arguments[3]);
    }
    if (command == QStringLiteral("thumbs") && arguments.size() >= 3) {
        return commandThumbs(QDir::cleanPath(arguments[2]), arguments.size() >= 4 ? arguments[3].toInt() : 256);
    }
    if (command == QStringLiteral("yield") && arguments.size() == 4) {
        return commandYield(arguments[2], QDir::cleanPath(arguments[3]));
    }
    if (command == QStringLiteral("walk") && arguments.size() >= 3) {
        QStringList folders;
        for (const QString &folder : arguments.mid(2)) {
            folders.append(QDir::cleanPath(folder));
        }
        return commandWalk(folders);
    }
    if (command == QStringLiteral("index") && arguments.size() >= 4) {
        return commandIndex(QDir::cleanPath(arguments[2]), arguments[3], arguments.mid(4));
    }
    out() << "Uso: eu-bench index <carpeta> <archivo.db> [consulta…]\n";
    out() << "Uso: eu-bench mktree <carpeta> <n> | volumes | listing <carpeta> | sort <carpeta> | copy <origen> <carpetaDestino>\n";
    return 2;
}
