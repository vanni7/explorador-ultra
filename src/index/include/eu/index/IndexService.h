// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/index/Index.h"
#include "eu/index/TreeWatcher.h"
#include "eu/tasks/Scheduler.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <map>
#include <memory>
#include <optional>

namespace eu::index {

/// Cómo está el índice de una carpeta.
struct RootStatus
{
    enum class State {
        Building,       ///< Primer recorrido: todavía no sirve para buscar.
        Ready,          ///< Completo y al día (hasta donde llegan los avisos).
        Updating,       ///< Completo, y se está volviendo a recorrer.
        Disconnected,   ///< La unidad no está: se conserva lo anotado.
        Failed,         ///< No se pudo abrir o escribir la base.
    };

    QString root;
    State state = State::Building;
    IndexInfo info;
    SyncProgress progress;        ///< Avance del recorrido en curso.
    bool watching = false;        ///< El sistema avisa de los cambios.
    bool watchLimited = false;    ///< Faltan carpetas por vigilar (límite de inotify).
    qint64 watchedFolders = 0;
    QString error;
};

/// Mantiene los índices de las carpetas elegidas: los construye en segundo
/// plano, los pone al día al arrancar y sigue los cambios del disco.
///
/// Vive en el hilo de interfaz. El trabajo va al planificador, con prioridad
/// baja y de uno en uno, para no acaparar el disco ni estorbar a las búsquedas.
/// No guarda qué carpetas se indexan: eso lo decide y lo recuerda quien lo usa.
class IndexService : public QObject
{
    Q_OBJECT

public:
    /// `dataDirectory` es donde se guardan las bases (una por carpeta).
    IndexService(tasks::Scheduler &scheduler, QString dataDirectory, QObject *parent = nullptr);
    ~IndexService() override;

    /// Nombres que no se indexan (la carpeta de la papelera).
    void setExcludedNames(const QStringList &names) { m_excludedNames = names; }

    QStringList roots() const;
    QList<RootStatus> status() const;
    std::optional<RootStatus> statusOf(const QString &root) const;

    /// Empieza a indexar una carpeta. Si ya tenía índice de otra sesión, se
    /// puede buscar en él enseguida mientras se pone al día.
    void addRoot(const QString &root);
    /// Deja de indexarla y borra su base.
    void removeRoot(const QString &root);
    /// Vuelve a recorrerla entera ahora.
    void refresh(const QString &root);
    /// Borra lo anotado y empieza de cero.
    void rebuild(const QString &root);

    /// Cada cuánto se vuelve a recorrer una carpeta que el sistema no vigila
    /// entera (límite de inotify, o sistema sin vigilancia). 30 minutos.
    void setReviewIntervalMs(int milliseconds) { m_reviewTimer.setInterval(milliseconds); }

    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }
    /// true si hay un recorrido en marcha o esperando.
    bool isBusy() const;

    /// El índice completo que cubre `path`, si lo hay.
    std::optional<Index> indexFor(const QString &path) const;

    /// Archivo de la base de una carpeta.
    QString databasePathFor(const QString &root) const;

signals:
    /// Cambió el estado o el avance de algún índice.
    void changed();

private:
    struct Root;

    void schedule();
    void startNext();
    void startTask(Root &root);
    void onProgress(const QString &root, quint64 generation, const SyncProgress &progress);
    void onFinished(const QString &root, quint64 generation, const core::Status &status, const IndexInfo &info);
    void stopTask(Root &root);

    tasks::Scheduler &m_scheduler;
    QString m_dataDirectory;
    QStringList m_excludedNames;
    std::map<QString, std::unique_ptr<Root>> m_roots;
    QHash<QString, QString> m_failures;   ///< Carpetas cuya base no se pudo abrir, con el motivo.
    std::shared_ptr<search::PauseGate> m_gate;
    QTimer m_startTimer;   ///< Junta los avisos seguidos antes de lanzar el trabajo.
    QTimer m_reviewTimer;  ///< Repaso periódico de lo que el sistema no vigila.
    bool m_paused = false;
    bool m_running = false;   ///< Hay una tarea en marcha (solo una a la vez).
    quint64 m_generation = 0;
};

} // namespace eu::index
