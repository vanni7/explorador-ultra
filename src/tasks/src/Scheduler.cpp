// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/tasks/Scheduler.h"

#include <QLoggingCategory>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <functional>
#include <set>
#include <thread>
#include <vector>

namespace eu::tasks {

namespace {
Q_LOGGING_CATEGORY(logTasks, "eu.tasks")
}

/// Permite despertar a la cola de una categoría desde un TaskHandle, que
/// puede vivir más que el planificador.
struct QueueWaker
{
    std::mutex mutex;
    std::function<void()> wake;   ///< Vacía cuando la cola ya no existe.

    void notify()
    {
        std::lock_guard lock(mutex);
        if (wake) {
            wake();
        }
    }
};

/// Estado compartido entre el planificador y los TaskHandle de una tarea.
struct TaskHandle::State
{
    std::shared_ptr<QueueWaker> waker;
    CancelSource cancelSource;
    TaskFunction function;
    Priority priority = Priority::Background;
    QStringList devices;          ///< Dispositivos que necesita (sin repetir).
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
        // Si estaba esperando sitio en un dispositivo, que salga ya de la cola.
        if (m_state->waker) {
            m_state->waker->notify();
        }
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
    config.deviceLimits = {
        {DeviceClass::Ssd, 6},
        {DeviceClass::Virtual, 6},
        // Un disco mecánico rinde más con un solo flujo que con varios
        // peleándose por el cabezal; lo mismo una memoria USB o un disco óptico.
        {DeviceClass::Hdd, 1},
        {DeviceClass::Removable, 1},
        {DeviceClass::Optical, 1},
        {DeviceClass::Network, 2},
        {DeviceClass::Unknown, 2},
    };
    return config;
}

/// Cuántas tareas hay sobre cada dispositivo. Lo comparten todas las categorías.
struct Scheduler::Devices
{
    /// Tres niveles de urgencia. Una tarea solo cede ante las de su nivel o
    /// más urgentes: el trabajo de fondo no hace esperar a una copia, ni una
    /// copia a abrir una carpeta.
    enum Tier { Interactive, UserWork, Background, TierCount };

    struct Usage
    {
        int running[TierCount] = {0, 0, 0};
        int total() const { return running[Interactive] + running[UserWork] + running[Background]; }
    };

    mutable std::mutex mutex;
    std::map<DeviceClass, int> limits;
    std::map<QString, DeviceClass> classes;
    std::map<QString, Usage> usage;

    static Tier tierOf(Priority priority)
    {
        if (int(priority) <= int(Priority::VisibleNavigation)) {
            return Interactive;
        }
        return priority == Priority::Background ? Background : UserWork;
    }

    int limitOf(const QString &device) const   // con `mutex` tomado
    {
        const auto known = classes.find(device);
        const auto limit = limits.find(known != classes.end() ? known->second : DeviceClass::Unknown);
        return limit != limits.end() ? std::max(1, limit->second) : 1;
    }

    /// Reserva sitio en todos los dispositivos de la tarea, o en ninguno.
    bool tryAcquire(const QStringList &devices, Priority priority)
    {
        if (devices.isEmpty()) {
            return true;
        }
        const Tier tier = tierOf(priority);
        std::lock_guard lock(mutex);
        for (const QString &device : devices) {
            const Usage &used = usage[device];
            int ahead = 0;   // tareas de su nivel o más urgentes
            for (int level = 0; level <= int(tier); ++level) {
                ahead += used.running[level];
            }
            // Lo interactivo tiene al menos dos sitios: dos pestañas pueden
            // abrir a la vez carpetas del mismo disco.
            const int limit = tier == Interactive ? std::max(limitOf(device), 2) : limitOf(device);
            if (ahead >= limit) {
                return false;
            }
        }
        for (const QString &device : devices) {
            ++usage[device].running[tier];
        }
        return true;
    }

    void release(const QStringList &devices, Priority priority)
    {
        const Tier tier = tierOf(priority);
        std::lock_guard lock(mutex);
        for (const QString &device : devices) {
            const auto used = usage.find(device);
            if (used == usage.end()) {
                continue;
            }
            --used->second.running[tier];
            if (used->second.total() == 0) {
                usage.erase(used);
            }
        }
    }
};

/// Grupo de hilos de una categoría, con su cola ordenada por prioridad.
class Scheduler::Pool
{
public:
    Pool(int threadCount, Devices &devices, std::function<void()> deviceReleased)
        : m_devices(devices), m_deviceReleased(std::move(deviceReleased)), m_waker(std::make_shared<QueueWaker>())
    {
        m_waker->wake = [this] { wake(); };
        for (int i = 0; i < std::max(1, threadCount); ++i) {
            m_threads.emplace_back([this] { workerLoop(); });
        }
    }

    ~Pool()
    {
        stop();
        std::lock_guard lock(m_waker->mutex);
        m_waker->wake = nullptr;
    }

    /// Cancela lo pendiente y espera a las tareas en curso. Se puede llamar
    /// más de una vez.
    void stop()
    {
        std::vector<std::shared_ptr<TaskHandle::State>> abandoned;
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            abandoned.assign(m_queue.begin(), m_queue.end());
            m_queue.clear();
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
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    /// Un dispositivo quedó libre: puede que una tarea en espera ya quepa.
    void wake()
    {
        {
            std::lock_guard lock(m_mutex);   // quien está comprobando la cola no se pierde el aviso
        }
        m_wakeUp.notify_all();
    }

    void push(std::shared_ptr<TaskHandle::State> task)
    {
        {
            std::lock_guard lock(m_mutex);
            task->sequence = m_nextSequence++;
            task->waker = m_waker;
            m_queue.insert(std::move(task));
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
    /// la que llegó antes.
    struct EarlierFirst
    {
        bool operator()(const std::shared_ptr<TaskHandle::State> &a,
                        const std::shared_ptr<TaskHandle::State> &b) const
        {
            if (a->priority != b->priority) {
                return int(a->priority) < int(b->priority);
            }
            return a->sequence < b->sequence;
        }
    };

    /// Saca la primera tarea de la cola cuyos dispositivos tienen sitio (y se
    /// lo reserva). Con `m_mutex` tomado. nullptr si ninguna puede empezar.
    std::shared_ptr<TaskHandle::State> takeRunnable()
    {
        for (auto candidate = m_queue.begin(); candidate != m_queue.end(); ++candidate) {
            // Una tarea ya cancelada no necesita sitio: sale para darse por terminada.
            const bool cancelled = (*candidate)->cancelSource.token().isCancelled();
            if (cancelled || m_devices.tryAcquire((*candidate)->devices, (*candidate)->priority)) {
                std::shared_ptr<TaskHandle::State> task = *candidate;
                if (cancelled) {
                    task->devices.clear();   // no reservó nada: nada que devolver
                }
                m_queue.erase(candidate);
                return task;
            }
        }
        return nullptr;
    }

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
                while (true) {
                    if (m_stopping) {
                        return;
                    }
                    task = takeRunnable();
                    if (task) {
                        break;
                    }
                    m_wakeUp.wait(lock);
                }
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
            if (!task->devices.isEmpty()) {
                m_devices.release(task->devices, task->priority);
                m_deviceReleased();   // en cualquier categoría puede haber alguien esperándolo
            }
            markFinished(*task, failed);
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_wakeUp;
    std::set<std::shared_ptr<TaskHandle::State>, EarlierFirst> m_queue;
    Devices &m_devices;
    std::function<void()> m_deviceReleased;
    std::shared_ptr<QueueWaker> m_waker;
    std::vector<std::shared_ptr<TaskHandle::State>> m_running;
    std::vector<std::thread> m_threads;
    std::uint64_t m_nextSequence = 0;
    bool m_stopping = false;
};

Scheduler::Scheduler(const SchedulerConfig &config) : m_devices(std::make_unique<Devices>())
{
    const SchedulerConfig fallback = SchedulerConfig::defaults();
    m_devices->limits = fallback.deviceLimits;
    for (const auto &[deviceClass, limit] : config.deviceLimits) {
        m_devices->limits[deviceClass] = limit;
    }
    for (const auto &[category, defaultThreads] : fallback.threads) {
        const auto custom = config.threads.find(category);
        const int threads = (custom != config.threads.end()) ? custom->second : defaultThreads;
        m_pools[category] = std::make_unique<Pool>(threads, *m_devices, [this] { wakeAllPools(); });
    }
}

Scheduler::~Scheduler()
{
    // Primero se paran todas las categorías y solo después se destruyen: una
    // tarea que termina avisa a las demás categorías, que deben seguir vivas.
    for (const auto &[category, pool] : m_pools) {
        pool->stop();
    }
}

void Scheduler::wakeAllPools()
{
    for (const auto &[category, pool] : m_pools) {
        pool->wake();
    }
}

void Scheduler::setDeviceClass(const QString &device, DeviceClass deviceClass)
{
    {
        std::lock_guard lock(m_devices->mutex);
        m_devices->classes[device] = deviceClass;
    }
    wakeAllPools();
}

void Scheduler::setDeviceLimit(DeviceClass deviceClass, int maxTasks)
{
    {
        std::lock_guard lock(m_devices->mutex);
        m_devices->limits[deviceClass] = std::max(1, maxTasks);
    }
    wakeAllPools();
}

int Scheduler::deviceLimit(DeviceClass deviceClass) const
{
    std::lock_guard lock(m_devices->mutex);
    const auto limit = m_devices->limits.find(deviceClass);
    return limit != m_devices->limits.end() ? std::max(1, limit->second) : 1;
}

int Scheduler::runningOn(const QString &device) const
{
    std::lock_guard lock(m_devices->mutex);
    const auto used = m_devices->usage.find(device);
    return used != m_devices->usage.end() ? used->second.total() : 0;
}

void Scheduler::setDeviceResolver(DeviceResolver resolver)
{
    auto shared = std::make_shared<const DeviceResolver>(std::move(resolver));
    std::lock_guard lock(m_devices->mutex);
    m_resolver = std::move(shared);
}

TaskHandle Scheduler::submitForPaths(Category category, Priority priority, const QStringList &paths,
                                     TaskFunction function)
{
    QStringList devices;
    std::shared_ptr<const DeviceResolver> resolver;
    {
        std::lock_guard lock(m_devices->mutex);
        resolver = m_resolver;
    }
    if (resolver && *resolver) {
        for (const QString &path : paths) {
            devices.append((*resolver)(path));
        }
    }
    return submit(category, priority, devices, std::move(function));
}

TaskHandle Scheduler::submit(Category category, Priority priority, TaskFunction function)
{
    return submit(category, priority, QStringList(), std::move(function));
}

TaskHandle Scheduler::submit(Category category, Priority priority, const QStringList &devices,
                             TaskFunction function)
{
    auto state = std::make_shared<TaskHandle::State>();
    state->function = std::move(function);
    state->priority = priority;
    for (const QString &device : devices) {
        if (!device.isEmpty() && !state->devices.contains(device)) {
            state->devices.append(device);
        }
    }

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
