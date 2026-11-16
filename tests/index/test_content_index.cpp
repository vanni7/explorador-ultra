// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/ContentIndex.h"
#include "eu/index/ContentIndexService.h"

#include "eu/content/TextExtractor.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>

using namespace eu;
using namespace eu::index;

namespace {

void writeText(const QString &path, const QString &text, qint64 shiftSeconds = 0)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(text.toUtf8());
    if (shiftSeconds != 0) {
        file.setFileTime(QDateTime::currentDateTime().addSecs(shiftSeconds), QFileDevice::FileModificationTime);
    }
}

struct Found
{
    QStringList paths;
    search::SearchProgress progress;
};

Found find(const QString &root, const QString &text, const ContentIndex *index, bool caseSensitive = false,
           const TextProvider &provider = {})
{
    Found found;
    search::QueryOptions queryOptions;
    queryOptions.caseSensitive = caseSensitive;
    const auto query = search::Query::parse(text, queryOptions);
    search::SearchOptions options;
    options.textProvider = provider;
    if (index != nullptr) {
        const ContentIndex copy = *index;
        options.contentCache = [copy] { return ContentIndex::cacheFor({copy}); };
    }
    search::searchTree({root}, query.value(), options, {}, nullptr,
                       [&](std::vector<search::Hit> &&hits, const search::SearchProgress &progress) {
        for (const search::Hit &hit : hits) {
            found.paths.append(QDir(root).relativeFilePath(hit.path()));
        }
        found.progress = progress;
    });
    found.paths.sort();
    return found;
}

} // namespace

class TestContentIndex : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_data;
    int m_count = 0;
    QString newDatabase() { return m_data.filePath(QStringLiteral("c%1.db").arg(++m_count)); }

private slots:
    void sameResultsAsReadingTheDisk()
    {
        QTemporaryDir tree;
        const QString root = QDir::cleanPath(tree.path());
        writeText(root + QStringLiteral("/actas/enero.txt"), QStringLiteral("Se aprobó el Presupuesto anual."));
        writeText(root + QStringLiteral("/actas/febrero.md"), QStringLiteral("Acta de febrero: sin novedades."));
        writeText(root + QStringLiteral("/notas/ideas.txt"), QStringLiteral("presupuesto pequeño; café y ñandú"));
        writeText(root + QStringLiteral("/notas/otra.txt"), QStringLiteral("Nada que ver aquí."));
        writeText(root + QStringLiteral("/datos.bin"), QStringLiteral("presupuesto en un binario"));

        const auto opened = ContentIndex::open(newDatabase(), root);
        QVERIFY(opened.ok());
        const ContentIndex index = opened.value();
        const auto synced = index.sync({}, {});
        QVERIFY2(synced.ok(), synced.ok() ? "" : qPrintable(synced.error().message));
        QCOMPARE(synced.value().found, 4);   // el .bin no es un tipo con texto
        QCOMPARE(synced.value().read, 4);
        QCOMPARE(index.info().documents, 4);
        QCOMPARE(index.info().withText, 4);
        QVERIFY(index.info().complete);

        const QStringList queries = {
            QStringLiteral("content:presupuesto"),
            QStringLiteral("content:\"presupuesto anual\""),
            QStringLiteral("content:pre"),                       // justo 3 caracteres
            QStringLiteral("content:aq"),                        // menos de 3: sin filtro previo
            QStringLiteral("content:presupuesto OR content:acta"),
            QStringLiteral("content:presupuesto content:café"),
            QStringLiteral("NOT content:presupuesto ext:txt"),
            QStringLiteral("content:ÑANDÚ"),
            QStringLiteral("content:zzzz"),
        };
        for (const QString &query : queries) {
            const Found disk = find(root, query, nullptr);
            const Found indexed = find(root, query, &index);
            QVERIFY2(disk.paths == indexed.paths,
                     qPrintable(query + QStringLiteral(": disco ") + disk.paths.join(QLatin1Char(','))
                                + QStringLiteral(" / índice ") + indexed.paths.join(QLatin1Char(','))));
            QCOMPARE(indexed.progress.filesRead, 0);   // nada leído del disco
        }
        QCOMPARE(find(root, QStringLiteral("content:presupuesto"), &index).paths,
                 (QStringList{QStringLiteral("actas/enero.txt"), QStringLiteral("notas/ideas.txt")}));
        // Distinguiendo mayúsculas, también lo mismo.
        QCOMPARE(find(root, QStringLiteral("content:Presupuesto"), &index, true).paths,
                 QStringList{QStringLiteral("actas/enero.txt")});
        QCOMPARE(find(root, QStringLiteral("content:Presupuesto"), nullptr, true).paths,
                 QStringList{QStringLiteral("actas/enero.txt")});
    }

    void changesAreReadFromDiskUntilTheNextSync()
    {
        QTemporaryDir tree;
        const QString root = QDir::cleanPath(tree.path());
        writeText(root + QStringLiteral("/a.txt"), QStringLiteral("versión vieja"));
        writeText(root + QStringLiteral("/b.txt"), QStringLiteral("se queda igual"));
        writeText(root + QStringLiteral("/c.txt"), QStringLiteral("se va a borrar"));
        const ContentIndex index = ContentIndex::open(newDatabase(), root).value();
        QVERIFY(index.sync({}, {}).ok());

        // Cambia uno (otro tamaño y otra fecha), aparece otro y se borra otro.
        writeText(root + QStringLiteral("/a.txt"), QStringLiteral("versión nueva y más larga"), 5);
        writeText(root + QStringLiteral("/d.txt"), QStringLiteral("versión recién llegada"));
        QVERIFY(QFile::remove(root + QStringLiteral("/c.txt")));
        // Sin repasar el índice, la búsqueda ya ve lo nuevo: lo lee del disco.
        const Found before = find(root, QStringLiteral("content:versión"), &index);
        QCOMPARE(before.paths, (QStringList{QStringLiteral("a.txt"), QStringLiteral("d.txt")}));
        QCOMPARE(before.progress.filesRead, 2);
        QVERIFY(find(root, QStringLiteral("content:vieja"), &index).paths.isEmpty());

        const auto again = index.sync({}, {});
        QVERIFY(again.ok());
        QCOMPARE(again.value().toRead, 2);
        QCOMPARE(again.value().removed, 1);
        QCOMPARE(index.info().documents, 3);
        const Found after = find(root, QStringLiteral("content:versión"), &index);
        QCOMPARE(after.paths, (QStringList{QStringLiteral("a.txt"), QStringLiteral("d.txt")}));
        QCOMPARE(after.progress.filesRead, 0);

        // Sin cambios, repasar no lee nada.
        QCOMPARE(index.sync({}, {}).value().toRead, 0);
    }

    void documentsWithoutTextAreRemembered()
    {
        QTemporaryDir tree;
        const QString root = QDir::cleanPath(tree.path());
        writeText(root + QStringLiteral("/bueno.txt"), QStringLiteral("texto con dato"));
        writeText(root + QStringLiteral("/roto.txt"), QStringLiteral("dato que no se puede leer"));
        std::atomic<int> calls{0};
        const TextProvider provider = [&](const QString &path, const tasks::CancelToken &cancel)
            -> core::Result<QString> {
            ++calls;
            if (path.endsWith(QStringLiteral("roto.txt"))) {
                return core::Error{core::ErrorCode::Corrupted, QStringLiteral("dañado"), path, 0};
            }
            return content::extractText(path, cancel);
        };
        const ContentIndex index = ContentIndex::open(newDatabase(), root).value();
        ContentSyncOptions options;
        options.textProvider = provider;
        const auto synced = index.sync(options, {});
        QVERIFY(synced.ok());
        QCOMPARE(synced.value().failed, 1);
        QCOMPARE(index.info().withText, 1);
        calls = 0;
        const Found found = find(root, QStringLiteral("content:dato"), &index, false, provider);
        QCOMPARE(found.paths, QStringList{QStringLiteral("bueno.txt")});
        QCOMPARE(calls.load(), 0);   // el dañado no se vuelve a intentar mientras no cambie
        QCOMPARE(find(root, QStringLiteral("content:dato"), nullptr, false, provider).paths, found.paths);
    }

    void aMissingDriveDoesNotEraseTheIndex()
    {
        QTemporaryDir tree;
        const QString root = QDir::cleanPath(tree.path()) + QStringLiteral("/unidad");
        writeText(root + QStringLiteral("/libro.txt"), QStringLiteral("capítulo uno"));
        const ContentIndex index = ContentIndex::open(newDatabase(), root).value();
        QVERIFY(index.sync({}, {}).ok());
        // Punto de montaje vacío (unidad sin montar).
        QVERIFY(QFile::rename(root + QStringLiteral("/libro.txt"), tree.filePath(QStringLiteral("fuera.txt"))));
        auto synced = index.sync({}, {});
        QVERIFY(!synced.ok());
        QCOMPARE(synced.error().code, core::ErrorCode::Disconnected);
        QCOMPARE(index.info().documents, 1);
        // Ni siquiera está la carpeta.
        QVERIFY(QDir(root).removeRecursively());
        synced = index.sync({}, {});
        QVERIFY(!synced.ok());
        QCOMPARE(synced.error().code, core::ErrorCode::Disconnected);
        QCOMPARE(index.info().documents, 1);
    }

    void interruptedSyncKeepsWhatItRead()
    {
        QTemporaryDir tree;
        const QString root = QDir::cleanPath(tree.path());
        for (int i = 0; i < 60; ++i) {
            writeText(root + QStringLiteral("/doc%1.txt").arg(i), QStringLiteral("documento %1").arg(i));
        }
        const ContentIndex index = ContentIndex::open(newDatabase(), root).value();
        tasks::CancelSource source;
        std::atomic<int> calls{0};
        ContentSyncOptions options;
        options.textProvider = [&](const QString &path, const tasks::CancelToken &cancel) {
            if (++calls == 45) {
                source.cancel();   // como si se cerrara la aplicación a mitad
            }
            return content::extractText(path, cancel);
        };
        const auto first = index.sync(options, source.token());
        QVERIFY(!first.ok());
        QCOMPARE(first.error().code, core::ErrorCode::Cancelled);
        const qint64 kept = index.info().documents;
        QVERIFY2(kept >= 40, qPrintable(QString::number(kept)));
        QVERIFY(!index.info().complete);
        calls = 0;
        options.textProvider = {};
        const auto second = index.sync(options, {});
        QVERIFY(second.ok());
        QCOMPARE(second.value().toRead, 60 - kept);
        QCOMPARE(index.info().documents, 60);
        QVERIFY(index.info().complete);
    }

    void reopeningAnotherFolderStartsAfresh()
    {
        QTemporaryDir one;
        QTemporaryDir two;
        writeText(one.filePath(QStringLiteral("x.txt")), QStringLiteral("uno"));
        const QString database = newDatabase();
        QVERIFY(ContentIndex::open(database, one.path()).value().sync({}, {}).ok());
        QCOMPARE(ContentIndex::open(database, one.path()).value().info().documents, 1);
        QCOMPARE(ContentIndex::open(database, two.path()).value().info().documents, 0);
    }

    void serviceBuildsAndOffersTheCache()
    {
        tasks::Scheduler scheduler;
        QTemporaryDir tree;
        QTemporaryDir data;
        const QString root = QDir::cleanPath(tree.path()) + QStringLiteral("/biblioteca");
        writeText(root + QStringLiteral("/libro.txt"), QStringLiteral("érase una vez"));
        ContentIndexService service(scheduler, data.path());
        service.addRoot(root);
        QVERIFY(QTest::qWaitFor([&] {
            const auto all = service.status();
            return !all.isEmpty() && all.first().state == ContentRootStatus::State::Ready && !service.isBusy();
        }, 30000));
        QCOMPARE(service.status().first().info.documents, 1);
        // Sirve buscando en ella, dentro de ella o en la carpeta que la contiene;
        // no en otra que no tiene que ver.
        QVERIFY(service.cacheFactory({root}));
        QVERIFY(service.cacheFactory({root + QStringLiteral("/sub")}));
        QVERIFY(service.cacheFactory({QDir::cleanPath(tree.path())}));
        QVERIFY(!service.cacheFactory({data.path()}));
        const QString database = service.databasePathFor(root);
        QVERIFY(QFile::exists(database));
        service.removeRoot(root);
        QVERIFY(service.roots().isEmpty());
        QVERIFY(!QFile::exists(database));
    }
};

QTEST_GUILESS_MAIN(TestContentIndex)

#include "test_content_index.moc"
