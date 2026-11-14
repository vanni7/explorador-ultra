// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/tasks/Cancel.h"

#include <QString>
#include <QStringList>

#include <chrono>
#include <functional>
#include <map>
#include <memory>

namespace eu::tasks {

/// Tipos de trabajo. Cada uno tiene su propio grupo de hilos, de modo que, por
/// ejemplo, generar miniaturas nunca deja sin hilos al listado de carpetas (SPEC §3.3).
enum class Category {
    Listing,     ///< Listar carpetas.
    Transfer,    ///< Copiar y mover.
    Hashing,     ///< Sumas de verificación.
    Media,       ///< Decodificación de audio y video.
    Thumbnails,  ///< Miniaturas.
    Indexing,    ///< Índice de búsqueda.
    Metadata,    ///< Metadatos y tamaño de carpetas.
    Compression, ///< Archivos comprimidos.
    General,     ///< Lo que no encaja en lo anterior.
};

/// Urgencia de una tarea dentro de su categoría. Menor valor = se atiende antes.
enum class Priority {
    UserInteraction = 0,   ///< Respuesta directa a una acción del usuario.
    VisibleNavigation = 1, ///< Contenido de lo que se está viendo.
    RequestedPreview = 2,  ///< Previsualización pedida.
    ActiveTransfer = 3,    ///< Transferencias en curso.
    VisibleThumbnail = 4,  ///< Miniaturas a la vista.
    Background = 5,        ///< Trabajo secundario.
};

/// Clase de dispositivo sobre el que trabaja una tarea (SPEC §3.3).
enum class DeviceClass {
    Unknown,    ///< No se sabe qué es: se trata con prudencia.
    Ssd,
    Hdd,        ///< Disco mecánico: un solo flujo secuencial.
    Removable,  ///< USB y similares.
    Network,
    Optical,
    Virtual,    ///< Memoria (tmpfs) e imágenes montadas.
};

/// Cuántos hilos tiene cada categoría y cuántas tareas admite a la vez cada
/// clase de dispositivo. Todo es configurable (SPEC §3.3).
struct SchedulerConfig
{
    std::map<Category, int> threads;
    /// Tareas simultáneas sobre un mismo dispositivo, contando todas las
    /// categorías. Ver Scheduler::submit con dispositivos.
    std::map<DeviceClass, int> deviceLimits;

    /// Valores predeterminados, proporcionales a los núcleos del equipo.
    static SchedulerConfig defaults();
};

/// El trabajo en sí. Recibe el token para comprobar si debe detenerse.
using TaskFunction = std::function<void(const CancelToken &)>;

/// Referencia a una tarea ya encolada: permite cancelarla y esperar a que acabe.
/// Se puede copiar; destruirla no cancela la tarea.
class TaskHandle
{
public:
    TaskHandle() = default;

    /// Pide la cancelación. Si la tarea aún no empezó, no llegará a ejecutarse.
    void cancel();

    /// true cuando la tarea terminó, fue cancelada antes de empezar o falló.
    bool isFinished() const;

    /// Espera a que termine. Devuelve false si se agotó el tiempo.
    bool waitFor(std::chrono::milliseconds timeout) const;

    /// true si la tarea lanzó una excepción (quedó registrada en el log).
    bool failed() const;

    bool isValid() const { return m_state != nullptr; }

private:
    friend class Scheduler;
    struct State;
    std::shared_ptr<State> m_state;
};

/// Planificador con un grupo de hilos acotado por categoría y cola por prioridad.
///
/// Garantías:
///  - dentro de una categoría, se atiende primero la prioridad más urgente y,
///    a igual prioridad, por orden de llegada;
///  - una tarea que declara sus dispositivos solo empieza cuando todos tienen
///    sitio; mientras espera, las de detrás que sí pueden empezar la adelantan;
///  - ninguna excepción sale de una tarea: se captura y se registra (SPEC §25.6);
///  - al destruirse cancela lo pendiente y espera a las tareas en curso.
class Scheduler
{
public:
    explicit Scheduler(const SchedulerConfig &config = SchedulerConfig::defaults());
    ~Scheduler();

    Scheduler(const Scheduler &) = delete;
    Scheduler &operator=(const Scheduler &) = delete;

    TaskHandle submit(Category category, Priority priority, TaskFunction function);

    /// Igual, declarando los dispositivos sobre los que va a leer o escribir
    /// (uno para listar una carpeta; origen y destino para copiar). Un
    /// dispositivo es un texto cualquiera que lo identifique; lo habitual es su
    /// punto de montaje (fs::DeviceMap).
    ///
    /// La concurrencia se adapta a cada dispositivo (SPEC §3.3): cada clase
    /// tiene un límite de tareas simultáneas (un disco mecánico, una). Una
    /// tarea solo espera a las de su nivel de urgencia o más urgentes:
    ///  - interactivas (UserInteraction, VisibleNavigation): solo compiten
    ///    entre sí y tienen dos sitios como mínimo, para que abrir una carpeta
    ///    no espere a una copia larga;
    ///  - trabajo pedido por el usuario (previsualización, transferencias,
    ///    miniaturas visibles): espera a las interactivas y a las de su nivel;
    ///  - de fondo (Background): espera a todo lo que haya en el dispositivo.
    /// No se interrumpe a una tarea que ya empezó.
    TaskHandle submit(Category category, Priority priority, const QStringList &devices, TaskFunction function);

    /// Dice de qué clase es un dispositivo. Los que no se declaran son Unknown.
    void setDeviceClass(const QString &device, DeviceClass deviceClass);
    /// Cambia el límite de una clase de dispositivo (mínimo 1). Vale para las
    /// tareas que empiecen a partir de ahora.
    void setDeviceLimit(DeviceClass deviceClass, int maxTasks);
    int deviceLimit(DeviceClass deviceClass) const;
    /// Tareas que están trabajando ahora mismo sobre un dispositivo.
    int runningOn(const QString &device) const;

    /// Cómo se averigua el dispositivo de una ruta (lo instala quien conoce
    /// las unidades: fs::DeviceMap). Sin esto, submitForPaths no limita nada.
    using DeviceResolver = std::function<QString(const QString &path)>;
    void setDeviceResolver(DeviceResolver resolver);
    /// Como submit con dispositivos, pero dando las rutas sobre las que se trabaja.
    TaskHandle submitForPaths(Category category, Priority priority, const QStringList &paths, TaskFunction function);

    /// Número de tareas en cola (sin contar las que ya se están ejecutando).
    int pendingCount(Category category) const;

private:
    class Pool;
    struct Devices;
    void wakeAllPools();

    std::unique_ptr<Devices> m_devices;
    std::shared_ptr<const DeviceResolver> m_resolver;   ///< Protegido por el cerrojo de m_devices.
    std::map<Category, std::unique_ptr<Pool>> m_pools;
};

} // namespace eu::tasks
