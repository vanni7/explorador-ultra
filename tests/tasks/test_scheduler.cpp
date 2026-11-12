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
