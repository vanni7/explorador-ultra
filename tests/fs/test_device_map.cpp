// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/fs/DeviceMap.h"

#include <QTest>

#include <atomic>
#include <thread>

using namespace eu;
using namespace std::chrono_literals;

class TestDeviceMap : public QObject
{
    Q_OBJECT

private slots:
    void pathsBelongToTheDeepestMount()
    {
        tasks::Scheduler scheduler;
        fs::DeviceMap map(scheduler);
        QVERIFY(map.deviceFor(QStringLiteral("/home/ana")).isEmpty());

        fs::Volume system;
        system.rootPath = QStringLiteral("/");
        system.kind = fs::DeviceKind::Ssd;
        fs::Volume backup;
        backup.rootPath = QStringLiteral("/media/ana/RESPALDO");
        backup.kind = fs::DeviceKind::Hdd;
        map.setVolumes({system, backup});

        QCOMPARE(map.deviceFor(QStringLiteral("/home/ana/foto.jpg")), QStringLiteral("/"));
        QCOMPARE(map.deviceFor(QStringLiteral("/")), QStringLiteral("/"));
        QCOMPARE(map.deviceFor(QStringLiteral("/media/ana/RESPALDO")), QStringLiteral("/media/ana/RESPALDO"));
        QCOMPARE(map.deviceFor(QStringLiteral("/media/ana/RESPALDO/libros/a.pdf")),
                 QStringLiteral("/media/ana/RESPALDO"));
        // Un nombre que solo empieza igual no es esa unidad.
        QCOMPARE(map.deviceFor(QStringLiteral("/media/ana/RESPALDO2/x")), QStringLiteral("/"));
    }

    void theSchedulerLearnsEachDriveKind()
    {
        tasks::Scheduler scheduler;
        fs::DeviceMap map(scheduler);
        fs::Volume disk;
        disk.rootPath = QStringLiteral("/media/disco");
        disk.kind = fs::DeviceKind::Hdd;
        map.setVolumes({disk});

        QCOMPARE(fs::deviceClassOf(fs::DeviceKind::Hdd), tasks::DeviceClass::Hdd);
        QCOMPARE(fs::deviceClassOf(fs::DeviceKind::Removable), tasks::DeviceClass::Removable);
        QCOMPARE(fs::deviceClassOf(fs::DeviceKind::Unknown), tasks::DeviceClass::Unknown);

        // Dos tareas de fondo en el disco mecánico: una detrás de otra.
        std::atomic<bool> release{false};
        std::atomic<int> running{0};
        std::atomic<int> most{0};
        const auto work = [&](const tasks::CancelToken &) {
            most = std::max(most.load(), ++running);
            while (!release) {
                std::this_thread::sleep_for(1ms);
            }
            --running;
        };
        tasks::TaskHandle one = scheduler.submitForPaths(tasks::Category::General, tasks::Priority::Background,
                                                         {QStringLiteral("/media/disco/a")}, work);
        tasks::TaskHandle two = scheduler.submitForPaths(tasks::Category::General, tasks::Priority::Background,
                                                         {QStringLiteral("/media/disco/b/c")}, work);
        std::this_thread::sleep_for(100ms);
        QCOMPARE(scheduler.runningOn(QStringLiteral("/media/disco")), 1);
        release = true;
        QVERIFY(one.waitFor(5s) && two.waitFor(5s));
        QCOMPARE(most.load(), 1);
    }
};

QTEST_GUILESS_MAIN(TestDeviceMap)
#include "test_device_map.moc"
