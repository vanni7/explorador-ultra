// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/fs/DirectoryLister.h"
#include "eu/fs/TreeSize.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using namespace eu;
using namespace eu::fs;

namespace {

void writeFile(const QString &path, const QByteArray &content = {})
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(content);
}

const Entry *find(const std::vector<Entry> &entries, const QString &name)
{
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [&](const Entry &entry) { return entry.name == name; });
    return it == entries.end() ? nullptr : &*it;
}

} // namespace

class TestDirectoryLister : public QObject
{
    Q_OBJECT

private slots:
    void listsFilesAndFolders()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        writeFile(directory.filePath(QStringLiteral("nota.txt")), "hola mundo");
        writeFile(directory.filePath(QStringLiteral("vacío.bin")));
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("carpeta")));

        const auto result = listDirectoryAll(directory.path());
        QVERIFY(result.ok());
        QCOMPARE(result.value().size(), size_t(3));

        const Entry *note = find(result.value(), QStringLiteral("nota.txt"));
        QVERIFY(note != nullptr);
        QCOMPARE(note->size, qint64(10));
        QVERIFY(!note->isDirectory);
        QVERIFY(!note->isSymlink);
        QVERIFY(!note->statFailed);
        // La fecha de modificación es de hace instantes.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QVERIFY(qAbs(now - note->modifiedMs) < 60'000);

        const Entry *folder = find(result.value(), QStringLiteral("carpeta"));
        QVERIFY(folder != nullptr);
        QVERIFY(folder->isDirectory);
    }

    void emptyFolderIsNotAnError()
    {
        QTemporaryDir directory;
        const auto result = listDirectoryAll(directory.path());
        QVERIFY(result.ok());
        QVERIFY(result.value().empty());
    }

    void unicodeNamesSurvive()
    {
        QTemporaryDir directory;
        const QStringList names{
            QStringLiteral("ñandú.txt"),
            QStringLiteral("emoji 🦤🎉.txt"),
            QStringLiteral("עברית.txt"),             // derecha a izquierda
            QStringLiteral("日本語のファイル.txt"),
            QString(200, QLatin1Char('x')),           // nombre largo
        };
        for (const QString &name : names) {
            writeFile(directory.filePath(name));
        }
        const auto result = listDirectoryAll(directory.path());
        QVERIFY(result.ok());
        QCOMPARE(result.value().size(), size_t(names.size()));
        for (const QString &name : names) {
            // macOS puede devolver el nombre descompuesto (NFD): se compara normalizado.
            const bool found = std::any_of(result.value().begin(), result.value().end(), [&](const Entry &entry) {
                return entry.name.normalized(QString::NormalizationForm_C)
                    == name.normalized(QString::NormalizationForm_C);
            });
            QVERIFY2(found, qPrintable(name));
        }
    }

    void missingFolderReportsNotFound()
    {
        QTemporaryDir directory;
        const auto result = listDirectoryAll(directory.filePath(QStringLiteral("no-existe")));
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code, core::ErrorCode::NotFound);
    }

    void fileInsteadOfFolderIsAnError()
    {
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("archivo")));
        const auto result = listDirectoryAll(directory.filePath(QStringLiteral("archivo")));
        QVERIFY(!result.ok());
#ifdef Q_OS_UNIX
        QCOMPARE(result.error().code, core::ErrorCode::NotADirectory);
#endif
    }

    void hiddenFilesAreMarked()
    {
#ifdef Q_OS_UNIX
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral(".oculto")));
        writeFile(directory.filePath(QStringLiteral("visible")));
        const auto result = listDirectoryAll(directory.path());
        QVERIFY(result.ok());
        QVERIFY(find(result.value(), QStringLiteral(".oculto"))->isHidden);
        QVERIFY(!find(result.value(), QStringLiteral("visible"))->isHidden);
#else
        QSKIP("En Windows lo oculto es un atributo; se prueba en la fase de operaciones.");
#endif
    }

    void symlinksAreReportedAndNotFollowed()
    {
#ifdef Q_OS_UNIX
        QTemporaryDir directory;
        QDir root(directory.path());
        QVERIFY(root.mkdir(QStringLiteral("real")));
        writeFile(directory.filePath(QStringLiteral("real/dentro.txt")), "x");
        QVERIFY(QFile::link(directory.filePath(QStringLiteral("real")), directory.filePath(QStringLiteral("enlace-carpeta"))));
        QVERIFY(QFile::link(directory.filePath(QStringLiteral("no-existe")), directory.filePath(QStringLiteral("enlace-roto"))));
        // Ciclo: real/bucle apunta a la carpeta que lo contiene.
        QVERIFY(QFile::link(directory.path(), directory.filePath(QStringLiteral("real/bucle"))));

        const auto result = listDirectoryAll(directory.path());
        QVERIFY(result.ok());
        QCOMPARE(result.value().size(), size_t(3));   // no se entra en los enlaces

        const Entry *linkToFolder = find(result.value(), QStringLiteral("enlace-carpeta"));
        QVERIFY(linkToFolder->isSymlink);
        QVERIFY(linkToFolder->isDirectory);
        QVERIFY(!linkToFolder->isBrokenSymlink);

        const Entry *broken = find(result.value(), QStringLiteral("enlace-roto"));
        QVERIFY(broken->isSymlink);
        QVERIFY(broken->isBrokenSymlink);
        QVERIFY(!broken->isDirectory);

        QVERIFY(!find(result.value(), QStringLiteral("real"))->isSymlink);
#else
        QSKIP("Crear enlaces simbólicos en Windows requiere privilegios.");
#endif
    }

    void unreadableFolderReportsPermissionDenied()
    {
#ifdef Q_OS_UNIX
        if (geteuid() == 0) {
            QSKIP("root puede leer cualquier carpeta.");
        }
        QTemporaryDir directory;
        const QString locked = directory.filePath(QStringLiteral("cerrada"));
        QVERIFY(QDir().mkdir(locked));
        QVERIFY(QFile::setPermissions(locked, QFileDevice::Permissions()));
        const auto result = listDirectoryAll(locked);
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code, core::ErrorCode::PermissionDenied);
#else
        QSKIP("Los permisos de Windows (ACL) se prueban en la fase de operaciones.");
#endif
    }

    void largeFolderArrivesInBatches()
    {
        QTemporaryDir directory;
        const int count = 20'000;
        for (int i = 0; i < count; ++i) {
            writeFile(directory.filePath(QStringLiteral("archivo%1.dat").arg(i)));
        }

        std::vector<size_t> batchSizes;
        size_t total = 0;
        ListOptions options;
        const core::Status status = listDirectory(directory.path(), options, tasks::CancelToken(),
                                                  [&](std::vector<Entry> &&batch) {
            batchSizes.push_back(batch.size());
            total += batch.size();
        });
        QVERIFY(status.ok());
        QCOMPARE(total, size_t(count));
        QVERIFY(batchSizes.size() >= 4);
        // El primer lote es pequeño para pintar pronto.
        QVERIFY(batchSizes.front() <= size_t(options.firstBatchSize));
        for (size_t size : batchSizes) {
            QVERIFY(size <= size_t(options.batchSize));
        }
    }

    void cancellationStopsTheListing()
    {
        QTemporaryDir directory;
        for (int i = 0; i < 2000; ++i) {
            writeFile(directory.filePath(QString::number(i)));
        }
        tasks::CancelSource source;
        size_t delivered = 0;
        const core::Status status = listDirectory(directory.path(), ListOptions(), source.token(),
                                                  [&](std::vector<Entry> &&batch) {
            delivered += batch.size();
            source.cancel();   // se cancela al recibir el primer lote
        });
        QVERIFY(!status.ok());
        QCOMPARE(status.error().code, core::ErrorCode::Cancelled);
        QVERIFY(delivered < 2000);
    }

    void treeSizeAddsEverythingWithoutFollowingLinks()
    {
        QTemporaryDir directory;
        const QDir root(directory.path());
        QVERIFY(root.mkpath(QStringLiteral("a/b/c")));
        QVERIFY(root.mkpath(QStringLiteral("vacia")));
        const auto write = [&](const QString &relative, qsizetype bytes) {
            QFile file(root.filePath(relative));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray(bytes, 'x'));
        };
        write(QStringLiteral("uno.bin"), 1000);
        write(QStringLiteral("a/dos.bin"), 2000);
        write(QStringLiteral("a/b/c/tres.bin"), 3000);
        // Un enlace a la propia carpeta: si se siguiera, la suma no acabaría nunca.
        const bool linked = QFile::link(directory.path(), root.filePath(QStringLiteral("a/ciclo")));

        int progressCalls = 0;
        const tasks::CancelToken cancel;
        const TreeSize size = measureTree(directory.path(), cancel, [&](const TreeSize &) { ++progressCalls; });
        QCOMPARE(size.bytes, qint64(6000));
        QCOMPARE(size.folders, qint64(4));
        QCOMPARE(size.files, qint64(linked ? 4 : 3));
        QVERIFY(!size.incomplete);
        QVERIFY(!size.cancelled);
        QCOMPARE(progressCalls, 5);   // una vez por carpeta leída

        // Una carpeta que no existe no es un tamaño cero fiable: se marca incompleta.
        QVERIFY(measureTree(root.filePath(QStringLiteral("no-existe")), cancel).incomplete);

        tasks::CancelSource source;
        source.cancel();
        QVERIFY(measureTree(directory.path(), source.token()).cancelled);
    }
};

QTEST_APPLESS_MAIN(TestDirectoryLister)
#include "test_directory_lister.moc"
