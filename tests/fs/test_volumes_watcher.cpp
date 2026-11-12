// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/fs/DirectoryWatcher.h"
#include "eu/fs/Volumes.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace eu::fs;

class TestVolumesWatcher : public QObject
{
    Q_OBJECT

private slots:
    void thereIsAtLeastOneVolume()
    {
        const QList<Volume> volumes = listVolumes();
        QVERIFY(!volumes.isEmpty());
        for (const Volume &volume : volumes) {
            QVERIFY(!volume.rootPath.isEmpty());
            QVERIFY(!volume.displayName.isEmpty());
        }
    }

    void temporaryFolderBelongsToAVolume()
    {
        QTemporaryDir directory;
        const Volume volume = volumeForPath(directory.path());
        QVERIFY(!volume.rootPath.isEmpty());
        QVERIFY(volume.totalBytes > 0);
        QVERIFY(directory.path().startsWith(volume.rootPath)
                || QDir(directory.path()).canonicalPath().startsWith(volume.rootPath));
    }

    void folderAndItsChildShareVolume()
    {
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("sub")));
        QVERIFY(isSameVolume(directory.path(), directory.filePath(QStringLiteral("sub"))));
        QVERIFY(!isSameVolume(directory.path(), QStringLiteral("/ruta/que/no/existe/en/ningún/sitio")));
    }

    void monitorStartsWithCurrentVolumes()
    {
        VolumeMonitor monitor;
        QCOMPARE(monitor.volumes(), listVolumes());
        QSignalSpy spy(&monitor, &VolumeMonitor::volumesChanged);
        monitor.refresh();
        QCOMPARE(spy.count(), 0);   // nada cambió: no debe avisar
    }

    void watcherReportsNewFilesOnce()
    {
        QTemporaryDir directory;
        DirectoryWatcher watcher;
        watcher.setQuietPeriodMs(100);
        QVERIFY(watcher.watch(directory.path()));
        QSignalSpy spy(&watcher, &DirectoryWatcher::directoryChanged);

        // Muchos cambios seguidos se agrupan en pocos avisos.
        for (int i = 0; i < 50; ++i) {
            QFile file(directory.filePath(QStringLiteral("nuevo%1.txt").arg(i)));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        QVERIFY(spy.wait(5000));
        QTest::qWait(400);
        QVERIFY(spy.count() >= 1);
        QVERIFY2(spy.count() <= 5, qPrintable(QString::number(spy.count())));
        QCOMPARE(spy.first().first().toString(), directory.path());
    }

    void unwatchStopsNotifications()
    {
        QTemporaryDir directory;
        DirectoryWatcher watcher;
        watcher.setQuietPeriodMs(50);
        QVERIFY(watcher.watch(directory.path()));
        QVERIFY(watcher.watch(directory.path()));   // dos pestañas en la misma carpeta
        watcher.unwatch(directory.path());           // una se cierra: sigue vigilada

        QSignalSpy spy(&watcher, &DirectoryWatcher::directoryChanged);
        QFile first(directory.filePath(QStringLiteral("a")));
        QVERIFY(first.open(QIODevice::WriteOnly));
        first.close();
        QVERIFY(spy.wait(5000));

        watcher.unwatch(directory.path());           // se cierra la otra
        spy.clear();
        QFile second(directory.filePath(QStringLiteral("b")));
        QVERIFY(second.open(QIODevice::WriteOnly));
        second.close();
        QVERIFY(!spy.wait(600));
    }

    void watchingAMissingFolderFails()
    {
        DirectoryWatcher watcher;
        QVERIFY(!watcher.watch(QStringLiteral("/ruta/que/no/existe/en/ningún/sitio")));
    }
};

QTEST_GUILESS_MAIN(TestVolumesWatcher)
#include "test_volumes_watcher.moc"
