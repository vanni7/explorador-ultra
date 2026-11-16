// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ops/Engine.h"

#include "eu/fs/Volumes.h"
#include "eu/ops/FileOps.h"
#include "eu/trash/Trash.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>

using namespace eu;
using namespace eu::ops;

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(content);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<no se pudo leer>");
}

/// Rutas relativas de todo lo que hay bajo una carpeta, ordenadas.
QStringList treeOf(const QString &root)
{
    QStringList result;
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        result.append(QDir(root).relativeFilePath(it.next()));
    }
    result.sort();
    return result;
}

QString describe(const Outcome &outcome)
{
    QString text;
    for (const core::Error &error : outcome.errors) {
        text += error.message + QStringLiteral(" [") + error.path + QStringLiteral("] ");
    }
    return text;
}

} // namespace

class TestEngine : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_journalDirectory;
    std::shared_ptr<journal::Journal> m_journal;
    const tasks::CancelToken m_noCancel;

    /// Una segunda carpeta temporal en otro volumen, o cadena vacía si este
    /// equipo no tiene dos volúmenes escribibles a mano.
    QString otherVolumeBase(const QString &reference) const
    {
        const QStringList candidates{
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation),
            QDir::tempPath(),
            QStringLiteral("/dev/shm"),
            QStringLiteral("/var/tmp"),
        };
        for (const QString &candidate : candidates) {
            if (!candidate.isEmpty() && QDir().mkpath(candidate) && QFileInfo(candidate).isWritable()
                && !fs::isSameVolume(candidate, reference)) {
                return candidate;
            }
        }
        return {};
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_journalDirectory.isValid());
        auto opened = journal::Journal::open(m_journalDirectory.filePath(QStringLiteral("historial.db")));
        QVERIFY(opened.ok());
        m_journal = opened.value();
    }

    void createRenameAndUndoThem()
    {
        QTemporaryDir directory;
        Engine engine(m_journal);

        const Outcome folder = engine.createFolder(directory.path(), QStringLiteral("Nueva carpeta"));
        QVERIFY2(folder.ok(), qPrintable(describe(folder)));
        QVERIFY(QFileInfo(directory.filePath(QStringLiteral("Nueva carpeta"))).isDir());

        const Outcome file = engine.createFile(directory.path(), QStringLiteral("notas.txt"));
        QVERIFY(file.ok());

        const Outcome renamed = engine.rename(directory.filePath(QStringLiteral("notas.txt")),
                                              QStringLiteral("apuntes.txt"));
        QVERIFY(renamed.ok());
        QCOMPARE(renamed.resultPaths, QStringList{directory.filePath(QStringLiteral("apuntes.txt"))});
        QCOMPARE(treeOf(directory.path()), (QStringList{QStringLiteral("Nueva carpeta"), QStringLiteral("apuntes.txt")}));

        // Deshacer en orden inverso deja la carpeta como al principio.
        QVERIFY(engine.undo(renamed.operationId, m_noCancel).ok());
        QCOMPARE(treeOf(directory.path()), (QStringList{QStringLiteral("Nueva carpeta"), QStringLiteral("notas.txt")}));
        QVERIFY(engine.undo(file.operationId, m_noCancel).ok());
        QVERIFY(engine.undo(folder.operationId, m_noCancel).ok());
        QVERIFY(treeOf(directory.path()).isEmpty());
        QCOMPARE(m_journal->operation(folder.operationId).value().state, journal::OperationState::Undone);
        QVERIFY(!Engine::canUndo(m_journal->operation(folder.operationId).value()));
    }

    void invalidNamesAndCollisionsAreRejected()
    {
        QTemporaryDir directory;
        Engine engine(m_journal);
        const Outcome slash = engine.createFolder(directory.path(), QStringLiteral("a/b"));
        QCOMPARE(slash.errors.first().code, core::ErrorCode::InvalidName);
        QCOMPARE(slash.operationId, journal::OperationId(0));   // ni siquiera se anotó
        QCOMPARE(engine.createFile(directory.path(), QString()).errors.first().code, core::ErrorCode::InvalidName);

        writeFile(directory.filePath(QStringLiteral("a")), "soy a");
        writeFile(directory.filePath(QStringLiteral("b")), "soy b");
        const Outcome collision = engine.rename(directory.filePath(QStringLiteral("a")), QStringLiteral("b"));
        QCOMPARE(collision.errors.first().code, core::ErrorCode::AlreadyExists);
        QCOMPARE(readFile(directory.filePath(QStringLiteral("b"))), QByteArray("soy b"));
        QCOMPARE(m_journal->operation(collision.operationId).value().state, journal::OperationState::Failed);

        QCOMPARE(engine.createFolder(directory.path(), QStringLiteral("a")).errors.first().code,
                 core::ErrorCode::AlreadyExists);
    }

    void copyTreeWithProgressThenUndo()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("origen"));
        writeFile(source + QStringLiteral("/a.txt"), "aaa");
        writeFile(source + QStringLiteral("/sub/b ñ.bin"), QByteArray(300'000, 'b'));
        QVERIFY(QDir().mkpath(source + QStringLiteral("/vacía")));
#ifdef Q_OS_UNIX
        QVERIFY(QFile::link(QStringLiteral("a.txt"), source + QStringLiteral("/enlace")));
#endif
        writeFile(directory.filePath(QStringLiteral("suelto.txt")), "suelto");
        const QString destination = directory.filePath(QStringLiteral("destino"));
        QVERIFY(QDir().mkdir(destination));

        Engine engine(m_journal);
        Progress last;
        const Outcome outcome = engine.copy({source, directory.filePath(QStringLiteral("suelto.txt"))}, destination,
                                            CollisionPolicy::Skip, m_noCancel,
                                            [&](const Progress &progress) { last = progress; });
        QVERIFY2(outcome.ok(), qPrintable(describe(outcome)));
        QCOMPARE(treeOf(destination + QStringLiteral("/origen")), treeOf(source));
        QCOMPARE(readFile(destination + QStringLiteral("/origen/sub/b ñ.bin")), QByteArray(300'000, 'b'));
        QCOMPARE(readFile(destination + QStringLiteral("/suelto.txt")), QByteArray("suelto"));
#ifdef Q_OS_UNIX
        QVERIFY(stampOf(destination + QStringLiteral("/origen/enlace")).isSymlink);   // copiado como enlace
#endif
        QCOMPARE(last.bytesDone, last.bytesTotal);
        QCOMPARE(last.bytesTotal, qint64(300'000 + 3 + 6));
        QCOMPARE(outcome.resultPaths.size(), 2);

        // Los originales siguen ahí; deshacer borra solo las copias.
        QVERIFY(engine.undo(outcome.operationId, m_noCancel).ok());
        QVERIFY(treeOf(destination).isEmpty());
        QCOMPARE(readFile(source + QStringLiteral("/a.txt")), QByteArray("aaa"));
    }

    void copyCollisionPolicies()
    {
        QTemporaryDir directory;
        const QString file = directory.filePath(QStringLiteral("foto.jpg"));
        writeFile(file, "original");
        const QString destination = directory.filePath(QStringLiteral("destino"));
        writeFile(destination + QStringLiteral("/foto.jpg"), "ya estaba");

        Engine engine(m_journal);
        const Outcome skipped = engine.copy({file}, destination, CollisionPolicy::Skip, m_noCancel);
        QVERIFY(skipped.ok());
        QCOMPARE(skipped.skipped, QStringList{file});
        QCOMPARE(readFile(destination + QStringLiteral("/foto.jpg")), QByteArray("ya estaba"));

        const Outcome renamed = engine.copy({file}, destination, CollisionPolicy::AutoRename, m_noCancel);
        QVERIFY(renamed.ok());
        QCOMPARE(readFile(destination + QStringLiteral("/foto (2).jpg")), QByteArray("original"));
        QCOMPARE(readFile(destination + QStringLiteral("/foto.jpg")), QByteArray("ya estaba"));

        // Copiar en la misma carpeta duplica con otro nombre, sea cual sea la política.
        const Outcome duplicate = engine.copy({file}, directory.path(), CollisionPolicy::Skip, m_noCancel);
        QVERIFY(duplicate.ok());
        QCOMPARE(readFile(directory.filePath(QStringLiteral("foto (2).jpg"))), QByteArray("original"));
    }

    void overwriteSendsTheOldOneToTheTrashAndUndoes()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("informe.txt"));
        const QString destination = directory.filePath(QStringLiteral("destino"));
        const QString target = destination + QStringLiteral("/informe.txt");
        writeFile(source, "versión nueva");
        writeFile(target, "versión vieja");

        Engine engine(m_journal);
        const Outcome replaced = engine.copy({source}, destination, CollisionPolicy::Overwrite, m_noCancel);
        if (!replaced.errors.isEmpty() && replaced.errors.first().code == core::ErrorCode::NotSupported) {
            QSKIP("El volumen de la carpeta temporal no admite papelera en este equipo.");
        }
        QVERIFY2(replaced.ok(), qPrintable(describe(replaced)));
        QCOMPARE(replaced.replaced, QStringList{target});
        QCOMPARE(readFile(target), QByteArray("versión nueva"));
        QCOMPARE(treeOf(destination), QStringList{QStringLiteral("informe.txt")});

        // Deshacer quita la copia y devuelve lo que había.
        const Outcome undone = engine.undo(replaced.operationId, m_noCancel);
        QVERIFY2(undone.ok(), qPrintable(describe(undone)));
        QCOMPARE(readFile(target), QByteArray("versión vieja"));
        QCOMPARE(readFile(source), QByteArray("versión nueva"));

        // Mover reemplazando: el origen desaparece y se puede deshacer igual.
        const Outcome moved = engine.move({source}, destination, CollisionPolicy::Overwrite, m_noCancel);
        QVERIFY2(moved.ok(), qPrintable(describe(moved)));
        QVERIFY(!QFileInfo::exists(source));
        QCOMPARE(readFile(target), QByteArray("versión nueva"));
        QVERIFY(engine.undo(moved.operationId, m_noCancel).ok());
        QCOMPARE(readFile(source), QByteArray("versión nueva"));
        QCOMPARE(readFile(target), QByteArray("versión vieja"));

        // Una carpeta no se reemplaza por algo de dentro de ella.
        writeFile(directory.filePath(QStringLiteral("x/x")), "dentro");
        const Outcome inside = engine.copy({directory.filePath(QStringLiteral("x/x"))}, directory.path(),
                                           CollisionPolicy::Overwrite, m_noCancel);
        QCOMPARE(inside.errors.size(), 1);
        QCOMPARE(readFile(directory.filePath(QStringLiteral("x/x"))), QByteArray("dentro"));
    }

    void overwriteIfNewerAndDatedNames()
    {
        QTemporaryDir directory;
        const QString destination = directory.filePath(QStringLiteral("destino"));
        const QString older = directory.filePath(QStringLiteral("viejo/a.txt"));
        const QString newer = directory.filePath(QStringLiteral("nuevo/a.txt"));
        writeFile(older, "viejo");
        writeFile(newer, "nuevo");
        writeFile(destination + QStringLiteral("/a.txt"), "destino");
        const auto setDate = [](const QString &path, const QDateTime &when) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(when, QFileDevice::FileModificationTime));
        };
        const QDateTime now = QDateTime::currentDateTimeUtc();
        setDate(older, now.addDays(-10));
        setDate(destination + QStringLiteral("/a.txt"), now.addDays(-5));
        setDate(newer, now.addDays(-1));

        Engine engine(m_journal);
        const Outcome notNewer = engine.copy({older}, destination, CollisionPolicy::OverwriteIfNewer, m_noCancel);
        QVERIFY(notNewer.ok());
        QCOMPARE(notNewer.skipped, QStringList{older});
        QCOMPARE(readFile(destination + QStringLiteral("/a.txt")), QByteArray("destino"));
        const Outcome isNewer = engine.copy({newer}, destination, CollisionPolicy::OverwriteIfNewer, m_noCancel);
        if (!isNewer.errors.isEmpty() && isNewer.errors.first().code == core::ErrorCode::NotSupported) {
            QSKIP("El volumen de la carpeta temporal no admite papelera en este equipo.");
        }
        QVERIFY2(isNewer.ok(), qPrintable(describe(isNewer)));
        QCOMPARE(readFile(destination + QStringLiteral("/a.txt")), QByteArray("nuevo"));

        // Con la fecha del original en el nombre.
        const Outcome dated = engine.copy({older}, destination, CollisionPolicy::AppendDateTime, m_noCancel);
        QVERIFY(dated.ok());
        const QString expected = nameWithDateTime(QStringLiteral("a.txt"), now.addDays(-10).toMSecsSinceEpoch());
        QVERIFY2(expected.startsWith(QStringLiteral("a ")) && expected.endsWith(QStringLiteral(".txt")), qPrintable(expected));
        QVERIFY(!expected.contains(QLatin1Char(':')));
        QCOMPARE(dated.resultPaths, QStringList{destination + QLatin1Char('/') + expected});
        QCOMPARE(readFile(destination + QLatin1Char('/') + expected), QByteArray("viejo"));
    }

    void identicalFilesAreSkippedOthersKept()
    {
        QTemporaryDir directory;
        // En el destino ya hay una carpeta "origen" con dos de los archivos.
        const QString destination = directory.filePath(QStringLiteral("dest2/origen"));
        writeFile(directory.filePath(QStringLiteral("origen/igual.bin")), QByteArray(300000, 'z'));
        writeFile(destination + QStringLiteral("/igual.bin"), QByteArray(300000, 'z'));
        // Mismo tamaño, distinto contenido: no es el mismo archivo.
        QByteArray almost(300000, 'z');
        almost[299999] = 'y';
        writeFile(directory.filePath(QStringLiteral("origen/distinto.bin")), almost);
        writeFile(destination + QStringLiteral("/distinto.bin"), QByteArray(300000, 'z'));

        for (const Comparison comparison : {Comparison::Hash, Comparison::Bytes}) {
            QCOMPARE(sameContent(directory.filePath(QStringLiteral("origen/igual.bin")),
                                 destination + QStringLiteral("/igual.bin"), comparison, m_noCancel).value(), true);
            QCOMPARE(sameContent(directory.filePath(QStringLiteral("origen/distinto.bin")),
                                 destination + QStringLiteral("/distinto.bin"), comparison, m_noCancel).value(), false);
        }
        Engine engine(m_journal);
        TransferOptions options(CollisionPolicy::SkipIfIdentical);
        options.comparison = Comparison::Bytes;
        // Las dos carpetas se juntan y cada archivo decide por su cuenta.
        const QString folder = directory.filePath(QStringLiteral("origen"));
        const QString merged = directory.filePath(QStringLiteral("dest2"));
        const Outcome outcome = engine.copy({folder}, merged, options, m_noCancel);
        QVERIFY2(outcome.ok(), qPrintable(describe(outcome)));
        QCOMPARE(outcome.identical, QStringList{folder + QStringLiteral("/igual.bin")});
        QCOMPARE(treeOf(merged + QStringLiteral("/origen")),
                 (QStringList{QStringLiteral("distinto (2).bin"), QStringLiteral("distinto.bin"),
                              QStringLiteral("igual.bin")}));
        QCOMPARE(readFile(merged + QStringLiteral("/origen/distinto (2).bin")), almost);
        // Copiar otra vez lo mismo no deja otro duplicado: "distinto (2)" ya es idéntico.
        const Outcome again = engine.copy({folder}, merged, options, m_noCancel);
        QVERIFY(again.ok());
        QCOMPARE(again.identical.size(), 2);
        QVERIFY(!QFileInfo::exists(merged + QStringLiteral("/origen/distinto (3).bin")));
        QVERIFY(engine.undo(again.operationId, m_noCancel).ok());
        // Deshacer quita solo lo copiado; la carpeta que ya existía se queda.
        QVERIFY(engine.undo(outcome.operationId, m_noCancel).ok());
        QCOMPARE(treeOf(merged + QStringLiteral("/origen")),
                 (QStringList{QStringLiteral("distinto.bin"), QStringLiteral("igual.bin")}));
    }

    void askDecidesAndCanApplyToAll()
    {
        QTemporaryDir directory;
        const QString destination = directory.filePath(QStringLiteral("destino"));
        QStringList sources;
        for (const QString &name : {QStringLiteral("1.txt"), QStringLiteral("2.txt"), QStringLiteral("3.txt")}) {
            writeFile(directory.filePath(QStringLiteral("origen/") + name), "nuevo");
            writeFile(destination + QLatin1Char('/') + name, "viejo");
            sources.append(directory.filePath(QStringLiteral("origen/") + name));
        }
        int asked = 0;
        TransferOptions options(CollisionPolicy::Ask);
        options.ask = [&](const Conflict &conflict, const tasks::CancelToken &) {
            ++asked;
            [&] { QCOMPARE(conflict.targetStamp.size, 5); }();
            // Al primero: omitir. Al segundo: renombrar, y lo mismo para los que queden.
            return asked == 1 ? CollisionDecision{CollisionPolicy::Skip, false, false}
                              : CollisionDecision{CollisionPolicy::AutoRename, true, false};
        };
        Engine engine(m_journal);
        const Outcome outcome = engine.copy(sources, destination, options, m_noCancel);
        QVERIFY(outcome.ok());
        QCOMPARE(asked, 2);   // el tercero no pregunta
        QCOMPARE(outcome.skipped, QStringList{sources[0]});
        QCOMPARE(treeOf(destination), (QStringList{QStringLiteral("1.txt"), QStringLiteral("2 (2).txt"),
                                                   QStringLiteral("2.txt"), QStringLiteral("3 (2).txt"),
                                                   QStringLiteral("3.txt")}));
        // Cancelar desde la pregunta para la operación.
        options.ask = [](const Conflict &, const tasks::CancelToken &) { return CollisionDecision{CollisionPolicy::Skip, false, true}; };
        const Outcome cancelled = engine.copy(sources, destination, options, m_noCancel);
        QVERIFY(cancelled.cancelled);
    }

    void moveMergesFoldersAndRemovesEmptySource()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("fotos"));
        const QString destination = directory.filePath(QStringLiteral("archivo"));
        writeFile(source + QStringLiteral("/2024/a.jpg"), "a");
        writeFile(source + QStringLiteral("/2024/b.jpg"), "b nueva");
        writeFile(destination + QStringLiteral("/fotos/2024/b.jpg"), "b vieja");
        writeFile(destination + QStringLiteral("/fotos/2023/c.jpg"), "c");
        Engine engine(m_journal);
        // Omitiendo los choques: lo omitido se queda en el origen, que no se borra.
        const Outcome skipping = engine.move({source}, destination, CollisionPolicy::SkipIfIdentical, m_noCancel);
        QVERIFY2(skipping.ok(), qPrintable(describe(skipping)));
        QCOMPARE(treeOf(destination + QStringLiteral("/fotos")),
                 (QStringList{QStringLiteral("2023"), QStringLiteral("2023/c.jpg"), QStringLiteral("2024"),
                              QStringLiteral("2024/a.jpg"), QStringLiteral("2024/b (2).jpg"),
                              QStringLiteral("2024/b.jpg")}));
        QVERIFY(!QFileInfo::exists(source));   // todo salió: el origen vacío se quita
        QVERIFY(engine.undo(skipping.operationId, m_noCancel).ok());
        QCOMPARE(treeOf(source), (QStringList{QStringLiteral("2024"), QStringLiteral("2024/a.jpg"),
                                              QStringLiteral("2024/b.jpg")}));
        QCOMPARE(readFile(destination + QStringLiteral("/fotos/2024/b.jpg")), QByteArray("b vieja"));

        const Outcome skipped = engine.move({source}, destination, CollisionPolicy::Skip, m_noCancel);
        QVERIFY(skipped.ok());
        QCOMPARE(skipped.skipped, QStringList{source});   // carpeta contra carpeta: omitida entera
    }

    void verifiedCopiesAreCounted()
    {
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("origen/a.txt")), "uno");
        writeFile(directory.filePath(QStringLiteral("origen/sub/b.txt")), "dos");
        QDir().mkpath(directory.filePath(QStringLiteral("destino")));
        TransferOptions options(CollisionPolicy::AutoRename);
        options.copy.verify = true;
        Engine engine(m_journal);
        const Outcome outcome = engine.copy({directory.filePath(QStringLiteral("origen"))},
                                            directory.filePath(QStringLiteral("destino")), options, m_noCancel);
        QVERIFY2(outcome.ok(), qPrintable(describe(outcome)));
        QCOMPARE(outcome.verified, 2);
    }

    void interruptedCopyIsRetriedWithoutDuplicates()
    {
        QTemporaryDir directory;
        const QString folder = directory.filePath(QStringLiteral("fotos"));
        const QString single = directory.filePath(QStringLiteral("suelto.txt"));
        for (int i = 0; i < 12; ++i) {
            writeFile(folder + QStringLiteral("/%1.jpg").arg(i), QByteArray(1000 + i, char('a' + i)));
        }
        writeFile(single, "suelto");
        const QString destination = directory.filePath(QStringLiteral("destino"));
        // En el destino ya había otra "fotos": la copia irá a "fotos (2)".
        writeFile(destination + QStringLiteral("/fotos/ajena.jpg"), "de otro");

        tasks::CancelSource source;
        int files = 0;
        Engine engine(m_journal);
        const Outcome first = engine.copy({folder, single}, destination, CollisionPolicy::AutoRename, source.token(),
                                          [&](const Progress &progress) {
            if (progress.itemsDone >= 6 && files++ == 0) {
                source.cancel();   // a media carpeta
            }
        });
        QVERIFY(first.cancelled);
        QCOMPARE(first.unfinished.size(), 2);
        QCOMPARE(first.unfinished[0].second, destination + QStringLiteral("/fotos (2)"));
        QCOMPARE(first.unfinished[1].first, single);
        const int copiedBefore = int(treeOf(destination + QStringLiteral("/fotos (2)")).size());
        QVERIFY(copiedBefore > 0 && copiedBefore < 12);

        const Outcome retried = engine.retry(first.unfinished, false, CollisionPolicy::AutoRename, m_noCancel);
        QVERIFY2(retried.ok(), qPrintable(describe(retried)));
        QCOMPARE(treeOf(destination + QStringLiteral("/fotos (2)")), treeOf(folder));
        QCOMPARE(treeOf(destination + QStringLiteral("/fotos")), QStringList{QStringLiteral("ajena.jpg")});
        QCOMPARE(readFile(destination + QStringLiteral("/suelto.txt")), QByteArray("suelto"));
        QVERIFY(!QFileInfo::exists(destination + QStringLiteral("/fotos (3)")));
        QCOMPARE(int(retried.identical.size()), copiedBefore);   // lo ya copiado no se repite
    }

    void pausePointHoldsTheCopy()
    {
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("a.bin")), QByteArray(5 * 1024 * 1024, 'p'));
        QDir().mkpath(directory.filePath(QStringLiteral("destino")));
        std::atomic<int> pauses{0};
        TransferOptions options(CollisionPolicy::AutoRename);
        options.copy.pausePoint = [&](const tasks::CancelToken &) { ++pauses; };
        Engine engine(m_journal);
        QVERIFY(engine.copy({directory.filePath(QStringLiteral("a.bin"))}, directory.filePath(QStringLiteral("destino")),
                            options, m_noCancel).ok());
        QVERIFY2(pauses >= 5, qPrintable(QString::number(pauses)));   // al menos uno por bloque de 1 MB
    }

    void folderCannotBeCopiedOrMovedIntoItself()
    {
        QTemporaryDir directory;
        const QString folder = directory.filePath(QStringLiteral("carpeta"));
        writeFile(folder + QStringLiteral("/sub/x"), "x");
        Engine engine(m_journal);
        const Outcome copied = engine.copy({folder}, folder + QStringLiteral("/sub"), CollisionPolicy::AutoRename, m_noCancel);
        QCOMPARE(copied.errors.size(), 1);
        const Outcome moved = engine.move({folder}, folder + QStringLiteral("/sub"), CollisionPolicy::AutoRename, m_noCancel);
        QCOMPARE(moved.errors.size(), 1);
        QCOMPARE(treeOf(folder), (QStringList{QStringLiteral("sub"), QStringLiteral("sub/x")}));
    }

    void moveWithinVolumeAndUndo()
    {
        QTemporaryDir directory;
        const QString folder = directory.filePath(QStringLiteral("álbum"));
        writeFile(folder + QStringLiteral("/1.jpg"), "uno");
        const QString destination = directory.filePath(QStringLiteral("destino"));
        QVERIFY(QDir().mkdir(destination));

        Engine engine(m_journal);
        const Outcome moved = engine.move({folder}, destination, CollisionPolicy::Skip, m_noCancel);
        QVERIFY2(moved.ok(), qPrintable(describe(moved)));
        QVERIFY(!QFileInfo::exists(folder));
        QCOMPARE(readFile(destination + QStringLiteral("/álbum/1.jpg")), QByteArray("uno"));
        // Un solo paso: dentro del mismo volumen mover es renombrar.
        QCOMPARE(m_journal->steps(moved.operationId).value().size(), 1);

        QVERIFY(engine.undo(moved.operationId, m_noCancel).ok());
        QCOMPARE(readFile(folder + QStringLiteral("/1.jpg")), QByteArray("uno"));
        QVERIFY(treeOf(destination).isEmpty());
    }

    void moveAcrossVolumesAndUndo()
    {
        QTemporaryDir sourceDirectory;
        const QString otherBase = otherVolumeBase(sourceDirectory.path());
        if (otherBase.isEmpty()) {
            QSKIP("Este equipo no tiene dos volúmenes escribibles para la prueba.");
        }
        QTemporaryDir destinationDirectory(otherBase + QStringLiteral("/eu-prueba-XXXXXX"));
        QVERIFY(destinationDirectory.isValid());

        const QString folder = sourceDirectory.filePath(QStringLiteral("proyecto"));
        writeFile(folder + QStringLiteral("/datos.bin"), QByteArray(2'000'000, 'd'));
        writeFile(folder + QStringLiteral("/sub/nota.txt"), "nota");
        QVERIFY(QDir().mkpath(folder + QStringLiteral("/vacía")));
        const QStringList before = treeOf(folder);

        Engine engine(m_journal);
        const Outcome moved = engine.move({folder}, destinationDirectory.path(), CollisionPolicy::Skip, m_noCancel);
        QVERIFY2(moved.ok(), qPrintable(describe(moved)));
        QVERIFY(!QFileInfo::exists(folder));
        const QString arrived = destinationDirectory.filePath(QStringLiteral("proyecto"));
        QCOMPARE(treeOf(arrived), before);
        QCOMPARE(readFile(arrived + QStringLiteral("/datos.bin")), QByteArray(2'000'000, 'd'));

        // Deshacer lo devuelve entero al volumen de origen.
        const Outcome undone = engine.undo(moved.operationId, m_noCancel);
        QVERIFY2(undone.ok(), qPrintable(describe(undone)));
        QCOMPARE(treeOf(folder), before);
        QCOMPARE(readFile(folder + QStringLiteral("/sub/nota.txt")), QByteArray("nota"));
        QVERIFY(treeOf(destinationDirectory.path()).isEmpty());
    }

    void trashAndUndo()
    {
        QTemporaryDir directory;
        const QString file = directory.filePath(QStringLiteral("borrar.txt"));
        const QString folder = directory.filePath(QStringLiteral("carpeta"));
        writeFile(file, "adiós");
        writeFile(folder + QStringLiteral("/dentro"), "d");

        Engine engine(m_journal);
        const Outcome trashed = engine.trash({file, folder}, m_noCancel);
        if (!trashed.errors.isEmpty() && trashed.errors.first().code == core::ErrorCode::NotSupported) {
            QSKIP("El volumen de la carpeta temporal no admite papelera en este equipo.");
        }
        QVERIFY2(trashed.ok(), qPrintable(describe(trashed)));
        QVERIFY(treeOf(directory.path()).isEmpty());
        QCOMPARE(trashed.resultPaths.size(), 2);
        const QString trashDirectory = trash::trashDirectoryFor(file).value();

        const Outcome undone = engine.undo(trashed.operationId, m_noCancel);
        QVERIFY2(undone.ok(), qPrintable(describe(undone)));
        QCOMPARE(readFile(file), QByteArray("adiós"));
        QCOMPARE(readFile(folder + QStringLiteral("/dentro")), QByteArray("d"));
        // No quedan restos de esos elementos en la papelera.
        for (const QString &stored : trashed.resultPaths) {
            QVERIFY(!QFileInfo::exists(stored));
            QVERIFY(!QFileInfo::exists(QFileInfo(stored).absolutePath()));
            QVERIFY(!QFileInfo::exists(QFileInfo(stored).absolutePath() + QStringLiteral(".info")));
        }
        Q_UNUSED(trashDirectory)
    }

    void permanentDeleteCannotBeUndone()
    {
        QTemporaryDir directory;
        const QString folder = directory.filePath(QStringLiteral("fuera"));
        writeFile(folder + QStringLiteral("/a/b"), "b");
        Engine engine(m_journal);
        const Outcome deleted = engine.deletePermanently({folder}, m_noCancel);
        QVERIFY2(deleted.ok(), qPrintable(describe(deleted)));
        QVERIFY(!QFileInfo::exists(folder));
        QVERIFY(!Engine::canUndo(m_journal->operation(deleted.operationId).value()));
        const Outcome undone = engine.undo(deleted.operationId, m_noCancel);
        QCOMPARE(undone.errors.first().code, core::ErrorCode::NotSupported);
    }

    void undoLeavesAloneWhatChangedOutside()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("a.txt"));
        writeFile(source, "original");
        const QString destination = directory.filePath(QStringLiteral("destino"));
        QVERIFY(QDir().mkdir(destination));
        Engine engine(m_journal);
        const Outcome copied = engine.copy({source}, destination, CollisionPolicy::Skip, m_noCancel);
        QVERIFY(copied.ok());

        // El usuario edita la copia desde otro programa.
        writeFile(destination + QStringLiteral("/a.txt"), "editado por fuera, con trabajo nuevo");

        const Outcome undone = engine.undo(copied.operationId, m_noCancel);
        QVERIFY(!undone.ok());
        QCOMPARE(undone.errors.size(), 1);
        QCOMPARE(readFile(destination + QStringLiteral("/a.txt")), QByteArray("editado por fuera, con trabajo nuevo"));
        // La operación no queda marcada como deshecha.
        QVERIFY(m_journal->operation(copied.operationId).value().state != journal::OperationState::Undone);
    }

    void undoKeepsFolderThatGotNewContent()
    {
        QTemporaryDir directory;
        Engine engine(m_journal);
        const Outcome created = engine.createFolder(directory.path(), QStringLiteral("nueva"));
        QVERIFY(created.ok());
        writeFile(directory.filePath(QStringLiteral("nueva/importante.txt")), "no borrar");
        const Outcome undone = engine.undo(created.operationId, m_noCancel);
        QVERIFY(!undone.ok());
        QCOMPARE(undone.errors.first().code, core::ErrorCode::DirectoryNotEmpty);
        QCOMPARE(readFile(directory.filePath(QStringLiteral("nueva/importante.txt"))), QByteArray("no borrar"));
    }

    void cancelledCopyCanBeUndone()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("origen"));
        for (int i = 0; i < 20; ++i) {
            writeFile(source + QStringLiteral("/f%1").arg(i), QByteArray(50'000, 'x'));
        }
        const QString destination = directory.filePath(QStringLiteral("destino"));
        QVERIFY(QDir().mkdir(destination));

        Engine engine(m_journal);
        tasks::CancelSource cancel;
        const Outcome outcome = engine.copy({source}, destination, CollisionPolicy::Skip, cancel.token(),
                                            [&](const Progress &progress) {
            if (progress.itemsDone >= 8) {
                cancel.cancel();
            }
        });
        QVERIFY(outcome.cancelled);
        QCOMPARE(m_journal->operation(outcome.operationId).value().state, journal::OperationState::Cancelled);
        const QStringList partial = treeOf(destination);
        QVERIFY(!partial.isEmpty());
        QVERIFY(partial.size() < 21);
        for (const QString &path : partial) {
            QVERIFY(!path.endsWith(partialSuffix()));   // ningún archivo a medias
        }
        QVERIFY(engine.undo(outcome.operationId, m_noCancel).ok());
        QVERIFY(treeOf(destination).isEmpty());
        QCOMPARE(treeOf(source).size(), 20);
    }

    void restoreAndPurgeFromTrashAreJournaled()
    {
        QTemporaryDir directory;
        QTemporaryDir journalDirectory;
        auto journal = journal::Journal::open(journalDirectory.filePath(QStringLiteral("h.db"))).value();
        Engine engine(journal);
        const QString first = directory.filePath(QStringLiteral("uno.txt"));
        const QString second = directory.filePath(QStringLiteral("carpeta"));
        writeFile(first, "uno");
        writeFile(second + QStringLiteral("/dentro.txt"), "dentro");

        const Outcome trashed = engine.trash({first, second}, m_noCancel);
        if (!trashed.ok()) {
            QSKIP("El volumen temporal de este equipo no admite papelera.");
        }
        const QString trashDirectory = trash::trashDirectoryFor(directory.path()).value();
        QList<trash::TrashItem> mine;
        // La lista se guarda en una variable: recorrer directamente el
        // resultado de la llamada usaría datos ya destruidos.
        const QList<trash::TrashItem> inTrash = trash::listTrash(trashDirectory).value();
        for (const trash::TrashItem &item : inTrash) {
            if (item.originalPath == first || item.originalPath == second) {
                mine.append(item);
            }
        }
        QCOMPARE(mine.size(), 2);

        // Si en el sitio original ya hay algo, no se sobrescribe: se queda en la papelera.
        writeFile(first, "otro");
        const trash::TrashItem fileItem = mine[0].originalPath == first ? mine[0] : mine[1];
        const trash::TrashItem folderItem = mine[0].originalPath == first ? mine[1] : mine[0];
        const Outcome blocked = engine.restoreFromTrash({fileItem}, m_noCancel);
        QCOMPARE(blocked.errors.size(), 1);
        QCOMPARE(blocked.errors.first().code, core::ErrorCode::AlreadyExists);
        QCOMPARE(readFile(first), QByteArray("otro"));
        QVERIFY(QFileInfo::exists(fileItem.storedPath()));

        // Con el sitio libre, vuelve tal cual estaba.
        QVERIFY(QFile::remove(first));
        const Outcome restored = engine.restoreFromTrash({fileItem}, m_noCancel);
        QVERIFY2(restored.ok(), qPrintable(describe(restored)));
        QCOMPARE(restored.resultPaths, QStringList{first});
        QCOMPARE(readFile(first), QByteArray("uno"));
        QVERIFY(!QFileInfo::exists(fileItem.infoPath()));
        // Queda en el historial y no se ofrece deshacerla.
        const journal::Operation operation = journal->operation(restored.operationId).value();
        QCOMPARE(operation.kind, journal::OperationKind::Restore);
        QVERIFY(!Engine::canUndo(operation));
        QVERIFY(!engine.undo(restored.operationId, m_noCancel).ok());
        QCOMPARE(readFile(first), QByteArray("uno"));

        // Eliminar para siempre lo que sigue en la papelera.
        const Outcome purged = engine.purgeFromTrash({folderItem}, m_noCancel);
        QVERIFY2(purged.ok(), qPrintable(describe(purged)));
        QVERIFY(!QFileInfo::exists(folderItem.storedPath()));
        QVERIFY(!QFileInfo::exists(folderItem.infoPath()));
        QVERIFY(!QFileInfo::exists(second));
        QCOMPARE(journal->operation(purged.operationId).value().kind, journal::OperationKind::Delete);
    }

    void killedMidMoveLosesNothingAndReverts()
    {
        const QString helper = QCoreApplication::applicationDirPath() + QStringLiteral("/eu-crash-helper");
        QTemporaryDir sourceDirectory;
        const QString otherBase = otherVolumeBase(sourceDirectory.path());
        if (otherBase.isEmpty()) {
            QSKIP("Este equipo no tiene dos volúmenes escribibles para la prueba.");
        }
        QTemporaryDir destinationDirectory(otherBase + QStringLiteral("/eu-prueba-XXXXXX"));
        QVERIFY(destinationDirectory.isValid());
        QTemporaryDir journalDirectory;
        const QString journalPath = journalDirectory.filePath(QStringLiteral("h.db"));

        const QString folder = sourceDirectory.filePath(QStringLiteral("fotos"));
        for (int i = 0; i < 10; ++i) {
            writeFile(folder + QStringLiteral("/foto%1.raw").arg(i), QByteArray(1'500'000, char('a' + i)));
        }
        const QStringList before = treeOf(folder);

        // El auxiliar muere tras copiar ~6,5 MB de los 15 MB: a mitad de un archivo.
        QProcess process;
        process.start(helper, {journalPath, folder, destinationDirectory.path(), QStringLiteral("6500000"),
                               QStringLiteral("mover")});
        QVERIFY(process.waitForFinished(60000));
        QCOMPARE(process.exitCode(), 42);

        // 1) Nada se perdió: el origen está completo (aún no se había borrado nada).
        QCOMPARE(treeOf(folder), before);
        for (int i = 0; i < 10; ++i) {
            QCOMPARE(QFileInfo(folder + QStringLiteral("/foto%1.raw").arg(i)).size(), qint64(1'500'000));
        }
        // 2) En el destino hay una copia parcial, con un archivo a medio escribir.
        const QStringList leftovers = treeOf(destinationDirectory.path());
        QVERIFY(leftovers.size() >= 2);
        QVERIFY(std::any_of(leftovers.begin(), leftovers.end(),
                            [](const QString &path) { return path.endsWith(partialSuffix()); }));

        // 3) Al "reabrir", el historial sabe que quedó una operación a medias.
        auto journal = journal::Journal::open(journalPath).value();
        const auto incomplete = journal->incompleteOperations().value();
        QCOMPARE(incomplete.size(), 1);
        QCOMPARE(incomplete.first().kind, journal::OperationKind::Move);

        // 4) Revertir limpia el destino por completo y deja el origen intacto.
        Engine engine(journal);
        const Outcome reverted = engine.revertInterrupted(incomplete.first().id);
        QVERIFY2(reverted.ok(), qPrintable(describe(reverted)));
        QVERIFY2(treeOf(destinationDirectory.path()).isEmpty(),
                 qPrintable(treeOf(destinationDirectory.path()).join(QLatin1Char(','))));
        QCOMPARE(treeOf(folder), before);
        QVERIFY(journal->incompleteOperations().value().isEmpty());
    }

    void killedMidCopyCanBeDiscarded()
    {
        const QString helper = QCoreApplication::applicationDirPath() + QStringLiteral("/eu-crash-helper");
        QTemporaryDir directory;
        QTemporaryDir journalDirectory;
        const QString journalPath = journalDirectory.filePath(QStringLiteral("h.db"));
        const QString folder = directory.filePath(QStringLiteral("origen"));
        for (int i = 0; i < 6; ++i) {
            writeFile(folder + QStringLiteral("/f%1").arg(i), QByteArray(1'200'000, 'z'));
        }
        const QString destination = directory.filePath(QStringLiteral("destino"));
        QVERIFY(QDir().mkdir(destination));

        QProcess process;
        process.start(helper, {journalPath, folder, destination, QStringLiteral("3000000"), QStringLiteral("copiar")});
        QVERIFY(process.waitForFinished(60000));
        QCOMPARE(process.exitCode(), 42);

        auto journal = journal::Journal::open(journalPath).value();
        const auto incomplete = journal->incompleteOperations().value();
        QCOMPARE(incomplete.size(), 1);
        Engine engine(journal);
        engine.discardInterrupted(incomplete.first().id);

        // Descartar conserva lo ya copiado entero y quita solo el archivo a medias.
        const QStringList kept = treeOf(destination);
        QVERIFY(kept.size() >= 2);
        for (const QString &path : kept) {
            QVERIFY(!path.endsWith(partialSuffix()));
            const QFileInfo info(destination + QLatin1Char('/') + path);
            if (info.isFile()) {
                QCOMPARE(info.size(), qint64(1'200'000));
            }
        }
        QVERIFY(journal->incompleteOperations().value().isEmpty());
        QCOMPARE(treeOf(folder).size(), 6);
    }
};

QTEST_GUILESS_MAIN(TestEngine)
#include "test_engine.moc"
