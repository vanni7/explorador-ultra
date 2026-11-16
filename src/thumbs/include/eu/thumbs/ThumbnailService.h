// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/tasks/Scheduler.h"
#include "eu/thumbs/ThumbnailCache.h"

#include <QHash>
#include <QObject>
#include <QSet>

namespace eu::thumbs {

/// Pide miniaturas en segundo plano y avisa cuando están (SPEC §5.5).
///
/// Vive en el hilo de interfaz. Cada miniatura se genera una sola vez aunque
/// se pida muchas; las peticiones de lo que ya no está a la vista se
/// cancelan con retainOnly() (desplazarse deprisa no deja trabajo atrasado).
class ThumbnailService : public QObject
{
    Q_OBJECT

public:
    ThumbnailService(tasks::Scheduler &scheduler, ThumbnailCache &cache, QObject *parent = nullptr);
    ~ThumbnailService() override;

    /// Si está en memoria la devuelve ya. Si no, la pide (salvo que ya esté
    /// pedida) y avisará con ready(). Para los tipos sin miniatura devuelve
    /// Failed sin pedir nada.
    ThumbnailCache::Entry request(const ThumbnailRequest &request);

    /// Cancela lo pedido cuyas rutas no estén en `paths` y aún no haya empezado.
    void retainOnly(const QSet<QString> &paths);
    /// Cancela todo lo pendiente (al cambiar de carpeta).
    void cancelAll();

    int pendingCount() const { return int(m_pending.size()); }
    ThumbnailCache &cache() { return m_cache; }

signals:
    /// La miniatura de `path` ya está en la caché (o falló: entonces el
    /// estado es Failed). Quien la pidió vuelve a llamar a request().
    void ready(const QString &path);

private:
    struct Pending
    {
        QString path;
        tasks::TaskHandle task;
        quint64 id = 0;   ///< Distingue una petición repetida de otra anterior ya cancelada.
    };
    void onFinished(const QString &key, quint64 id);

    tasks::Scheduler &m_scheduler;
    ThumbnailCache &m_cache;
    QHash<QString, Pending> m_pending;   ///< Por clave de la caché.
    quint64 m_lastId = 0;
};

} // namespace eu::thumbs
