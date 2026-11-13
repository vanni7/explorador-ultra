// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/fs/Volumes.h"
#include "eu/tasks/Scheduler.h"

#include <QList>
#include <QString>

#include <memory>
#include <mutex>

namespace eu::fs {

/// La clase de dispositivo que entiende el planificador para un tipo de unidad.
tasks::DeviceClass deviceClassOf(DeviceKind kind);

/// Dice al planificador en qué unidad está cada ruta y de qué clase es cada
/// unidad, para que adapte la concurrencia (SPEC §3.3): un flujo por disco
/// mecánico, varios por SSD.
///
/// El dispositivo de una ruta es el punto de montaje de su unidad. Se puede
/// consultar desde cualquier hilo.
class DeviceMap
{
public:
    /// Se instala como traductor de rutas del planificador.
    explicit DeviceMap(tasks::Scheduler &scheduler);

    /// Lista de unidades vigente (al arrancar y cada vez que cambia).
    void setVolumes(const QList<Volume> &volumes);

    /// Punto de montaje de la unidad que contiene `path`, o vacío si ninguna.
    QString deviceFor(const QString &path) const;

private:
    struct State
    {
        mutable std::mutex mutex;
        QStringList roots;
    };

    tasks::Scheduler &m_scheduler;
    /// Compartido con el planificador, que puede vivir más que este objeto.
    std::shared_ptr<State> m_state;
};

} // namespace eu::fs
