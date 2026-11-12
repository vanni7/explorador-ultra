// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/DirectoryModel.h"

#include <QAbstractItemModelTester>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace eu;
using namespace eu::ui;

namespace {

void writeFile(const QString &path, qsizetype bytes = 0)
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(QByteArray(bytes, 'x'));
}

/// Espera a que el modelo termine de cargar (o falle).
bool waitUntilSettled(DirectoryModel &model, int timeoutMs = 20000)
{
    QElapsedTimer timer;
    timer.start();
    while (model.state() == DirectoryModel::State::Loading && timer.elapsed() < timeoutMs) {
        QTest::qWait(5);
    }
    return model.state() != DirectoryModel::State::Loading;
}

QStringList names(const DirectoryModel &model)
{
    QStringList result;
    for (int row = 0; row < model.rowCount(); ++row) {
        result.append(model.entryAt(row).name);
    }
    return result;
}

} // namespace

class TestDirectoryModel : public QObject
{
    Q_OBJECT

private:
    tasks::Scheduler m_scheduler;

private slots:
    void loadsSortedWithFoldersFirst()
    {
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("archivo10.txt")));
        writeFile(directory.filePath(QStringLiteral("archivo2.txt")));
        writeFile(directory.filePath(QStringLiteral("Beta.txt")));
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("zeta")));
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("alfa")));

        DirectoryModel model(m_scheduler);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy replaced(&model, &DirectoryModel::contentReplaced);
        model.setPath(directory.path());
        QCOMPARE(model.state(), DirectoryModel::State::Loading);
        QVERIFY(waitUntilSettled(model));

        QCOMPARE(model.state(), DirectoryModel::State::Ready);
        QCOMPARE(names(model), (QStringList{QStringLiteral("alfa"), QStringLiteral("zeta"),
                                            QStringLiteral("archivo2.txt"), QStringLiteral("archivo10.txt"),
                                            QStringLiteral("Beta.txt")}));
        QCOMPARE(replaced.count(), 1);
        QCOMPARE(model.columnCount(), 4);
    }

    void columnsShowReadableData()
    {
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("datos.txt")), 1500);
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("carpeta")));

        DirectoryModel model(m_scheduler);
        model.setPath(directory.path());
        QVERIFY(waitUntilSettled(model));

        const int fileRow = model.rowOfName(QStringLiteral("datos.txt"));
        const int folderRow = model.rowOfName(QStringLiteral("carpeta"));
        QVERIFY(fileRow >= 0 && folderRow >= 0);

        QLocale::setDefault(QLocale(QLocale::Spanish, QLocale::Spain));
        QCOMPARE(model.data(model.index(fileRow, int(Column::Size))).toString(), QStringLiteral("1,5 kB"));
        model.setSizeUnits(core::SizeUnits::Binary);
        QCOMPARE(model.data(model.index(fileRow, int(Column::Size))).toString(), QStringLiteral("1,5 KiB"));
        QVERIFY(model.data(model.index(folderRow, int(Column::Size))).toString().isEmpty());
        QCOMPARE(model.data(model.index(folderRow, int(Column::Type))).toString(), QStringLiteral("Carpeta"));
        QVERIFY(!model.data(model.index(fileRow, int(Column::Type))).toString().isEmpty());
        QVERIFY(!model.data(model.index(fileRow, int(Column::Modified))).toString().isEmpty());
        QCOMPARE(model.filePath(fileRow), directory.filePath(QStringLiteral("datos.txt")));
        QCOMPARE(model.headerData(0, Qt::Horizontal).toString(), QStringLiteral("Nombre"));
    }

    void sortsByEachColumn()
    {
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("grande.zip")), 3000);
        writeFile(directory.filePath(QStringLiteral("chico.txt")), 10);
        writeFile(directory.filePath(QStringLiteral("mediano.avi")), 500);
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("carpeta")));

        DirectoryModel model(m_scheduler);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.setPath(directory.path());
        QVERIFY(waitUntilSettled(model));

        model.sort(int(Column::Size), Qt::DescendingOrder);
        QCOMPARE(names(model), (QStringList{QStringLiteral("carpeta"), QStringLiteral("grande.zip"),
                                            QStringLiteral("mediano.avi"), QStringLiteral("chico.txt")}));
        model.sort(int(Column::Type), Qt::AscendingOrder);
        QCOMPARE(names(model), (QStringList{QStringLiteral("carpeta"), QStringLiteral("mediano.avi"),
                                            QStringLiteral("chico.txt"), QStringLiteral("grande.zip")}));
        model.sort(int(Column::Name), Qt::DescendingOrder);
        QCOMPARE(names(model), (QStringList{QStringLiteral("carpeta"), QStringLiteral("mediano.avi"),
                                            QStringLiteral("grande.zip"), QStringLiteral("chico.txt")}));
    }

    void hiddenFilesAreFilteredAndCounted()
    {
#ifdef Q_OS_UNIX
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral(".oculto")));
        writeFile(directory.filePath(QStringLiteral("visible")));

        DirectoryModel model(m_scheduler);
        model.setPath(directory.path());
        QVERIFY(waitUntilSettled(model));
        QCOMPARE(names(model), QStringList{QStringLiteral("visible")});
        QCOMPARE(model.hiddenCount(), 1);

        model.setShowHidden(true);
        QVERIFY(waitUntilSettled(model));
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.hiddenCount(), 0);
#else
        QSKIP("En Windows lo oculto es un atributo.");
#endif
    }

    void missingFolderEndsInFailedState()
    {
        QTemporaryDir directory;
        DirectoryModel model(m_scheduler);
        model.setPath(directory.filePath(QStringLiteral("no-existe")));
        QVERIFY(waitUntilSettled(model));
        QCOMPARE(model.state(), DirectoryModel::State::Failed);
        QCOMPARE(model.lastError().code, core::ErrorCode::NotFound);
        QCOMPARE(model.rowCount(), 0);
    }

    void refreshPicksUpNewFilesWithoutEmptyingTheView()
    {
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("uno")));
        DirectoryModel model(m_scheduler);
        model.setPath(directory.path());
        QVERIFY(waitUntilSettled(model));
        QCOMPARE(model.rowCount(), 1);

        writeFile(directory.filePath(QStringLiteral("dos")));
        model.refresh();
        QCOMPARE(model.rowCount(), 1);   // lo anterior sigue a la vista mientras carga
        QVERIFY(waitUntilSettled(model));
        QCOMPARE(names(model), (QStringList{QStringLiteral("dos"), QStringLiteral("uno")}));
    }

    void changingFolderMidLoadKeepsOnlyTheLastOne()
    {
        QTemporaryDir first;
        QTemporaryDir second;
        for (int i = 0; i < 3000; ++i) {
            writeFile(first.filePath(QStringLiteral("a%1").arg(i)));
        }
        writeFile(second.filePath(QStringLiteral("solo")));

        DirectoryModel model(m_scheduler);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.setPath(first.path());
        model.setPath(second.path());   // sin esperar a la primera
        QVERIFY(waitUntilSettled(model));
        QTest::qWait(100);              // margen para que llegue algo rezagado de la primera
        QCOMPARE(names(model), QStringList{QStringLiteral("solo")});
    }

    void hugeFolderLoadsProgressivelyAndFast()
    {
        QTemporaryDir directory;
        const int count = 100'000;
        for (int i = 0; i < count; ++i) {
            QFile file(directory.filePath(QStringLiteral("archivo%1.dat").arg(i)));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }

        // Sin QAbstractItemModelTester: sus comprobaciones multiplican por cinco
        // el tiempo y aquí se mide rendimiento. Las demás pruebas sí lo usan.
        DirectoryModel model(m_scheduler);
        model.setRevealDelayMs(20);
        QElapsedTimer timer;
        timer.start();
        model.setPath(directory.path());

        // Primeros elementos a la vista antes de que termine todo.
        qint64 firstRowsMs = -1;
        while (model.state() == DirectoryModel::State::Loading && timer.elapsed() < 30000) {
            QTest::qWait(1);
            if (firstRowsMs < 0 && model.rowCount() > 0) {
                firstRowsMs = timer.elapsed();
            }
        }
        const qint64 totalMs = timer.elapsed();
        QCOMPARE(model.state(), DirectoryModel::State::Ready);
        QCOMPARE(model.rowCount(), count);
        QCOMPARE(model.entryAt(0).name, QStringLiteral("archivo0.dat"));
        QCOMPARE(model.entryAt(count - 1).name, QStringLiteral("archivo99999.dat"));
        qInfo("100 000 archivos: primeras filas a los %lld ms, listo y ordenado a los %lld ms",
              firstRowsMs, totalMs);
#ifdef EU_CHECK_TIMINGS
        // Meta de SPEC §4 (holgada para máquinas de CI lentas): completo en < 3 s.
        QVERIFY2(totalMs < 3000, qPrintable(QString::number(totalMs)));
#endif

        // Reordenar una carpeta enorme no vacía la vista y termina bien.
        model.sort(int(Column::Name), Qt::DescendingOrder);
        QCOMPARE(model.rowCount(), count);
        QTRY_COMPARE_WITH_TIMEOUT(model.entryAt(0).name, QStringLiteral("archivo99999.dat"), 60000);
    }

    void folderSizesAppearOnRequest()
    {
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("fotos/viaje")));
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("vacia")));
        writeFile(directory.filePath(QStringLiteral("fotos/a.jpg")), 1500);
        writeFile(directory.filePath(QStringLiteral("fotos/viaje/b.jpg")), 2500);
        writeFile(directory.filePath(QStringLiteral("suelto.txt")), 10);

        DirectoryModel model(m_scheduler);
        model.setPath(directory.path());
        QVERIFY(waitUntilSettled(model));
        const int photos = model.rowOfName(QStringLiteral("fotos"));
        const int empty = model.rowOfName(QStringLiteral("vacia"));
        const QModelIndex photosSize = model.index(photos, int(Column::Size));

        // Sin pedirlo, las carpetas no muestran tamaño (ni se recorre el disco).
        QCOMPARE(model.data(photosSize).toString(), QString());
        QVERIFY(model.folderSizeAt(photos) == nullptr);

        model.setFolderSizesEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(model.folderSizeAt(empty) != nullptr && model.folderSizeAt(empty)->finished
                                     && model.folderSizeAt(photos) != nullptr
                                     && model.folderSizeAt(photos)->finished, 10000);
        QCOMPARE(model.folderSizeAt(photos)->size.bytes, qint64(4000));
        QCOMPARE(model.folderSizeAt(photos)->size.files, qint64(2));
        QCOMPARE(model.folderSizeAt(photos)->size.folders, qint64(1));
        QCOMPARE(model.data(photosSize).toString(), core::formatSize(4000, core::SizeUnits::Decimal));
        QCOMPARE(model.folderSizeAt(empty)->size.bytes, qint64(0));
        QTRY_VERIFY(!model.isMeasuringFolders());

        // Al refrescar se conserva lo ya calculado y se mide solo lo nuevo.
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("nueva")));
        writeFile(directory.filePath(QStringLiteral("nueva/c.bin")), 700);
        model.refresh();
        QVERIFY(waitUntilSettled(model));
        QVERIFY(model.folderSizeAt(model.rowOfName(QStringLiteral("fotos"))) != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(model.folderSizeAt(model.rowOfName(QStringLiteral("nueva"))) != nullptr
                                     && model.folderSizeAt(model.rowOfName(QStringLiteral("nueva")))->finished,
                                 10000);
        QCOMPARE(model.folderSizeAt(model.rowOfName(QStringLiteral("nueva")))->size.bytes, qint64(700));

        // Al desactivarlo, los tamaños desaparecen; al cambiar de carpeta, también.
        model.setFolderSizesEnabled(false);
        QVERIFY(model.folderSizeAt(model.rowOfName(QStringLiteral("fotos"))) == nullptr);
        model.setFolderSizesEnabled(true);
        model.setPath(directory.filePath(QStringLiteral("fotos")));
        QVERIFY(waitUntilSettled(model));
        QTRY_VERIFY_WITH_TIMEOUT(model.folderSizeAt(model.rowOfName(QStringLiteral("viaje"))) != nullptr
                                     && model.folderSizeAt(model.rowOfName(QStringLiteral("viaje")))->finished,
                                 10000);
        QCOMPARE(model.folderSizeAt(model.rowOfName(QStringLiteral("viaje")))->size.bytes, qint64(2500));
    }

    void destroyingWhileLoadingIsSafe()
    {
        QTemporaryDir directory;
        for (int i = 0; i < 5000; ++i) {
            QFile file(directory.filePath(QString::number(i)));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        for (int round = 0; round < 20; ++round) {
            DirectoryModel model(m_scheduler);
            model.setPath(directory.path());
            model.setPath(directory.path());
            if (round % 2 == 0) {
                QTest::qWait(round);
            }
        }   // se destruye a mitad de carga
        QTest::qWait(50);
    }
};

QTEST_MAIN(TestDirectoryModel)
#include "test_directory_model.moc"
