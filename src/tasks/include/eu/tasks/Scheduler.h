// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/tasks/Cancel.h"

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

/// Cuántos hilos tiene cada categoría. Todo es configurable (SPEC §3.3).
struct SchedulerConfig
{
    std::map<Category, int> threads;

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

    /// Número de tareas en cola (sin contar las que ya se están ejecutando).
    int pendingCount(Category category) const;

private:
    class Pool;
    std::map<Category, std::unique_ptr<Pool>> m_pools;
};

} // namespace eu::tasks
