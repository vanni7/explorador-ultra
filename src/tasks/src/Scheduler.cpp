// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/tasks/Scheduler.h"

#include <QLoggingCategory>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace eu::tasks {

namespace {
Q_LOGGING_CATEGORY(logTasks, "eu.tasks")
}

/// Estado compartido entre el planificador y los TaskHandle de una tarea.
struct TaskHandle::State
{
    CancelSource cancelSource;
    TaskFunction function;
    Priority priority = Priority::Background;
    std::uint64_t sequence = 0;   ///< Orden de llegada, para desempatar.

    mutable std::mutex mutex;
    mutable std::condition_variable finishedCondition;
    bool finished = false;
    bool failed = false;
};

void TaskHandle::cancel()
{
    if (m_state) {
        m_state->cancelSource.cancel();
    }
}

bool TaskHandle::isFinished() const
{
    if (!m_state) {
        return true;
    }
    std::lock_guard lock(m_state->mutex);
    return m_state->finished;
}

bool TaskHandle::failed() const
{
    if (!m_state) {
        return false;
    }
    std::lock_guard lock(m_state->mutex);
    return m_state->failed;
}

bool TaskHandle::waitFor(std::chrono::milliseconds timeout) const
{
    if (!m_state) {
        return true;
    }
    std::unique_lock lock(m_state->mutex);
    return m_state->finishedCondition.wait_for(lock, timeout, [this] { return m_state->finished; });
}

SchedulerConfig SchedulerConfig::defaults()
{
    const int cores = std::max(2, int(std::thread::hardware_concurrency()));
    SchedulerConfig config;
    config.threads = {
        {Category::Listing, std::clamp(cores / 2, 2, 4)},
        {Category::Transfer, 2},
        {Category::Hashing, std::clamp(cores / 2, 1, 4)},
        {Category::Media, 2},
        {Category::Thumbnails, std::clamp(cores / 2, 2, 6)},
        // Tres: el índice usa uno como mucho (IndexService) y quedan dos para
        // las búsquedas, que así nunca esperan a que termine de indexar.
        {Category::Indexing, 3},
        {Category::Metadata, 2},
        {Category::Compression, 2},
        {Category::General, 2},
    };
    return config;
}

/// Grupo de hilos de una categoría, con su cola ordenada por prioridad.
class Scheduler::Pool
{
public:
    explicit Pool(int threadCount)
    {
        for (int i = 0; i < std::max(1, threadCount); ++i) {
            m_threads.emplace_back([this] { workerLoop(); });
        }
    }

    ~Pool()
    {
        std::vector<std::shared_ptr<TaskHandle::State>> abandoned;
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            while (!m_queue.empty()) {
                abandoned.push_back(m_queue.top());
                m_queue.pop();
            }
        }
        m_wakeUp.notify_all();

        // Lo que no llegó a empezar se da por cancelado y terminado, para que
        // nadie se quede esperándolo para siempre.
        for (const auto &task : abandoned) {
            task->cancelSource.cancel();
            markFinished(*task, false);
        }
        // A las tareas en curso se les pide parar y se espera a que salgan
        // por un punto seguro.
        {
            std::lock_guard lock(m_mutex);
            for (const auto &task : m_running) {
                task->cancelSource.cancel();
            }
        }
        for (std::thread &thread : m_threads) {
            thread.join();
        }
    }

    void push(std::shared_ptr<TaskHandle::State> task)
    {
        {
            std::lock_guard lock(m_mutex);
            task->sequence = m_nextSequence++;
            m_queue.push(std::move(task));
        }
        m_wakeUp.notify_one();
    }

    int pendingCount() const
    {
        std::lock_guard lock(m_mutex);
        return int(m_queue.size());
    }

private:
    /// Orden de la cola: primero la prioridad más urgente; a igual prioridad,
    /// la que llegó antes. (priority_queue saca el "mayor", de ahí los signos.)
    struct LaterFirst
    {
        bool operator()(const std::shared_ptr<TaskHandle::State> &a,
                        const std::shared_ptr<TaskHandle::State> &b) const
        {
            if (a->priority != b->priority) {
                return int(a->priority) > int(b->priority);
            }
            return a->sequence > b->sequence;
        }
    };

    static void markFinished(TaskHandle::State &task, bool failed)
    {
        {
            std::lock_guard lock(task.mutex);
            task.finished = true;
            task.failed = failed;
        }
        task.finishedCondition.notify_all();
    }

    void workerLoop()
    {
        while (true) {
            std::shared_ptr<TaskHandle::State> task;
            {
                std::unique_lock lock(m_mutex);
                m_wakeUp.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
                if (m_stopping) {
                    return;
                }
                task = m_queue.top();
                m_queue.pop();
                m_running.push_back(task);
            }

            bool failed = false;
            // Una tarea cancelada antes de empezar no se ejecuta.
            if (!task->cancelSource.token().isCancelled()) {
                try {
                    task->function(task->cancelSource.token());
                } catch (const std::exception &exception) {
                    failed = true;
                    qCCritical(logTasks, "Una tarea lanzó una excepción: %s", exception.what());
                } catch (...) {
                    failed = true;
                    qCCritical(logTasks, "Una tarea lanzó una excepción desconocida.");
                }
            }
            // Se libera la función ya: puede retener memoria o referencias.
            task->function = nullptr;

            {
                std::lock_guard lock(m_mutex);
                m_running.erase(std::find(m_running.begin(), m_running.end(), task));
            }
            markFinished(*task, failed);
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_wakeUp;
    std::priority_queue<std::shared_ptr<TaskHandle::State>,
                        std::vector<std::shared_ptr<TaskHandle::State>>, LaterFirst> m_queue;
    std::vector<std::shared_ptr<TaskHandle::State>> m_running;
    std::vector<std::thread> m_threads;
    std::uint64_t m_nextSequence = 0;
    bool m_stopping = false;
};

Scheduler::Scheduler(const SchedulerConfig &config)
{
    const SchedulerConfig fallback = SchedulerConfig::defaults();
    for (const auto &[category, defaultThreads] : fallback.threads) {
        const auto custom = config.threads.find(category);
        const int threads = (custom != config.threads.end()) ? custom->second : defaultThreads;
        m_pools[category] = std::make_unique<Pool>(threads);
    }
}

Scheduler::~Scheduler() = default;

TaskHandle Scheduler::submit(Category category, Priority priority, TaskFunction function)
{
    auto state = std::make_shared<TaskHandle::State>();
    state->function = std::move(function);
    state->priority = priority;

    TaskHandle handle;
    handle.m_state = state;
    m_pools.at(category)->push(std::move(state));
    return handle;
}

int Scheduler::pendingCount(Category category) const
{
    return m_pools.at(category)->pendingCount();
}

} // namespace eu::tasks
