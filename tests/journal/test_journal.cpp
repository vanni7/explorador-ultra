// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/journal/Journal.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <atomic>
#include <thread>
#include <vector>

using namespace eu;
using namespace eu::journal;

class TestJournal : public QObject
{
    Q_OBJECT

private slots:
    void recordsOperationWithSteps()
    {
        QTemporaryDir directory;
        auto opened = Journal::open(directory.filePath(QStringLiteral("sub/historial.db")));
        QVERIFY2(opened.ok(), opened.ok() ? "" : qPrintable(opened.error().message));
        Journal &journal = *opened.value();
        QCOMPARE(journal.schemaVersion(), 1);

        const auto operationId = journal.beginOperation(OperationKind::Move, QStringLiteral("Mover 2 elementos"));
        QVERIFY(operationId.ok());
        const auto first = journal.addStep(operationId.value(), StepKind::Rename,
                                           QStringLiteral("/a/ñ.txt"), QStringLiteral("/b/ñ.txt"));
        const auto second = journal.addStep(operationId.value(), StepKind::CopyFile,
                                            QStringLiteral("/a/2"), QStringLiteral("/b/2"));
        QVERIFY(first.ok() && second.ok());
        QVERIFY(journal.completeStep(first.value(), 123, 456789).ok());

        const auto steps = journal.steps(operationId.value());
        QVERIFY(steps.ok());
        QCOMPARE(steps.value().size(), 2);
        QCOMPARE(steps.value()[0].sequence, 1);
        QCOMPARE(steps.value()[0].state, StepState::Done);
        QCOMPARE(steps.value()[0].source, QStringLiteral("/a/ñ.txt"));
        QCOMPARE(steps.value()[0].resultSize, qint64(123));
        QCOMPARE(steps.value()[0].resultModifiedMs, qint64(456789));
        QCOMPARE(steps.value()[1].sequence, 2);
        QCOMPARE(steps.value()[1].state, StepState::Pending);
        QCOMPARE(steps.value()[1].kind, StepKind::CopyFile);

        QVERIFY(journal.finishOperation(operationId.value(), OperationState::Completed).ok());
        const auto operation = journal.operation(operationId.value());
        QVERIFY(operation.ok());
        QCOMPARE(operation.value().state, OperationState::Completed);
        QCOMPARE(operation.value().description, QStringLiteral("Mover 2 elementos"));
        QVERIFY(operation.value().finishedMs >= operation.value().startedMs);
    }

    void survivesReopeningAndReportsInterruptedOperations()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("historial.db"));
        OperationId interrupted = 0;
        {
            auto journal = Journal::open(path).value();
            const auto done = journal->beginOperation(OperationKind::Copy, QStringLiteral("terminada"));
            journal->finishOperation(done.value(), OperationState::Completed);
            interrupted = journal->beginOperation(OperationKind::Move, QStringLiteral("a medias")).value();
            journal->addStep(interrupted, StepKind::CopyFile, QStringLiteral("/o"), QStringLiteral("/d"));
        }   // se cierra sin terminar la segunda: como un cierre inesperado

        auto reopened = Journal::open(path).value();
        const auto incomplete = reopened->incompleteOperations();
        QVERIFY(incomplete.ok());
        QCOMPARE(incomplete.value().size(), 1);
        QCOMPARE(incomplete.value().first().id, interrupted);
        QCOMPARE(reopened->steps(interrupted).value().first().state, StepState::Pending);
        QCOMPARE(reopened->recentOperations(10).value().size(), 2);
        QCOMPARE(reopened->recentOperations(10).value().first().id, interrupted);   // la más reciente primero
    }

    void pruneKeepsNewestAndRunning()
    {
        QTemporaryDir directory;
        auto journal = Journal::open(directory.filePath(QStringLiteral("h.db"))).value();
        const OperationId running = journal->beginOperation(OperationKind::Copy, QStringLiteral("en curso")).value();
        journal->addStep(running, StepKind::CopyFile, QStringLiteral("/a"), QStringLiteral("/b"));
        for (int i = 0; i < 10; ++i) {
            const OperationId id = journal->beginOperation(OperationKind::Rename, QString::number(i)).value();
            journal->addStep(id, StepKind::Rename, QStringLiteral("/x"), QStringLiteral("/y"));
            journal->finishOperation(id, OperationState::Completed);
        }
        const auto removed = journal->prune(3, 30);
        QVERIFY(removed.ok());
        QCOMPARE(removed.value(), 7);

        const auto remaining = journal->recentOperations(100).value();
        QCOMPARE(remaining.size(), 4);   // las 3 más nuevas + la que está en curso
        QVERIFY(journal->operation(running).ok());
        // Los pasos de lo purgado también desaparecen (borrado en cascada).
        QCOMPARE(journal->steps(remaining.last().id).value().size(), 1);
    }

    void unknownOperationIsNotFound()
    {
        QTemporaryDir directory;
        auto journal = Journal::open(directory.filePath(QStringLiteral("h.db"))).value();
        const auto missing = journal->operation(999);
        QVERIFY(!missing.ok());
        QCOMPARE(missing.error().code, core::ErrorCode::NotFound);
    }

    void newerSchemaIsRejected()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("h.db"));
        Journal::open(path).value();   // crea la base en la versión actual
        {
            // Se simula una base creada por una versión futura.
            auto journal = Journal::open(path).value();
            Q_UNUSED(journal)
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        // user_version ocupa los bytes 60..63 de la cabecera (big endian).
        QVERIFY(file.seek(60));
        file.write(QByteArray::fromHex("000003e7"));   // 999
        file.close();
        QFile::remove(path + QStringLiteral("-wal"));
        QFile::remove(path + QStringLiteral("-shm"));

        const auto opened = Journal::open(path);
        QVERIFY(!opened.ok());
        QCOMPARE(opened.error().code, core::ErrorCode::NotSupported);
    }

    void manyThreadsWriteAtOnce()
    {
        QTemporaryDir directory;
        auto journal = Journal::open(directory.filePath(QStringLiteral("h.db"))).value();
        std::atomic<int> failures{0};
        std::vector<std::thread> threads;
        for (int thread = 0; thread < 6; ++thread) {
            threads.emplace_back([&journal, &failures, thread] {
                for (int i = 0; i < 25; ++i) {
                    const auto id = journal->beginOperation(OperationKind::Copy,
                                                            QStringLiteral("hilo %1").arg(thread));
                    if (!id.ok()) { ++failures; continue; }
                    const auto step = journal->addStep(id.value(), StepKind::CopyFile,
                                                       QStringLiteral("/o"), QStringLiteral("/d"));
                    if (!step.ok() || !journal->completeStep(step.value(), 1, 1).ok()
                        || !journal->finishOperation(id.value(), OperationState::Completed).ok()) {
                        ++failures;
                    }
                }
            });
        }
        for (std::thread &thread : threads) {
            thread.join();
        }
        QCOMPARE(failures.load(), 0);
        QCOMPARE(journal->recentOperations(1000).value().size(), 150);
        QVERIFY(journal->incompleteOperations().value().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestJournal)
#include "test_journal.moc"
