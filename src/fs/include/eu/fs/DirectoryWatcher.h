// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QTimer>

namespace eu::fs {

/// Avisa cuando cambia el contenido de las carpetas que se están mostrando.
///
/// Se apoya en QFileSystemWatcher (inotify en Linux, ReadDirectoryChangesW en
/// Windows, FSEvents/kqueue en macOS). Solo vigila las carpetas abiertas en
/// pantalla —unas pocas—, así que no choca con los límites de inotify; la
/// vigilancia de discos enteros para el índice es otra pieza (Fase 2).
///
/// Los avisos se agrupan: una copia de mil archivos produce un solo aviso por
/// carpeta cada `quietPeriodMs`, no mil.
class DirectoryWatcher : public QObject
{
    Q_OBJECT

public:
    explicit DirectoryWatcher(QObject *parent = nullptr);

    /// Empieza a vigilar la carpeta. Se puede pedir varias veces (una por
    /// pestaña); deja de vigilarse cuando se suelta el mismo número de veces.
    /// Devuelve false si el sistema no permitió vigilarla.
    bool watch(const QString &directory);
    void unwatch(const QString &directory);

    void setQuietPeriodMs(int milliseconds) { m_flushTimer.setInterval(milliseconds); }

signals:
    /// El contenido de la carpeta cambió: conviene volver a listarla.
    void directoryChanged(const QString &directory);

private:
    void onSystemNotification(const QString &directory);
    void flushPending();

    QFileSystemWatcher m_watcher;
    QHash<QString, int> m_useCount;
    QStringList m_pending;
    QTimer m_flushTimer;
};

} // namespace eu::fs
