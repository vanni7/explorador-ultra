// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>

namespace eu::tasks {

/// Permite a una tarea preguntar si le han pedido que se detenga, y ceder el
/// disco cuando el usuario está esperando algo de él.
///
/// La cancelación es *cooperativa*: nadie interrumpe el hilo a la fuerza (eso
/// podría dejar un archivo a medio escribir). La tarea consulta el token en sus
/// puntos seguros —entre archivo y archivo, entre bloque y bloque— y termina
/// ordenadamente. Copiar un token es barato: todas las copias ven la misma señal.
class CancelToken
{
public:
    CancelToken() : m_state(std::make_shared<State>()) {}

    bool isCancelled() const { return m_state->cancelled.load(std::memory_order_relaxed); }

    /// Para tareas largas que leen o escriben mucho (copiar, indexar, medir
    /// carpetas): se llama en los mismos puntos seguros que isCancelled().
    ///
    /// Si en uno de los discos de la tarea está trabajando algo que el usuario
    /// espera —abrir una carpeta—, espera a que termine. Medido en este
    /// equipo: abrir carpetas mientras se lee un archivo grande tarda de 10 a
    /// 50 veces más en un disco mecánico y unas 18 veces más en un SSD SATA
    /// (docs/BENCHMARKS.md). No cede en las unidades en memoria (tmpfs).
    ///
    /// Nunca deja la tarea parada para siempre: tras `maxWait` seguidos
    /// cediendo, sigue trabajando un segundo sin ceder. Sale enseguida si se
    /// cancela. Fuera del planificador, o en tareas que no declararon disco,
    /// no hace nada.
    void yieldToInteractive(std::chrono::milliseconds maxWait = std::chrono::seconds(2)) const;

private:
    friend class CancelSource;

    struct State
    {
        std::atomic<bool> cancelled{false};
        std::mutex mutex;
        std::function<bool()> deviceBusy;   ///< La pone el planificador mientras corre la tarea.
        std::chrono::steady_clock::time_point freeUntil{};   ///< Hasta cuándo no se cede.
    };
    std::shared_ptr<State> m_state;
};

/// El lado que ordena la cancelación.
class CancelSource
{
public:
    void cancel() { m_token.m_state->cancelled.store(true, std::memory_order_relaxed); }
    const CancelToken &token() const { return m_token; }

    /// Instala (o quita, con una función vacía) la pregunta "¿hay alguien
    /// esperando el disco?" que usa CancelToken::yieldToInteractive().
    void setDeviceBusyCheck(std::function<bool()> check)
    {
        const std::lock_guard<std::mutex> lock(m_token.m_state->mutex);
        m_token.m_state->deviceBusy = std::move(check);
    }

private:
    CancelToken m_token;
};

} // namespace eu::tasks
