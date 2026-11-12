// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/FileActions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace eu;
using namespace eu::ui;

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(content);
}

QString describe(const ActionResult &result)
{
    QString text;
    for (const core::Error &error : result.outcome.errors) {
        text += error.message + QStringLiteral(" [") + error.path + QStringLiteral("] ");
    }
    return text;
}

} // namespace

/// Recoge los resultados que llegan por la señal finished().
class Collector : public QObject
{
    Q_OBJECT

public:
    explicit Collector(FileActions &actions)
    {
        connect(&actions, &FileActions::finished, this, [this](const ActionResult &result) {
            results.append(result);
        });
    }

    /// Espera el resultado de la acción `id`.
    bool waitFor(quint64 id, ActionResult *result)
    {
        const bool arrived = QTest::qWaitFor([&] {
            for (const ActionResult &candidate : std::as_const(results)) {
                if (candidate.id == id) {
                    return true;
                }
            }
            return false;
        }, 30000);
        for (const ActionResult &candidate : std::as_const(results)) {
            if (candidate.id == id) {
                *result = candidate;
            }
        }
        return arrived;
    }

    QList<ActionResult> results;
};

class TestFileActions : public QObject
{
    Q_OBJECT

private:
    tasks::Scheduler m_scheduler;

    std::shared_ptr<journal::Journal> openJournal(const QTemporaryDir &directory)
    {
        auto journal = journal::Journal::open(directory.filePath(QStringLiteral("historial.db")));
        return journal.ok() ? journal.value() : nullptr;
    }

private slots:
    void createRenameAndUndo()
    {
        QTemporaryDir data;
        QTemporaryDir work;
        FileActions actions(m_scheduler, openJournal(data));
        QVERIFY(actions.isAvailable());
        Collector collector(actions);
        QSignalSpy startedSpy(&actions, &FileActions::started);
        ActionResult result;

        const quint64 createId = actions.createFolder(work.path(), QStringLiteral("Fotos"));
        QVERIFY(actions.isBusy());
        QVERIFY(collector.waitFor(createId, &result));
        QVERIFY2(result.outcome.ok(), qPrintable(describe(result)));
        QCOMPARE(result.kind, ActionKind::CreateFolder);
        QCOMPARE(result.directory, work.path());
        QCOMPARE(result.outcome.resultPaths, QStringList{work.filePath(QStringLiteral("Fotos"))});
        QVERIFY(QFileInfo(work.filePath(QStringLiteral("Fotos"))).isDir());
        QVERIFY(!actions.isBusy());
        QCOMPARE(startedSpy.count(), 1);

        const quint64 renameId = actions.rename(work.filePath(QStringLiteral("Fotos")), QStringLiteral("Viaje"));
        QVERIFY(collector.waitFor(renameId, &result));
        QVERIFY2(result.outcome.ok(), qPrintable(describe(result)));
        QCOMPARE(result.directory, QDir::cleanPath(work.path()));
        QVERIFY(QFileInfo(work.filePath(QStringLiteral("Viaje"))).isDir());
        QVERIFY(!QFileInfo::exists(work.filePath(QStringLiteral("Fotos"))));

        // Deshacer va de la más reciente a la más antigua: primero el
        // renombrado, luego la creación.
        QVERIFY(collector.waitFor(actions.undoLast(), &result));
        QVERIFY2(result.outcome.ok(), qPrintable(describe(result)));
        QVERIFY(!result.nothingToUndo);
        QVERIFY(result.undoneDescription.contains(QStringLiteral("Viaje")));
        QVERIFY(QFileInfo(work.filePath(QStringLiteral("Fotos"))).isDir());

        QVERIFY(collector.waitFor(actions.undoLast(), &result));
        QVERIFY2(result.outcome.ok(), qPrintable(describe(result)));
        QVERIFY(!QFileInfo::exists(work.filePath(QStringLiteral("Fotos"))));

        QVERIFY(collector.waitFor(actions.undoLast(), &result));
        QVERIFY(result.nothingToUndo);
    }

    void invalidNameIsReportedAndNothingChanges()
    {
        QTemporaryDir data;
        QTemporaryDir work;
        FileActions actions(m_scheduler, openJournal(data));
        Collector collector(actions);
        ActionResult result;

        QVERIFY(collector.waitFor(actions.createFolder(work.path(), QStringLiteral("a/b")), &result));
        QCOMPARE(result.outcome.errors.size(), 1);
        QCOMPARE(result.outcome.errors.first().code, core::ErrorCode::InvalidName);
        QVERIFY(QDir(work.path()).isEmpty());
    }

    void copyMoveTrashAndDelete()
    {
        QTemporaryDir data;
        QTemporaryDir work;
        writeFile(work.filePath(QStringLiteral("origen/uno.txt")), "uno");
        writeFile(work.filePath(QStringLiteral("origen/sub/dos.txt")), "dos");
        QVERIFY(QDir(work.path()).mkpath(QStringLiteral("destino")));
        const QString destination = work.filePath(QStringLiteral("destino"));

        FileActions actions(m_scheduler, openJournal(data));
        Collector collector(actions);
        ActionResult result;

        // Copiar una carpeta entera.
        QVERIFY(collector.waitFor(actions.copy({work.filePath(QStringLiteral("origen"))}, destination,
                                               ops::CollisionPolicy::AutoRename), &result));
        QVERIFY2(result.outcome.ok(), qPrintable(describe(result)));
        QVERIFY(QFileInfo::exists(destination + QStringLiteral("/origen/sub/dos.txt")));
        QVERIFY(QFileInfo::exists(work.filePath(QStringLiteral("origen/uno.txt"))));

        // Copiarla otra vez: no sobrescribe, crea otra con nombre distinto.
        QVERIFY(collector.waitFor(actions.copy({work.filePath(QStringLiteral("origen"))}, destination,
                                               ops::CollisionPolicy::AutoRename), &result));
        QVERIFY2(result.outcome.ok(), qPrintable(describe(result)));
        QCOMPARE(result.outcome.resultPaths.size(), 1);
        QVERIFY(result.outcome.resultPaths.first() != destination + QStringLiteral("/origen"));
        QCOMPARE(QDir(destination).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 2);

        // Mover un archivo.
        QVERIFY(collector.waitFor(actions.move({work.filePath(QStringLiteral("origen/uno.txt"))}, destination,
                                               ops::CollisionPolicy::Skip), &result));
        QVERIFY2(result.outcome.ok(), qPrintable(describe(result)));
        QVERIFY(QFileInfo::exists(destination + QStringLiteral("/uno.txt")));
        QVERIFY(!QFileInfo::exists(work.filePath(QStringLiteral("origen/uno.txt"))));

        // A la papelera y de vuelta con Deshacer.
        const QString moved = destination + QStringLiteral("/uno.txt");
        QVERIFY(collector.waitFor(actions.trash({moved}), &result));
        if (result.outcome.ok()) {
            QVERIFY(!QFileInfo::exists(moved));
            QVERIFY(collector.waitFor(actions.undoLast(), &result));
            QVERIFY2(result.outcome.ok(), qPrintable(describe(result)));
            QVERIFY(QFileInfo::exists(moved));
        } else {
            // Hay volúmenes temporales sin sitio para la papelera: entonces el
            // archivo debe seguir intacto y el error decirlo.
            QCOMPARE(result.outcome.errors.first().code, core::ErrorCode::NotSupported);
            QVERIFY(QFileInfo::exists(moved));
        }

        // Eliminación permanente.
        QVERIFY(collector.waitFor(actions.deletePermanently({destination}), &result));
        QVERIFY2(result.outcome.ok(), qPrintable(describe(result)));
        QVERIFY(!QFileInfo::exists(destination));
    }

    void progressArrivesAndCancelStops()
    {
        QTemporaryDir data;
        QTemporaryDir work;
        // Muchos archivos pequeños: da tiempo a cancelar a mitad.
        for (int i = 0; i < 3000; ++i) {
            writeFile(work.filePath(QStringLiteral("origen/archivo%1.bin").arg(i)), QByteArray(2048, 'x'));
        }
        QVERIFY(QDir(work.path()).mkpath(QStringLiteral("destino")));

        FileActions actions(m_scheduler, openJournal(data));
        Collector collector(actions);
        QSignalSpy progressSpy(&actions, &FileActions::progressed);
        ActionResult result;

        const quint64 id = actions.copy({work.filePath(QStringLiteral("origen"))},
                                        work.filePath(QStringLiteral("destino")), ops::CollisionPolicy::Skip);
        QVERIFY(QTest::qWaitFor([&] { return progressSpy.count() > 0 || !actions.isBusy(); }, 30000));
        actions.cancelAll();
        QVERIFY(collector.waitFor(id, &result));
        // Según la velocidad del disco pudo terminar antes de cancelarse; lo
        // que no puede pasar es que quede a medias sin decirlo.
        if (result.outcome.cancelled) {
            QVERIFY(QDir(work.filePath(QStringLiteral("destino/origen"))).entryList(QDir::Files).size() < 3000);
        } else {
            QVERIFY2(result.outcome.ok(), qPrintable(describe(result)));
            QCOMPARE(QDir(work.filePath(QStringLiteral("destino/origen"))).entryList(QDir::Files).size(), 3000);
        }
        // El original no se toca en ningún caso.
        QCOMPARE(QDir(work.filePath(QStringLiteral("origen"))).entryList(QDir::Files).size(), 3000);
    }

    void withoutJournalNothingIsDone()
    {
        QTemporaryDir work;
        FileActions actions(m_scheduler, nullptr);
        QVERIFY(!actions.isAvailable());
        Collector collector(actions);
        ActionResult result;
        QVERIFY(collector.waitFor(actions.createFolder(work.path(), QStringLiteral("Nueva")), &result));
        QCOMPARE(result.outcome.errors.size(), 1);
        QVERIFY(QDir(work.path()).isEmpty());
    }

    void destroyingWhileBusyIsSafe()
    {
        QTemporaryDir data;
        QTemporaryDir work;
        for (int i = 0; i < 500; ++i) {
            writeFile(work.filePath(QStringLiteral("origen/archivo%1.bin").arg(i)), QByteArray(4096, 'x'));
        }
        QVERIFY(QDir(work.path()).mkpath(QStringLiteral("destino")));
        {
            FileActions actions(m_scheduler, openJournal(data));
            actions.copy({work.filePath(QStringLiteral("origen"))}, work.filePath(QStringLiteral("destino")),
                         ops::CollisionPolicy::Skip);
        }
        // La tarea sigue o se cancela por su cuenta; su aviso final llega a
        // un objeto que ya no existe y debe descartarse sin más.
        QTest::qWait(300);
        QCOMPARE(QDir(work.filePath(QStringLiteral("origen"))).entryList(QDir::Files).size(), 500);
    }
};

QTEST_GUILESS_MAIN(TestFileActions)
#include "test_file_actions.moc"
