// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/tasks/Scheduler.h"

#include <QTest>

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace eu::tasks;
using namespace std::chrono_literals;

namespace {

/// Planificador con un solo hilo en General, para poder predecir el orden.
SchedulerConfig singleThread()
{
    SchedulerConfig config;
    config.threads[Category::General] = 1;
    return config;
}

/// Tarea que ocupa el hilo hasta que la prueba la suelta.
struct Gate
{
    std::atomic<bool> open{false};
    std::atomic<bool> entered{false};

    TaskFunction task()
    {
        return [this](const CancelToken &) {
            entered = true;
            while (!open) {
                std::this_thread::sleep_for(1ms);
            }
        };
    }

    void waitUntilEntered() const
    {
        while (!entered) {
            std::this_thread::sleep_for(1ms);
        }
    }
};

} // namespace

class TestScheduler : public QObject
{
    Q_OBJECT

private slots:
    void runsATask()
    {
        Scheduler scheduler;
        std::atomic<int> value{0};
        TaskHandle handle = scheduler.submit(Category::General, Priority::Background,
                                             [&](const CancelToken &) { value = 7; });
        QVERIFY(handle.waitFor(5s));
        QVERIFY(handle.isFinished());
        QVERIFY(!handle.failed());
        QCOMPARE(value.load(), 7);
    }

    void urgentTasksRunFirstAndTiesKeepArrivalOrder()
    {
        Scheduler scheduler(singleThread());
        Gate gate;
        scheduler.submit(Category::General, Priority::Background, gate.task());
        gate.waitUntilEntered();   // el único hilo está ocupado: lo demás se encola

        std::mutex mutex;
        std::vector<int> order;
        const auto record = [&](int id) {
            return [&, id](const CancelToken &) {
                std::lock_guard lock(mutex);
                order.push_back(id);
            };
        };
        scheduler.submit(Category::General, Priority::Background, record(1));
        scheduler.submit(Category::General, Priority::VisibleThumbnail, record(2));
        scheduler.submit(Category::General, Priority::UserInteraction, record(3));
        scheduler.submit(Category::General, Priority::Background, record(4));
        TaskHandle last = scheduler.submit(Category::General, Priority::UserInteraction, record(5));
        QCOMPARE(scheduler.pendingCount(Category::General), 5);

        gate.open = true;
        // La última en ejecutarse es la 4 (Background, llegó después de la 1).
        while (true) {
            std::lock_guard lock(mutex);
            if (order.size() == 5) {
                break;
            }
        }
        QVERIFY(last.waitFor(5s));
        QCOMPARE(order, (std::vector<int>{3, 5, 2, 1, 4}));
    }

    void cancelledBeforeStartNeverRuns()
    {
        Scheduler scheduler(singleThread());
        Gate gate;
        scheduler.submit(Category::General, Priority::Background, gate.task());
        gate.waitUntilEntered();

        std::atomic<bool> ran{false};
        TaskHandle handle = scheduler.submit(Category::General, Priority::Background,
                                             [&](const CancelToken &) { ran = true; });
        handle.cancel();
        gate.open = true;
        QVERIFY(handle.waitFor(5s));
        QVERIFY(!ran);
    }

    void runningTaskStopsCooperatively()
    {
        Scheduler scheduler;
        std::atomic<bool> started{false};
        std::atomic<bool> sawCancel{false};
        TaskHandle handle = scheduler.submit(Category::Transfer, Priority::ActiveTransfer,
                                             [&](const CancelToken &token) {
            started = true;
            while (!token.isCancelled()) {
                std::this_thread::sleep_for(1ms);
            }
            sawCancel = true;
        });
        while (!started) {
            std::this_thread::sleep_for(1ms);
        }
        handle.cancel();
        // Meta de SPEC §4: la cancelación es efectiva en menos de 500 ms.
        QVERIFY(handle.waitFor(500ms));
        QVERIFY(sawCancel);
    }

    void exceptionsAreContained()
    {
        Scheduler scheduler(singleThread());
        QTest::ignoreMessage(QtCriticalMsg, "Una tarea lanzó una excepción: fallo de prueba");
        TaskHandle bad = scheduler.submit(Category::General, Priority::Background, [](const CancelToken &) {
            throw std::runtime_error("fallo de prueba");
        });
        QVERIFY(bad.waitFor(5s));
        QVERIFY(bad.failed());

        // El hilo sigue vivo y atiende la siguiente tarea.
        std::atomic<bool> ran{false};
        TaskHandle good = scheduler.submit(Category::General, Priority::Background,
                                           [&](const CancelToken &) { ran = true; });
        QVERIFY(good.waitFor(5s));
        QVERIFY(ran);
    }

    void categoriesDoNotBlockEachOther()
    {
        Scheduler scheduler(singleThread());
        Gate gate;
        scheduler.submit(Category::General, Priority::Background, gate.task());
        gate.waitUntilEntered();

        // General está bloqueado, pero Listing tiene sus propios hilos.
        std::atomic<bool> ran{false};
        TaskHandle listing = scheduler.submit(Category::Listing, Priority::VisibleNavigation,
                                              [&](const CancelToken &) { ran = true; });
        QVERIFY(listing.waitFor(5s));
        QVERIFY(ran);
        gate.open = true;
    }

    void manyTasksFromManyThreads()
    {
        Scheduler scheduler;
        std::atomic<int> counter{0};
        std::vector<std::thread> producers;
        std::mutex mutex;
        std::vector<TaskHandle> handles;
        for (int producer = 0; producer < 4; ++producer) {
            producers.emplace_back([&] {
                for (int i = 0; i < 500; ++i) {
                    TaskHandle handle = scheduler.submit(Category::Thumbnails, Priority::VisibleThumbnail,
                                                         [&](const CancelToken &) { ++counter; });
                    std::lock_guard lock(mutex);
                    handles.push_back(handle);
                }
            });
        }
        for (std::thread &thread : producers) {
            thread.join();
        }
        for (const TaskHandle &handle : handles) {
            QVERIFY(handle.waitFor(10s));
        }
        QCOMPARE(counter.load(), 2000);
    }

    void destructorCancelsPendingAndWaitsForRunning()
    {
        std::atomic<bool> pendingRan{false};
        std::atomic<bool> runningFinished{false};
        TaskHandle pending;
        {
            Scheduler scheduler(singleThread());
            std::atomic<bool> started{false};
            scheduler.submit(Category::General, Priority::Background, [&](const CancelToken &token) {
                started = true;
                while (!token.isCancelled()) {
                    std::this_thread::sleep_for(1ms);
                }
                runningFinished = true;
            });
            while (!started) {
                std::this_thread::sleep_for(1ms);
            }
            pending = scheduler.submit(Category::General, Priority::Background,
                                       [&](const CancelToken &) { pendingRan = true; });
        }   // aquí se destruye el planificador
        QVERIFY(runningFinished);
        QVERIFY(!pendingRan);
        QVERIFY(pending.isFinished());
    }

    void aHardDiskRunsOneBackgroundTaskAtATime()
    {
        SchedulerConfig config;
        config.threads[Category::General] = 4;
        Scheduler scheduler(config);
        const QString disk = QStringLiteral("/media/disco");
        scheduler.setDeviceClass(disk, DeviceClass::Hdd);
        QCOMPARE(scheduler.deviceLimit(DeviceClass::Hdd), 1);

        Gate first;
        Gate second;
        Gate elsewhere;
        TaskHandle one = scheduler.submit(Category::General, Priority::Background, {disk}, first.task());
        first.waitUntilEntered();
        TaskHandle two = scheduler.submit(Category::General, Priority::Background, {disk}, second.task());
        // Hay hilos libres, pero el disco ya tiene su tarea: la segunda espera…
        TaskHandle other = scheduler.submit(Category::General, Priority::Background, elsewhere.task());
        elsewhere.waitUntilEntered();   // …y la que no usa ese disco la adelanta.
        std::this_thread::sleep_for(50ms);
        QVERIFY(!second.entered);
        QCOMPARE(scheduler.runningOn(disk), 1);
        QCOMPARE(scheduler.pendingCount(Category::General), 1);

        first.open = true;
        second.waitUntilEntered();   // al acabar la primera entra la segunda
        QCOMPARE(scheduler.runningOn(disk), 1);
        second.open = true;
        elsewhere.open = true;
        QVERIFY(one.waitFor(5s) && two.waitFor(5s) && other.waitFor(5s));
        QCOMPARE(scheduler.runningOn(disk), 0);
    }

    void theDeviceLimitCountsAcrossCategories()
    {
        Scheduler scheduler;
        const QString disk = QStringLiteral("usb");
        scheduler.setDeviceClass(disk, DeviceClass::Removable);
        Gate copy;
        Gate sizes;
        TaskHandle one = scheduler.submit(Category::Transfer, Priority::ActiveTransfer, {disk}, copy.task());
        copy.waitUntilEntered();
        TaskHandle two = scheduler.submit(Category::Metadata, Priority::Background, {disk}, sizes.task());
        std::this_thread::sleep_for(50ms);
        QVERIFY(!sizes.entered);   // otra categoría, con hilos libres, pero el mismo disco
        copy.open = true;
        sizes.waitUntilEntered();
        sizes.open = true;
        QVERIFY(one.waitFor(5s) && two.waitFor(5s));
    }

    void whatTheUserIsWaitingForDoesNotQueueBehindBackgroundWork()
    {
        Scheduler scheduler;
        const QString disk = QStringLiteral("hdd");
        scheduler.setDeviceClass(disk, DeviceClass::Hdd);
        Gate copy;
        TaskHandle background = scheduler.submit(Category::Transfer, Priority::ActiveTransfer, {disk}, copy.task());
        copy.waitUntilEntered();
        // Abrir una carpeta de ese disco no espera a que termine la copia.
        Gate listing;
        Gate secondListing;
        Gate thirdListing;
        TaskHandle one = scheduler.submit(Category::Listing, Priority::VisibleNavigation, {disk}, listing.task());
        TaskHandle two = scheduler.submit(Category::Listing, Priority::UserInteraction, {disk}, secondListing.task());
        listing.waitUntilEntered();
        secondListing.waitUntilEntered();
        QCOMPARE(scheduler.runningOn(disk), 3);
        // Pero lo interactivo también tiene tope: dos a la vez en un disco mecánico.
        SchedulerConfig config;
        QVERIFY(SchedulerConfig::defaults().threads.at(Category::Listing) >= 2);
        TaskHandle three = scheduler.submit(Category::General, Priority::UserInteraction, {disk}, thirdListing.task());
        std::this_thread::sleep_for(50ms);
        QVERIFY(!thirdListing.entered);
        listing.open = true;
        thirdListing.waitUntilEntered();
        // Y mientras haya algo interactivo, lo de fondo nuevo espera.
        Gate moreBackground;
        TaskHandle later = scheduler.submit(Category::Metadata, Priority::Background, {disk}, moreBackground.task());
        copy.open = true;
        QVERIFY(background.waitFor(5s));
        std::this_thread::sleep_for(50ms);
        QVERIFY(!moreBackground.entered);
        secondListing.open = true;
        thirdListing.open = true;
        moreBackground.waitUntilEntered();
        moreBackground.open = true;
        QVERIFY(one.waitFor(5s) && two.waitFor(5s) && three.waitFor(5s) && later.waitFor(5s));
    }

    void backgroundWorkDoesNotHoldUpACopy()
    {
        Scheduler scheduler;
        const QString disk = QStringLiteral("hdd");
        scheduler.setDeviceClass(disk, DeviceClass::Hdd);
        Gate indexing;
        Gate copy;
        Gate secondCopy;
        TaskHandle one = scheduler.submit(Category::Indexing, Priority::Background, {disk}, indexing.task());
        indexing.waitUntilEntered();
        // Indexar puede durar minutos: la copia que pide el usuario no espera a eso…
        TaskHandle two = scheduler.submit(Category::Transfer, Priority::ActiveTransfer, {disk}, copy.task());
        copy.waitUntilEntered();
        // …pero dos copias al mismo disco mecánico sí van una detrás de otra.
        TaskHandle three = scheduler.submit(Category::Transfer, Priority::ActiveTransfer, {disk}, secondCopy.task());
        std::this_thread::sleep_for(50ms);
        QVERIFY(!secondCopy.entered);
        copy.open = true;
        secondCopy.waitUntilEntered();
        secondCopy.open = true;
        indexing.open = true;
        QVERIFY(one.waitFor(5s) && two.waitFor(5s) && three.waitFor(5s));
    }

    void pathsAreMappedToDevices()
    {
        Scheduler scheduler;
        scheduler.setDeviceClass(QStringLiteral("/mnt/a"), DeviceClass::Hdd);
        Gate busy;
        Gate waiting;
        // Sin nadie que diga de qué dispositivo es una ruta, no se limita nada.
        TaskHandle free = scheduler.submitForPaths(Category::General, Priority::Background,
                                                   {QStringLiteral("/mnt/a/x")}, [](const CancelToken &) {});
        QVERIFY(free.waitFor(5s));
        scheduler.setDeviceResolver([](const QString &path) {
            return path.startsWith(QStringLiteral("/mnt/a")) ? QStringLiteral("/mnt/a") : QString();
        });
        TaskHandle one = scheduler.submitForPaths(Category::General, Priority::Background,
                                                  {QStringLiteral("/mnt/a/x")}, busy.task());
        busy.waitUntilEntered();
        QCOMPARE(scheduler.runningOn(QStringLiteral("/mnt/a")), 1);
        TaskHandle two = scheduler.submitForPaths(Category::General, Priority::Background,
                                                  {QStringLiteral("/mnt/a/y"), QStringLiteral("/otra")}, waiting.task());
        std::this_thread::sleep_for(50ms);
        QVERIFY(!waiting.entered);
        busy.open = true;
        waiting.waitUntilEntered();
        waiting.open = true;
        QVERIFY(one.waitFor(5s) && two.waitFor(5s));
    }

    void aTaskNeedsRoomOnAllItsDevices()
    {
        SchedulerConfig config;
        config.threads[Category::Transfer] = 4;
        Scheduler scheduler(config);
        const QString source = QStringLiteral("origen");
        const QString target = QStringLiteral("destino");
        scheduler.setDeviceClass(source, DeviceClass::Hdd);
        scheduler.setDeviceClass(target, DeviceClass::Hdd);
        Gate busyTarget;
        Gate copy;
        Gate onSource;
        TaskHandle one = scheduler.submit(Category::Transfer, Priority::Background, {target}, busyTarget.task());
        busyTarget.waitUntilEntered();
        // La copia necesita los dos discos: no ocupa el origen mientras espera al destino.
        TaskHandle two = scheduler.submit(Category::Transfer, Priority::Background, {source, target, source},
                                          copy.task());
        TaskHandle three = scheduler.submit(Category::Transfer, Priority::Background, {source}, onSource.task());
        onSource.waitUntilEntered();
        QVERIFY(!copy.entered);
        QCOMPARE(scheduler.runningOn(source), 1);
        busyTarget.open = true;
        onSource.open = true;
        copy.waitUntilEntered();
        QCOMPARE(scheduler.runningOn(source), 1);   // repetido en la lista, cuenta una vez
        QCOMPARE(scheduler.runningOn(target), 1);
        copy.open = true;
        QVERIFY(one.waitFor(5s) && two.waitFor(5s) && three.waitFor(5s));
    }

    void limitsAreConfigurableAndUnknownDevicesAreCautious()
    {
        SchedulerConfig config;
        config.threads[Category::General] = 6;
        config.deviceLimits[DeviceClass::Ssd] = 3;
        Scheduler scheduler(config);
        QCOMPARE(scheduler.deviceLimit(DeviceClass::Ssd), 3);
        QCOMPARE(scheduler.deviceLimit(DeviceClass::Unknown), 2);   // sin saber qué es, dos
        const QString disk = QStringLiteral("ssd");
        scheduler.setDeviceClass(disk, DeviceClass::Ssd);
        std::vector<std::unique_ptr<Gate>> gates;
        std::vector<TaskHandle> handles;
        for (int i = 0; i < 5; ++i) {
            gates.push_back(std::make_unique<Gate>());
            handles.push_back(scheduler.submit(Category::General, Priority::Background, {disk}, gates.back()->task()));
        }
        for (int i = 0; i < 3; ++i) {
            gates[size_t(i)]->waitUntilEntered();
        }
        std::this_thread::sleep_for(50ms);
        QCOMPARE(scheduler.runningOn(disk), 3);
        // Subir el límite deja pasar a las que esperaban, sin que termine ninguna.
        scheduler.setDeviceLimit(DeviceClass::Ssd, 5);
        gates[3]->waitUntilEntered();
        gates[4]->waitUntilEntered();
        QCOMPARE(scheduler.runningOn(disk), 5);
        for (const auto &gate : gates) {
            gate->open = true;
        }
        for (const TaskHandle &handle : handles) {
            QVERIFY(handle.waitFor(5s));
        }
    }

    void cancellingATaskThatWaitsForADeviceFinishesItAtOnce()
    {
        Scheduler scheduler;
        const QString disk = QStringLiteral("hdd");
        scheduler.setDeviceClass(disk, DeviceClass::Hdd);
        Gate busy;
        TaskHandle running = scheduler.submit(Category::General, Priority::Background, {disk}, busy.task());
        busy.waitUntilEntered();
        std::atomic<bool> ran{false};
        TaskHandle waiting = scheduler.submit(Category::General, Priority::Background, {disk},
                                              [&](const CancelToken &) { ran = true; });
        waiting.cancel();
        QVERIFY(waiting.waitFor(2s));   // no espera a que el disco quede libre
        QVERIFY(!ran);
        QCOMPARE(scheduler.runningOn(disk), 1);
        busy.open = true;
        QVERIFY(running.waitFor(5s));
        QCOMPARE(scheduler.runningOn(disk), 0);
    }

    void emptyHandleIsHarmless()
    {
        TaskHandle handle;
        QVERIFY(!handle.isValid());
        handle.cancel();
        QVERIFY(handle.isFinished());
        QVERIFY(handle.waitFor(1ms));
    }
};

QTEST_APPLESS_MAIN(TestScheduler)
#include "test_scheduler.moc"
