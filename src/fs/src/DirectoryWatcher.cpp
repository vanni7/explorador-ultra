// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/fs/DirectoryWatcher.h"

namespace eu::fs {

DirectoryWatcher::DirectoryWatcher(QObject *parent)
    : QObject(parent)
{
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(150);
    connect(&m_flushTimer, &QTimer::timeout, this, &DirectoryWatcher::flushPending);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &DirectoryWatcher::onSystemNotification);
}

bool DirectoryWatcher::watch(const QString &directory)
{
    int &count = m_useCount[directory];
    if (count == 0 && !m_watcher.addPath(directory)) {
        m_useCount.remove(directory);
        return false;
    }
    ++count;
    return true;
}

void DirectoryWatcher::unwatch(const QString &directory)
{
    const auto it = m_useCount.find(directory);
    if (it == m_useCount.end()) {
        return;
    }
    if (--it.value() <= 0) {
        m_useCount.erase(it);
        m_watcher.removePath(directory);
        m_pending.removeAll(directory);
    }
}

void DirectoryWatcher::onSystemNotification(const QString &directory)
{
    if (!m_pending.contains(directory)) {
        m_pending.append(directory);
    }
    // No se reinicia el temporizador si ya corre: durante una copia larga los
    // avisos no paran y, si se reiniciara, la vista nunca se actualizaría.
    if (!m_flushTimer.isActive()) {
        m_flushTimer.start();
    }
}

void DirectoryWatcher::flushPending()
{
    const QStringList ready = m_pending;
    m_pending.clear();
    for (const QString &directory : ready) {
        if (m_useCount.contains(directory)) {
            emit directoryChanged(directory);
        }
    }
}

} // namespace eu::fs
