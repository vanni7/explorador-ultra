// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/index/ContentIndex.h"
#include "eu/tasks/Scheduler.h"

#include <QHash>
#include <QObject>
#include <QTimer>

#include <map>
#include <memory>

namespace eu::index {

/// Cómo está el índice de contenido de una carpeta.
struct ContentRootStatus
{
    enum class State {
        Building,       ///< Primer recorrido (ya se puede buscar: lo que falta se lee del disco).
        Ready,          ///< Al día hasta el último repaso.
        Updating,       ///< Repasando qué cambió.
        Waiting,        ///< Esperando turno.
        Disconnected,   ///< La carpeta no está: se conserva lo anotado.
        Failed,
    };

    QString root;
    State state = State::Waiting;
    ContentIndexInfo info;
    ContentSyncProgress progress;
    QString error;
};

/// Mantiene los índices de contenido de las carpetas elegidas (SPEC §6.5): los
/// crea en segundo plano, los repasa al arrancar y cada 30 minutos.
///
/// Vive en el hilo de interfaz; el trabajo va al planificador, de fondo y de
/// uno en uno. No guarda qué carpetas se indexan: eso lo hace quien lo usa.
class ContentIndexService : public QObject
{
    Q_OBJECT

public:
    ContentIndexService(tasks::Scheduler &scheduler, QString dataDirectory, QObject *parent = nullptr);
    ~ContentIndexService() override;

    /// Quién saca el texto (la interfaz pone el del proceso auxiliar).
    void setTextProvider(TextProvider provider) { m_textProvider = std::move(provider); }
    void setExcludedNames(const QStringList &names) { m_excludedNames = names; }

    QStringList roots() const;
    QList<ContentRootStatus> status() const;

    void addRoot(const QString &root);
    /// Deja de indexarla y borra su base.
    void removeRoot(const QString &root);
    /// La repasa ahora.
    void refresh(const QString &root);
    /// Borra lo anotado y la lee entera de nuevo (también reintenta los que fallaron).
    void rebuild(const QString &root);

    void setReviewIntervalMs(int milliseconds) { m_reviewTimer.setInterval(milliseconds); }
    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }
    bool isBusy() const;

    /// Para una búsqueda en `searchRoots`: crea (en el hilo de la búsqueda) la
    /// consulta a los índices que tienen algo que ver. Nulo si no hay ninguno.
    std::function<std::unique_ptr<search::ContentCache>()> cacheFactory(const QStringList &searchRoots) const;

    QString databasePathFor(const QString &root) const;

signals:
    void changed();

private:
    struct Root;

    void schedule();
    void startNext();
    void onProgress(const QString &root, quint64 generation, const ContentSyncProgress &progress);
    void onFinished(const QString &root, quint64 generation, const core::Status &status, const ContentIndexInfo &info);
    void stopTask(Root &root);

    tasks::Scheduler &m_scheduler;
    QString m_dataDirectory;
    TextProvider m_textProvider;
    QStringList m_excludedNames;
    std::map<QString, std::unique_ptr<Root>> m_roots;
    QHash<QString, QString> m_failures;
    std::shared_ptr<search::PauseGate> m_gate;
    QTimer m_startTimer;
    QTimer m_reviewTimer;
    bool m_paused = false;
    bool m_running = false;
};

} // namespace eu::index
