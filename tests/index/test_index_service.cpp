// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/IndexService.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <mutex>

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

/// Un diario de cambios de mentira: la prueba dice qué carpetas cambiaron.
class FakeJournal : public ChangeJournal
{
public:
    QString volumeOf(const QString &) const override { return QStringLiteral("X:"); }

    core::Result<JournalPosition> position(const QString &) const override { return JournalPosition{7, now.load()}; }

    core::Result<JournalChanges> changesSince(const QString &, const JournalPosition &since,
                                              int maxFolders) const override
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        ++m_asked;
        m_lastSince = since;
        if (recreated) {
            return core::Error{core::ErrorCode::Corrupted, QStringLiteral("diario recreado"), QString(), 0};
        }
        if (changed.size() > maxFolders) {
            return core::Error{core::ErrorCode::Busy, QStringLiteral("demasiados"), QString(), 0};
        }
        return JournalChanges{changed, JournalPosition{7, now.load()}};
    }

    core::Result<VolumeCounts> counts(const QString &) const override { return VolumeCounts{100, 10}; }

    int asked() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_asked;
    }
    JournalPosition lastSince() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastSince;
    }

    std::atomic<qint64> now{100};
    // Se cambian solo con el servicio parado.
    QStringList changed;
    bool recreated = false;

private:
    mutable std::mutex m_mutex;
    mutable int m_asked = 0;
    mutable JournalPosition m_lastSince;
};

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

    void catchesUpWithTheChangeJournalInsteadOfWalkingTheDisk()
    {
        QTemporaryDir tree;
        QTemporaryDir data;
        const QString root = QDir::cleanPath(tree.path());
        writeFile(root + QStringLiteral("/a/viejo.txt"));
        writeFile(root + QStringLiteral("/b/viejo2.txt"));
        const auto journal = std::make_shared<FakeJournal>();
        const auto open = [&](IndexService &service) {
            service.setChangeJournal(journal);
            service.setExcludedNames({QStringLiteral("papelera")});
            service.addRoot(root);
        };
        {
            // La primera vez se recorre entera y se anota hasta dónde llegaba el diario.
            IndexService first(m_scheduler, data.path());
            open(first);
            QCOMPARE(first.statusOf(root)->state, RootStatus::State::Building);
            QVERIFY(waitReady(first, root));
            QCOMPARE(journal->asked(), 0);
            QCOMPARE(first.indexFor(root)->property(QStringLiteral("diario")), QStringLiteral("7:100"));
        }

        // Con la aplicación cerrada: cambian «a» y «b», pero el diario (de
        // mentira) solo cuenta «a». Si se recorriera el disco, saldría lo de «b».
        writeFile(root + QStringLiteral("/a/nuevo-a.txt"));
        writeFile(root + QStringLiteral("/b/nuevo-b.txt"));
        writeFile(root + QStringLiteral("/papelera/tirado.txt"));
        journal->now = 200;
        journal->changed = {root + QStringLiteral("/a"), root + QStringLiteral("/papelera"),
                            QStringLiteral("/fuera/de/la/raiz")};
        {
            IndexService second(m_scheduler, data.path());
            open(second);
            QCOMPARE(second.statusOf(root)->state, RootStatus::State::Updating);
            QVERIFY(waitReady(second, root));
            QCOMPARE(journal->asked(), 1);
            QCOMPARE(journal->lastSince().usn, qint64(100));
            const auto index = second.indexFor(root);
            QCOMPARE(find(*index, QStringLiteral("nuevo")), QStringList{root + QStringLiteral("/a/nuevo-a.txt")});
            QVERIFY(find(*index, QStringLiteral("tirado")).isEmpty());   // la papelera no se indexa
            // Solo «a»: lo de fuera de la raíz y la papelera se descartan.
            QCOMPARE(second.statusOf(root)->journalFolders, qint64(1));
            QCOMPARE(index->property(QStringLiteral("diario")), QStringLiteral("7:200"));
        }

        // Si el diario no sirve (se volvió a crear), se recorre entera como siempre.
        journal->recreated = true;
        {
            IndexService third(m_scheduler, data.path());
            open(third);
            QVERIFY(waitReady(third, root));
            QCOMPARE(journal->asked(), 2);
            QCOMPARE(third.statusOf(root)->journalFolders, qint64(-1));
            QCOMPARE(find(*third.indexFor(root), QStringLiteral("nuevo")),
                     (QStringList{root + QStringLiteral("/a/nuevo-a.txt"), root + QStringLiteral("/b/nuevo-b.txt")}));
        }

        // Sin diario (otro sistema, o el servicio sin instalar), lo de siempre.
        IndexService fourth(m_scheduler, data.path());
        fourth.setChangeJournal(nullptr);
        fourth.addRoot(root);
        QVERIFY(waitReady(fourth, root));
        QCOMPARE(journal->asked(), 2);
    }

    void doesNotFollowItsOwnWrites()
    {
        // Como al indexar la unidad del sistema: las bases del índice están
        // dentro de lo indexado. Antes, cada escritura del índice avisaba de un
        // cambio, que volvía a escribir… sin parar (visto en Windows con C:).
        QTemporaryDir tree;
        const QString root = QDir::cleanPath(tree.path());
        writeFile(root + QStringLiteral("/a/uno.txt"));
        writeFile(root + QStringLiteral("/cache-de-la-app/vieja.png"));
        IndexService service(m_scheduler, root + QStringLiteral("/datos-de-la-app"));
        service.setChangeJournal(nullptr);
        service.setIgnoredFolders({root + QStringLiteral("/cache-de-la-app")});
        service.addRoot(root);
        QVERIFY(waitReady(service, root));
        if (!service.statusOf(root)->watching) {
            QSKIP("Este sistema no avisa de los cambios.");
        }
        QSignalSpy changes(&service, &IndexService::changed);
        writeFile(root + QStringLiteral("/datos-de-la-app/otra-base.db"));
        writeFile(root + QStringLiteral("/cache-de-la-app/miniatura.png"));
        QTest::qWait(2000);
        QCOMPARE(changes.count(), 0);   // no se pone a trabajar
        QVERIFY(!service.isBusy());
        // Lo demás se sigue igual.
        writeFile(root + QStringLiteral("/a/dos.txt"));
        QTRY_VERIFY_WITH_TIMEOUT(!find(*service.indexFor(root), QStringLiteral("dos")).isEmpty(), 20000);
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
