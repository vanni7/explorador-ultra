// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <memory>

namespace eu::tasks {

/// Permite a una tarea preguntar si le han pedido que se detenga.
///
/// La cancelación es *cooperativa*: nadie interrumpe el hilo a la fuerza (eso
/// podría dejar un archivo a medio escribir). La tarea consulta el token en sus
/// puntos seguros —entre archivo y archivo, entre bloque y bloque— y termina
/// ordenadamente. Copiar un token es barato: todas las copias ven la misma señal.
class CancelToken
{
public:
    CancelToken() : m_flag(std::make_shared<std::atomic<bool>>(false)) {}

    bool isCancelled() const { return m_flag->load(std::memory_order_relaxed); }

private:
    friend class CancelSource;
    std::shared_ptr<std::atomic<bool>> m_flag;
};

/// El lado que ordena la cancelación.
class CancelSource
{
public:
    void cancel() { m_token.m_flag->store(true, std::memory_order_relaxed); }
    const CancelToken &token() const { return m_token; }

private:
    CancelToken m_token;
};

} // namespace eu::tasks
