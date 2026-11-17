// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/dupes/Duplicates.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using namespace eu;
using namespace eu::dupes;

namespace {

void write(const QString &path, const QByteArray &content, const QDateTime &modified = {})
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
    file.flush();
    if (modified.isValid()) {
        QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
    }
}

QByteArray pattern(qint64 size, char seed)
{
    QByteArray data(size, Qt::Uninitialized);
    for (qint64 i = 0; i < size; ++i) {
        data[i] = char((i * 7 + seed) % 251);
    }
    return data;
}

QStringList relative(const Group &group, const QString &root)
{
    QStringList out;
    for (const File &file : group.files) {
        out.append(QDir(root).relativeFilePath(file.path));
    }
    return out;
}

} // namespace

class TestDupes : public QObject
{
    Q_OBJECT

private slots:
    void findsRealCopiesOnly()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        const QByteArray big = pattern(200000, 1);
        write(root + QStringLiteral("/a/foto.jpg"), big);
        write(root + QStringLiteral("/b/foto (copia).jpg"), big);
        write(root + QStringLiteral("/c/otra.jpg"), big);
        // Mismo tamaño, distinto en medio (pasa el tamaño, no el hash parcial)…
        QByteArray middle = big;
        middle[100000] = char(middle[100000] ^ 1);
        write(root + QStringLiteral("/d/medio.jpg"), middle);
        // …y distinto en un punto que el hash parcial no mira (lo pilla el completo).
        QByteArray hidden = big;
        hidden[50000] = char(hidden[50000] ^ 1);
        write(root + QStringLiteral("/d/escondido.jpg"), hidden);
        // Pequeños iguales, y vacíos (no cuentan).
        write(root + QStringLiteral("/p1.txt"), "hola");
        write(root + QStringLiteral("/p2.txt"), "hola");
        write(root + QStringLiteral("/v1.txt"), "");
        write(root + QStringLiteral("/v2.txt"), "");
        write(root + QStringLiteral("/solo.bin"), pattern(5000, 9));

        Options options;
        options.roots = {root};
        QList<Progress::Stage> stages;
        const Result result = findDuplicates(options, {}, [&](const Progress &progress) {
            if (stages.isEmpty() || stages.last() != progress.stage) {
                stages.append(progress.stage);
            }
        });
        QCOMPARE(result.groups.size(), 2);
        QCOMPARE(relative(result.groups[0], root),
                 (QStringList{QStringLiteral("a/foto.jpg"), QStringLiteral("b/foto (copia).jpg"), QStringLiteral("c/otra.jpg")}));
        QCOMPARE(result.groups[0].physicalCopies, 3);
        QCOMPARE(result.groups[0].recoverable(), 2 * 200000);
        QCOMPARE(relative(result.groups[1], root), (QStringList{QStringLiteral("p1.txt"), QStringLiteral("p2.txt")}));
        QCOMPARE(result.recoverable(), 2 * 200000 + 4);
        QVERIFY(stages.contains(Progress::Stage::PartialHash));
        QVERIFY(stages.contains(Progress::Stage::FullHash));
        QCOMPARE(stages.last(), Progress::Stage::Done);

        // Byte a byte da lo mismo.
        options.byteByByte = true;
        const Result checked = findDuplicates(options, {});
        QCOMPARE(checked.groups.size(), 2);
        QCOMPARE(checked.groups[0].files.size(), 3);

        // Cancelar no deja resultados a medias.
        tasks::CancelSource cancel;
        cancel.cancel();
        QVERIFY(findDuplicates(options, cancel.token()).groups.isEmpty());
    }

    void hardlinksAreNotCopies()
    {
#ifndef Q_OS_UNIX
        QSKIP("Enlaces físicos con link() solo en Unix.");
#else
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        write(root + QStringLiteral("/original.bin"), pattern(30000, 3));
        QCOMPARE(::link(QFile::encodeName(root + QStringLiteral("/original.bin")).constData(),
                        QFile::encodeName(root + QStringLiteral("/enlace.bin")).constData()), 0);
        Options options;
        options.roots = {root};
        Result result = findDuplicates(options, {});
        QVERIFY(result.groups.isEmpty());   // es un solo archivo con dos nombres
        QCOMPARE(result.hardlinkOnlySets, 1);

        // Con una copia real además: dos copias físicas, una con dos nombres.
        write(root + QStringLiteral("/copia.bin"), pattern(30000, 3));
        result = findDuplicates(options, {});
        QCOMPARE(result.groups.size(), 1);
        QCOMPARE(result.groups[0].files.size(), 3);
        QCOMPARE(result.groups[0].physicalCopies, 2);
        QCOMPARE(result.groups[0].recoverable(), 30000);
        // Conservando el original, se quita la copia, no el otro nombre del original.
        const QStringList remove = proposeRemoval(result.groups[0], Keep::Preferred, root + QStringLiteral("/original.bin"));
        QCOMPARE(remove, QStringList{root + QStringLiteral("/copia.bin")});
#endif
    }

    void selectionRulesKeepOneRealCopy()
    {
        Group group;
        group.size = 10;
        group.physicalCopies = 3;
        const qint64 day = 24LL * 3600 * 1000;
        group.files = {File{QStringLiteral("/fotos/2020/a.jpg"), 3 * day, 0, 0, 0},
                       File{QStringLiteral("/respaldo/a.jpg"), 1 * day, 0, 0, 1},
                       File{QStringLiteral("/descargas/x/y/a (1).jpg"), 5 * day, 0, 0, 2}};
        QCOMPARE(proposeRemoval(group, Keep::Oldest),
                 (QStringList{QStringLiteral("/fotos/2020/a.jpg"), QStringLiteral("/descargas/x/y/a (1).jpg")}));
        QCOMPARE(proposeRemoval(group, Keep::Newest),
                 (QStringList{QStringLiteral("/fotos/2020/a.jpg"), QStringLiteral("/respaldo/a.jpg")}));
        QCOMPARE(proposeRemoval(group, Keep::Preferred, QStringLiteral("/fotos")),
                 (QStringList{QStringLiteral("/respaldo/a.jpg"), QStringLiteral("/descargas/x/y/a (1).jpg")}));
        QCOMPARE(proposeRemoval(group, Keep::ShortestPath),
                 (QStringList{QStringLiteral("/fotos/2020/a.jpg"), QStringLiteral("/descargas/x/y/a (1).jpg")}));
        // Siempre queda una.
        QCOMPARE(proposeRemoval(group, Keep::Preferred, QStringLiteral("/no/existe")).size(), 2);
    }
};

QTEST_GUILESS_MAIN(TestDupes)

#include "test_dupes.moc"
