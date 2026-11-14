// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/fs/DeviceMap.h"

#include <QDir>

namespace eu::fs {

namespace {

QString longestRootContaining(const QStringList &roots, const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    QString best;
    for (const QString &root : roots) {
        const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
        if ((clean == root || clean.startsWith(prefix)) && root.size() > best.size()) {
            best = root;
        }
    }
    return best;
}

} // namespace

tasks::DeviceClass deviceClassOf(DeviceKind kind)
{
    switch (kind) {
    case DeviceKind::Ssd:       return tasks::DeviceClass::Ssd;
    case DeviceKind::Hdd:       return tasks::DeviceClass::Hdd;
    case DeviceKind::Removable: return tasks::DeviceClass::Removable;
    case DeviceKind::Network:   return tasks::DeviceClass::Network;
    case DeviceKind::Optical:   return tasks::DeviceClass::Optical;
    case DeviceKind::Virtual:   return tasks::DeviceClass::Virtual;
    case DeviceKind::Unknown:   break;
    }
    return tasks::DeviceClass::Unknown;
}

DeviceMap::DeviceMap(tasks::Scheduler &scheduler) : m_scheduler(scheduler), m_state(std::make_shared<State>())
{
    m_scheduler.setDeviceResolver([state = m_state](const QString &path) {
        const std::lock_guard<std::mutex> lock(state->mutex);
        return longestRootContaining(state->roots, path);
    });
}

void DeviceMap::setVolumes(const QList<Volume> &volumes)
{
    QStringList roots;
    for (const Volume &volume : volumes) {
        const QString root = QDir::cleanPath(volume.rootPath);
        roots.append(root);
        m_scheduler.setDeviceClass(root, deviceClassOf(volume.kind));
    }
    const std::lock_guard<std::mutex> lock(m_state->mutex);
    m_state->roots = roots;
}

QString DeviceMap::deviceFor(const QString &path) const
{
    const std::lock_guard<std::mutex> lock(m_state->mutex);
    return longestRootContaining(m_state->roots, path);
}

} // namespace eu::fs
