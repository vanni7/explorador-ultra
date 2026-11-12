// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

namespace eu::fs {

/// Tipo de dispositivo, para decidir cuánta concurrencia admite (SPEC §3.3).
enum class DeviceKind {
    Unknown,
    Ssd,
    Hdd,       ///< Disco mecánico: un solo flujo secuencial.
    Removable, ///< USB y similares.
    Network,
    Optical,
    Virtual,   ///< tmpfs, imágenes montadas…
};

/// Una unidad montada y accesible.
struct Volume
{
    QString rootPath;      ///< Punto de montaje ("/", "/media/ana/USB", "C:/").
    QString displayName;   ///< Etiqueta, o un nombre razonable si no tiene.
    QString device;        ///< "/dev/sda1", "\\\\?\\Volume{…}"…
    QString fileSystem;    ///< "ext4", "ntfs", "exfat"…
    qint64 totalBytes = 0;
    qint64 freeBytes = 0;
    bool readOnly = false;
    DeviceKind kind = DeviceKind::Unknown;

    bool operator==(const Volume &other) const
    {
        return rootPath == other.rootPath && device == other.device && displayName == other.displayName
            && readOnly == other.readOnly;
    }
};

/// Unidades que tiene sentido mostrar al usuario (se descartan los sistemas de
/// archivos internos del sistema operativo como /proc, /sys o los snaps).
QList<Volume> listVolumes();

/// Volumen que contiene `path`, o un Volume con rootPath vacío si no se encuentra.
Volume volumeForPath(const QString &path);

/// true si las dos rutas están en el mismo volumen (mismo dispositivo).
/// Lo necesitan la papelera y el mover: dentro del mismo volumen basta renombrar.
bool isSameVolume(const QString &pathA, const QString &pathB);

/// Avisa cuando se conecta o desconecta una unidad (SPEC §5.3).
///
/// Comprueba la lista de montajes cada pocos segundos. Es una consulta barata
/// al sistema y funciona igual en las tres plataformas.
class VolumeMonitor : public QObject
{
    Q_OBJECT

public:
    explicit VolumeMonitor(QObject *parent = nullptr);

    QList<Volume> volumes() const { return m_volumes; }
    void setIntervalMs(int milliseconds);

public slots:
    /// Fuerza una comprobación inmediata.
    void refresh();

signals:
    void volumesChanged(const QList<eu::fs::Volume> &volumes);
    void volumeAdded(const eu::fs::Volume &volume);
    void volumeRemoved(const eu::fs::Volume &volume);

private:
    QList<Volume> m_volumes;
    QTimer m_timer;
};

} // namespace eu::fs
