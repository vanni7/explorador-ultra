// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ops/FileOps.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace eu;
using namespace eu::ops;

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(content);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<no se pudo leer>");
}

} // namespace

class TestFileOps : public QObject
{
    Q_OBJECT

private slots:
    void stamps()
    {
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("a.txt")), "12345");
        const FileStamp file = stampOf(directory.filePath(QStringLiteral("a.txt")));
        QVERIFY(file.exists && !file.isDirectory && !file.isSymlink);
        QCOMPARE(file.size, qint64(5));
        QVERIFY(file.modifiedMs > 0);
        QVERIFY(stampOf(directory.path()).isDirectory);
        QVERIFY(!stampOf(directory.filePath(QStringLiteral("nada"))).exists);
    }

    void makeDirectoryAndCreateFileRefuseExisting()
    {
        QTemporaryDir directory;
        const QString folder = directory.filePath(QStringLiteral("nueva"));
        QVERIFY(makeDirectory(folder).ok());
        const core::Status again = makeDirectory(folder);
        QVERIFY(!again.ok());
        QCOMPARE(again.error().code, core::ErrorCode::AlreadyExists);

        const QString file = directory.filePath(QStringLiteral("nuevo.txt"));
        QVERIFY(createEmptyFile(file).ok());
        writeFile(file, "contenido");
        const core::Status secondFile = createEmptyFile(file);
        QCOMPARE(secondFile.error().code, core::ErrorCode::AlreadyExists);
        QCOMPARE(readFile(file), QByteArray("contenido"));   // no se vació

        QCOMPARE(makeDirectory(directory.filePath(QStringLiteral("no/existe/padre"))).error().code,
                 core::ErrorCode::NotFound);
    }

    void renameNeverOverwrites()
    {
        QTemporaryDir directory;
        const QString a = directory.filePath(QStringLiteral("a"));
        const QString b = directory.filePath(QStringLiteral("b"));
        writeFile(a, "soy a");
        writeFile(b, "soy b");

        const core::Status blocked = renameNoReplace(a, b);
        QVERIFY(!blocked.ok());
        QCOMPARE(blocked.error().code, core::ErrorCode::AlreadyExists);
        QCOMPARE(readFile(a), QByteArray("soy a"));
        QCOMPARE(readFile(b), QByteArray("soy b"));

        const QString c = directory.filePath(QStringLiteral("ñandú 🦤.txt"));
        QVERIFY(renameNoReplace(a, c).ok());
        QVERIFY(!QFileInfo::exists(a));
        QCOMPARE(readFile(c), QByteArray("soy a"));

        QCOMPARE(renameNoReplace(directory.filePath(QStringLiteral("fantasma")), a).error().code,
                 core::ErrorCode::NotFound);
    }

    void renameWorksForFolders()
    {
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("carpeta/dentro")));
        writeFile(directory.filePath(QStringLiteral("carpeta/dentro/x")), "x");
        QVERIFY(renameNoReplace(directory.filePath(QStringLiteral("carpeta")),
                                directory.filePath(QStringLiteral("otra"))).ok());
        QCOMPARE(readFile(directory.filePath(QStringLiteral("otra/dentro/x"))), QByteArray("x"));
    }

    void copyKeepsContentDateAndLeavesNoPartial()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("origen.bin"));
        // 3,5 MiB: obliga a varias vueltas del bucle de 1 MiB.
        QByteArray content(3 * 1024 * 1024 + 512 * 1024, '\0');
        for (qsizetype i = 0; i < content.size(); ++i) {
            content[i] = char(i * 31 + i / 7);
        }
        writeFile(source, content);
        QFile stamper(source);
        QVERIFY(stamper.open(QIODevice::ReadWrite));
        const QDateTime oldDate = QDateTime::fromSecsSinceEpoch(1'600'000'000);
        QVERIFY(stamper.setFileTime(oldDate, QFileDevice::FileModificationTime));
        stamper.close();

        const QString target = directory.filePath(QStringLiteral("copia.bin"));
        qint64 reported = 0;
        const core::Status status = copyFile(source, target, tasks::CancelToken(),
                                             [&](qint64 bytes) { reported += bytes; });
        QVERIFY2(status.ok(), status.ok() ? "" : qPrintable(status.error().message));
        QCOMPARE(readFile(target), content);
        QCOMPARE(reported, qint64(content.size()));
        QCOMPARE(QFileInfo(target).lastModified().toSecsSinceEpoch(), oldDate.toSecsSinceEpoch());
        QVERIFY(!QFileInfo::exists(target + partialSuffix()));
        QCOMPARE(readFile(source), content);   // el origen queda intacto
    }

    void copyRefusesExistingTarget()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("a"));
        const QString target = directory.filePath(QStringLiteral("b"));
        writeFile(source, "nuevo");
        writeFile(target, "valioso");
        const core::Status status = copyFile(source, target, tasks::CancelToken());
        QCOMPARE(status.error().code, core::ErrorCode::AlreadyExists);
        QCOMPARE(readFile(target), QByteArray("valioso"));
    }

    void cancelledCopyLeavesNothingBehind()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("grande"));
        writeFile(source, QByteArray(5 * 1024 * 1024, 'z'));
        const QString target = directory.filePath(QStringLiteral("destino"));

        tasks::CancelSource cancel;
        const core::Status status = copyFile(source, target, cancel.token(), [&](qint64) { cancel.cancel(); });
        QVERIFY(!status.ok());
        QCOMPARE(status.error().code, core::ErrorCode::Cancelled);
        QVERIFY(!QFileInfo::exists(target));
        QVERIFY(!QFileInfo::exists(target + partialSuffix()));
        QCOMPARE(QFileInfo(source).size(), qint64(5 * 1024 * 1024));
    }

    void copyOfMissingSourceFails()
    {
        QTemporaryDir directory;
        const core::Status status = copyFile(directory.filePath(QStringLiteral("no")),
                                             directory.filePath(QStringLiteral("destino")), tasks::CancelToken());
        QCOMPARE(status.error().code, core::ErrorCode::NotFound);
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("destino"))));
    }

    void removalIsCareful()
    {
        QTemporaryDir directory;
        const QString folder = directory.filePath(QStringLiteral("carpeta"));
        QVERIFY(QDir().mkdir(folder));
        writeFile(folder + QStringLiteral("/x"), "x");

        // removeFile no borra carpetas; removeEmptyDirectory no borra carpetas con contenido.
        QVERIFY(!removeFile(folder).ok());
        const core::Status notEmpty = removeEmptyDirectory(folder);
        QVERIFY(!notEmpty.ok());
        QCOMPARE(notEmpty.error().code, core::ErrorCode::DirectoryNotEmpty);
        QVERIFY(QFileInfo::exists(folder + QStringLiteral("/x")));

        QVERIFY(removeFile(folder + QStringLiteral("/x")).ok());
        QVERIFY(removeEmptyDirectory(folder).ok());
        QVERIFY(!QFileInfo::exists(folder));
        QCOMPARE(removeFile(folder).error().code, core::ErrorCode::NotFound);
    }

    void symlinksAreHandledAsLinks()
    {
#ifdef Q_OS_UNIX
        QTemporaryDir directory;
        const QString realFolder = directory.filePath(QStringLiteral("real"));
        QVERIFY(QDir().mkdir(realFolder));
        writeFile(realFolder + QStringLiteral("/tesoro"), "no me borres");
        const QString link = directory.filePath(QStringLiteral("enlace"));
        QVERIFY(QFile::link(realFolder, link));

        const FileStamp stamp = stampOf(link);
        QVERIFY(stamp.isSymlink && !stamp.isDirectory);

        const QString linkCopy = directory.filePath(QStringLiteral("enlace-copia"));
        QVERIFY(copySymlink(link, linkCopy).ok());
        QVERIFY(stampOf(linkCopy).isSymlink);
        QCOMPARE(QFileInfo(linkCopy).symLinkTarget(), realFolder);

        // Borrar el enlace no toca la carpeta a la que apunta.
        QVERIFY(removeFile(link).ok());
        QVERIFY(!stampOf(link).exists);
        QCOMPARE(readFile(realFolder + QStringLiteral("/tesoro")), QByteArray("no me borres"));
#else
        QSKIP("Crear enlaces simbólicos en Windows requiere privilegios.");
#endif
    }

    void uniqueNames()
    {
        QTemporaryDir directory;
        QCOMPARE(uniqueName(directory.path(), QStringLiteral("foto.jpg")), QStringLiteral("foto.jpg"));
        writeFile(directory.filePath(QStringLiteral("foto.jpg")), "");
        QCOMPARE(uniqueName(directory.path(), QStringLiteral("foto.jpg")), QStringLiteral("foto (2).jpg"));
        writeFile(directory.filePath(QStringLiteral("foto (2).jpg")), "");
        QCOMPARE(uniqueName(directory.path(), QStringLiteral("foto.jpg")), QStringLiteral("foto (3).jpg"));
        writeFile(directory.filePath(QStringLiteral(".oculto")), "");
        QCOMPARE(uniqueName(directory.path(), QStringLiteral(".oculto")), QStringLiteral(".oculto (2)"));
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("carpeta")));
        QCOMPARE(uniqueName(directory.path(), QStringLiteral("carpeta")), QStringLiteral("carpeta (2)"));
    }
};

QTEST_APPLESS_MAIN(TestFileOps)
#include "test_file_ops.moc"
