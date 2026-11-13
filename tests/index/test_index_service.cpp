// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/IndexService.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace eu;
using namespace eu::index;

namespace {

void writeFile(const QString &path, qsizetype bytes = 1)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(QByteArray(bytes, 'x'));
}

QStringList find(const Index &index, const QString &text)
{
    QStringList paths;
    const auto query = search::Query::parse(text);
    index.search({index.root()}, query.value(), {}, {},
                 [&](std::vector<search::Hit> &&hits, const search::SearchProgress &) {
        for (const search::Hit &hit : hits) {
            paths.append(hit.path());
        }
    });
    paths.sort();
    return paths;
}

bool waitReady(IndexService &service, const QString &root)
{
    return QTest::qWaitFor([&] {
        const auto status = service.statusOf(root);
        return status && status->state == RootStatus::State::Ready && !service.isBusy();
    }, 30000);
}

} // namespace

class TestIndexService : public QObject
{
    Q_OBJECT

private:
    tasks::Scheduler m_scheduler;

private slots:
    void buildsFollowsAndRemoves()
    {
        QTemporaryDir tree;
        QTemporaryDir data;
        const QString root = QDir::cleanPath(tree.path());
        writeFile(root + QStringLiteral("/a/uno.txt"));
        writeFile(root + QStringLiteral("/a/b/dos.txt"));
        writeFile(root + QStringLiteral("/papelera/tirado.txt"));

        IndexService service(m_scheduler, data.path());
        service.setExcludedNames({QStringLiteral("papelera")});
        QSignalSpy changes(&service, &IndexService::changed);
        QVERIFY(!service.indexFor(root));

        service.addRoot(root);
        QCOMPARE(service.roots(), QStringList{root});
        QCOMPARE(service.statusOf(root)->state, RootStatus::State::Building);
        // Mientras se construye no sirve para buscar.
        QVERIFY(!service.indexFor(root));
        QVERIFY(waitReady(service, root));
        QVERIFY(!changes.isEmpty());
        QCOMPARE(service.statusOf(root)->info.entries, 4);   // a, uno.txt, b, dos.txt

        const auto index = service.indexFor(root + QStringLiteral("/a/b"));
        QVERIFY(index.has_value());
        QCOMPARE(index->root(), root);
        QVERIFY(!service.indexFor(QDir::cleanPath(data.path())));
        QCOMPARE(find(*index, QStringLiteral("txt")),
                 (QStringList{root + QStringLiteral("/a/b/dos.txt"), root + QStringLiteral("/a/uno.txt")}));

        if (!service.statusOf(root)->watching) {
            QSKIP("Este sistema no avisa de los cambios: el índice se pone al día a mano.");
        }
        // Un archivo nuevo aparece solo en el índice.
        writeFile(root + QStringLiteral("/a/b/nuevo.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(!find(*index, QStringLiteral("nuevo")).isEmpty(), 20000);
        // Una carpeta nueva con contenido, y lo que se crea después dentro.
        writeFile(root + QStringLiteral("/c/d/hondo.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(!find(*index, QStringLiteral("hondo")).isEmpty(), 20000);
        writeFile(root + QStringLiteral("/c/d/despues.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(!find(*index, QStringLiteral("despues")).isEmpty(), 20000);
        // Borrar una carpeta se lleva lo que colgaba.
        QVERIFY(QDir(root + QStringLiteral("/a/b")).removeRecursively());
        QTRY_VERIFY_WITH_TIMEOUT(find(*index, QStringLiteral("dos")).isEmpty(), 20000);

        // En pausa no se toca el índice; al reanudar se pone al día.
        service.setPaused(true);
        QVERIFY(service.isPaused());
        writeFile(root + QStringLiteral("/a/en-pausa.txt"));
        QTest::qWait(1500);
        QVERIFY(find(*index, QStringLiteral("en-pausa")).isEmpty());
        service.setPaused(false);
        QTRY_VERIFY_WITH_TIMEOUT(!find(*index, QStringLiteral("en-pausa")).isEmpty(), 20000);

        // Quitarla borra la base.
        const QString database = service.databasePathFor(root);
        QVERIFY(QFile::exists(database));
        service.removeRoot(root);
        QVERIFY(service.roots().isEmpty());
        QVERIFY(!service.indexFor(root));
        QVERIFY(!QFile::exists(database));
    }

    void anExistingIndexIsUsableAtOnceAndCaughtUp()
    {
        QTemporaryDir tree;
        QTemporaryDir data;
        const QString root = QDir::cleanPath(tree.path());
        writeFile(root + QStringLiteral("/viejo.txt"));
        {
            IndexService first(m_scheduler, data.path());
            first.addRoot(root);
            QVERIFY(waitReady(first, root));
        }
        // Con la aplicación cerrada cambian cosas.
        writeFile(root + QStringLiteral("/mientras/cerrado.txt"));
        QVERIFY(QFile::remove(root + QStringLiteral("/viejo.txt")));

        IndexService second(m_scheduler, data.path());
        second.addRoot(root);
        // Sirve desde el primer momento (con lo que sabía)…
        QCOMPARE(second.statusOf(root)->state, RootStatus::State::Updating);
        const auto index = second.indexFor(root);
        QVERIFY(index.has_value());
        // …y se pone al día sola.
        QVERIFY(waitReady(second, root));
        QCOMPARE(find(*index, QStringLiteral("txt")), QStringList{root + QStringLiteral("/mientras/cerrado.txt")});

        // Reconstruir empieza de cero.
        second.rebuild(root);
        QCOMPARE(second.statusOf(root)->state, RootStatus::State::Building);
        QVERIFY(waitReady(second, root));
        QCOMPARE(second.statusOf(root)->info.entries, 2);
    }

    void anUnmountedRootKeepsItsIndex()
    {
        QTemporaryDir tree;
        QTemporaryDir data;
        const QString root = QDir::cleanPath(tree.path()) + QStringLiteral("/unidad");
        writeFile(root + QStringLiteral("/dato.txt"));
        IndexService service(m_scheduler, data.path());
        service.addRoot(root);
        QVERIFY(waitReady(service, root));
        // La unidad se desmonta: queda el punto de montaje vacío.
        QVERIFY(QFile::remove(root + QStringLiteral("/dato.txt")));
        service.refresh(root);
        QTRY_COMPARE_WITH_TIMEOUT(service.statusOf(root)->state, RootStatus::State::Disconnected, 20000);
        QCOMPARE(service.statusOf(root)->info.entries, 1);
        QVERIFY(service.indexFor(root).has_value());
    }
};

QTEST_GUILESS_MAIN(TestIndexService)
#include "test_index_service.moc"
