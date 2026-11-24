// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
// Mediciones con la ventana real (SPEC §4, Fase 6): abrir una carpeta grande,
// cuánto se bloquea el hilo de la interfaz, memoria por pestaña, ordenar y
// cancelar. No forma parte de la aplicación.
//
//   eu-bench-ui open <carpeta>     abrir, ordenar y memoria
//   eu-bench-ui cancel <carpeta>   cancelar una medida de tamaño en curso
#include "eu/core/CrashReport.h"
#include "eu/fs/TreeSize.h"
#include "eu/tasks/Scheduler.h"
#include "eu/ui/BrowserPane.h"
#include "eu/ui/MainWindow.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <algorithm>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace eu;

namespace {

QTextStream &out()
{
    // Sin búfer: si el proceso se cae, lo medido hasta ahí queda escrito.
    static QTextStream stream(stdout);
    stream.setAutoDetectUnicode(false);
    stream.flush();
    return stream;
}

/// Memoria residente del proceso en MB (Linux y Windows; -1 donde no se sabe).
double residentMb()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return double(counters.WorkingSetSize) / (1024.0 * 1024.0);
    }
    return -1;
#else
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly)) {
        return -1;
    }
    for (const QByteArray &line : status.readAll().split('\n')) {
        if (line.startsWith("VmRSS:")) {
            return line.mid(6).trimmed().split(' ').value(0).toDouble() / 1024.0;
        }
    }
    return -1;
#endif
}

/// Vigila el hilo de la interfaz: un temporizador de 1 ms anota el mayor hueco
/// entre dos vueltas del bucle de eventos (= el bloqueo más largo).
class StallMeter : public QObject
{
public:
    StallMeter()
    {
        m_timer.setInterval(1);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            const qint64 now = m_clock.nsecsElapsed();
            if (m_last > 0) {
                m_worst = std::max(m_worst, now - m_last);
            }
            m_last = now;
        });
    }
    void start()
    {
        m_worst = 0;
        m_last = 0;
        m_clock.start();
        m_timer.start();
    }
    double worstMs() const { return double(m_worst) / 1e6; }

private:
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_last = 0;
    qint64 m_worst = 0;
};

/// Deja correr el bucle de eventos hasta que `done` sea cierto o pase `limitMs`.
bool spinUntil(const std::function<bool()> &done, int limitMs = 60000)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < limitMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return done();
}

int commandOpen(const QString &folder)
{
    tasks::Scheduler scheduler;
    qInfo("creando la ventana");
    ui::MainWindow window(scheduler);
    qInfo("ventana creada");
    window.resize(1400, 900);
    window.show();
    spinUntil([] { return false; }, 500);   // que termine de arrancar
    const double before = residentMb();
    qInfo("abriendo");

    StallMeter stalls;
    stalls.start();
    QElapsedTimer clock;
    clock.start();
    ui::BrowserPane *pane = window.openTab(folder);
    qInfo("pestaña abierta (%lld ms)", clock.elapsed());
    qint64 firstRows = -1;
    const bool ready = spinUntil([&] {
        if (firstRows < 0 && pane->model()->rowCount() > 0) {
            firstRows = clock.elapsed();
        }
        static qint64 lastPrint = 0;
        if (clock.elapsed() - lastPrint > 2000) {
            lastPrint = clock.elapsed();
            qInfo("filas=%d estado=%d", pane->model()->rowCount(), int(pane->model()->state()));
        }
        return pane->model()->state() == ui::DirectoryModel::State::Ready;
    }, 15000);
    const qint64 full = clock.elapsed();
    const double openStall = stalls.worstMs();
    spinUntil([] { return false; }, 1000);   // lo que quede pendiente (iconos, árbol)
    const double after = residentMb();

    out() << "Carpeta: " << folder << " · " << pane->model()->rowCount() << " elementos\n" << Qt::flush;
    out() << "Primeros elementos en el modelo: " << firstRows << " ms\n" << Qt::flush;
    out() << "Listado completo: " << full << " ms" << (ready ? "" : " (NO TERMINÓ)") << Qt::endl;
    out() << "Bloqueo máximo de la interfaz al abrir: " << QString::number(openStall, 'f', 1) << " ms\n" << Qt::flush;
    out() << "Memoria: " << QString::number(before, 'f', 0) << " MB antes, " << QString::number(after, 'f', 0)
          << " MB después (+" << QString::number(after - before, 'f', 0) << " MB)\n" << Qt::flush;

    // Ordenar por tamaño y volver por nombre (en el hilo de la interfaz).
    for (const auto &[column, order, name] :
         {std::tuple{ui::Column::Size, Qt::DescendingOrder, "tamaño ↓"},
          std::tuple{ui::Column::Modified, Qt::AscendingOrder, "fecha ↑"},
          std::tuple{ui::Column::Name, Qt::AscendingOrder, "nombre ↑"}}) {
        stalls.start();
        clock.restart();
        pane->sortBy(column, order);
        const qint64 took = clock.elapsed();
        spinUntil([] { return false; }, 300);
        out() << "Ordenar por " << name << ": " << took << " ms (bloqueo máximo "
              << QString::number(stalls.worstMs(), 'f', 1) << " ms)\n" << Qt::flush;
    }
    // Desplazarse de arriba abajo por la lista.
    stalls.start();
    pane->selectAll();
    spinUntil([] { return false; }, 300);
    out() << "Seleccionar todo: bloqueo máximo " << QString::number(stalls.worstMs(), 'f', 1) << " ms\n" << Qt::flush;
    // Releer la carpeta con todo seleccionado (al cambiar algo dentro): la
    // selección se recupera por nombre.
    stalls.start();
    clock.restart();
    pane->refresh();
    spinUntil([&] { return pane->model()->state() == ui::DirectoryModel::State::Ready; });
    spinUntil([] { return false; }, 300);
    out() << "Releer con todo seleccionado: " << clock.elapsed() << " ms, bloqueo máximo "
          << QString::number(stalls.worstMs(), 'f', 1) << " ms, " << pane->selectedCount() << " seleccionados\n" << Qt::flush;
    out().flush();
    return ready ? 0 : 1;
}

int commandCancel(const QString &folder)
{
    tasks::Scheduler scheduler;
    // Medir el tamaño de un árbol grande y cancelar a los 300 ms, cinco veces.
    for (int round = 0; round < 5; ++round) {
        std::atomic<qint64> seen{0};
        tasks::TaskHandle handle = scheduler.submit(
            tasks::Category::Metadata, tasks::Priority::UserInteraction, [&](const tasks::CancelToken &cancel) {
                fs::measureTree(folder, cancel, [&](const fs::TreeSize &partial) { seen = partial.files; });
            });
        QThread::msleep(300);
        QElapsedTimer clock;
        clock.start();
        handle.cancel();
        handle.waitFor(std::chrono::milliseconds(10000));
        out() << "Vuelta " << round + 1 << ": cancelada en " << QString::number(clock.nsecsElapsed() / 1e6, 'f', 2)
              << " ms (" << seen.load() << " archivos contados)\n" << Qt::flush;
    }
    out().flush();
    return 0;
}

/// Con EU_BENCH_EVENTOS=1 dice qué evento tuvo ocupado el hilo de la
/// interfaz más de 20 ms (a quién iba y de qué tipo).
class TimedApplication : public QApplication
{
public:
    using QApplication::QApplication;
    bool notify(QObject *receiver, QEvent *event) override
    {
        if (!m_enabled) {
            return QApplication::notify(receiver, event);
        }
        QElapsedTimer clock;
        clock.start();
        const int type = event->type();
        const bool result = QApplication::notify(receiver, event);
        const qint64 ms = clock.elapsed();
        if (ms > 20) {
            qInfo("evento %d a %s (%s): %lld ms", type, receiver->metaObject()->className(),
                  qPrintable(receiver->objectName()), ms);
        }
        return result;
    }

private:
    bool m_enabled = qEnvironmentVariableIntValue("EU_BENCH_EVENTOS") == 1;
};

} // namespace

int main(int argc, char *argv[])
{
    TimedApplication application(argc, argv);
    // Ni la sesión ni los ajustes ni el historial del usuario: todo aparte.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("eu-bench-ui"));
    QCoreApplication::setApplicationName(QStringLiteral("eu-bench-ui"));
    QSettings().clear();   // cada vuelta empieza sin la sesión de la anterior
    QSettings().setValue(QStringLiteral("index/asked"), true);   // sin el asistente del índice
    const QStringList arguments = QCoreApplication::arguments();
    const QString command = arguments.value(1);
    // Si se cae, el registro de fallos de la aplicación dice dónde (con la pila).
    if (qEnvironmentVariableIsSet("EU_BENCH_FALLOS")) {
        core::installCrashReport(qEnvironmentVariable("EU_BENCH_FALLOS"));
    }
    // Vigilante: si el hilo de la interfaz pasa 20 s sin dar una vuelta, se
    // dice y se sale (si no, una medición colgada no terminaría nunca).
    static std::atomic<qint64> lastTick{0};
    static QElapsedTimer sinceStart;   // estático: el vigilante lo lee hasta el final
    sinceStart.start();
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, [] { lastTick = sinceStart.elapsed(); });
    tick.start(100);
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (sinceStart.elapsed() - lastTick.load() > 20000) {
                std::fprintf(stderr, "BLOQUEADO: el hilo de la interfaz lleva más de 20 s sin responder\n");
                std::fflush(stderr);
                std::fflush(stdout);
                std::_Exit(3);
            }
        }
    }).detach();
    if (command == QStringLiteral("open") && arguments.size() == 3) {
        return commandOpen(arguments[2]);
    }
    if (command == QStringLiteral("cancel") && arguments.size() == 3) {
        return commandCancel(arguments[2]);
    }
    out() << "Uso: eu-bench-ui open <carpeta> | cancel <carpeta>\n" << Qt::flush;
    return 2;
}
