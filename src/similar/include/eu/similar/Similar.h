// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/tasks/Cancel.h"

#include <QImage>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace eu::search {
class PauseGate;
}

namespace eu::similar {

/// Huella perceptual de una imagen: dHash horizontal y vertical (128 bits).
/// Cambia poco si la imagen se recomprime, se redimensiona o se retoca un poco.
struct Fingerprint
{
    quint64 horizontal = 0;
    quint64 vertical = 0;
    bool valid = false;
};

Fingerprint fingerprint(const QImage &image);
/// Bits distintos entre dos huellas (0 = iguales a la vista, 128 = opuestas).
int distance(const Fingerprint &a, const Fingerprint &b);
/// Parecido en %, para mostrarlo: 100 - distancia × 100 / 128.
int similarityPercent(int distance);
/// Distancia máxima que corresponde a un parecido mínimo en %.
int distanceForPercent(int percent);

struct Image
{
    QString path;
    qint64 size = 0;
    qint64 modifiedMs = 0;
    int distance = 0;   ///< A la primera del grupo.
};

struct Group
{
    QList<Image> images;
    int maxDistance = 0;
};

struct Options
{
    QStringList roots;
    int maxDistance = 12;        ///< Parecido mínimo: ~90 %.
    bool includeHidden = false;
    /// Extensiones que se miran (en minúsculas).
    QStringList extensions{QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("png"),
                           QStringLiteral("webp"), QStringLiteral("bmp"),  QStringLiteral("gif"),
                           QStringLiteral("tif"),  QStringLiteral("tiff"), QStringLiteral("heic"),
                           QStringLiteral("heif"), QStringLiteral("cr2"),  QStringLiteral("cr3"),
                           QStringLiteral("nef"),  QStringLiteral("arw"),  QStringLiteral("dng"),
                           QStringLiteral("orf"),  QStringLiteral("rw2"),  QStringLiteral("raf")};
    const search::PauseGate *pause = nullptr;
};

/// Quién da la imagen (pequeña basta: 64 px) para calcular la huella. La
/// aplicación decodifica en el proceso auxiliar; vacío: Qt en este proceso.
using Decoder = std::function<QImage(const QString &path, const tasks::CancelToken &cancel)>;

struct Progress
{
    qint64 found = 0;     ///< Imágenes encontradas.
    qint64 done = 0;      ///< Con su huella calculada.
    bool comparing = false;
};

struct Result
{
    QList<Group> groups;  ///< De más a menos imágenes.
    qint64 images = 0;
    qint64 unreadable = 0;
};

/// Busca grupos de imágenes parecidas (SPEC §14.2): originales, versiones
/// comprimidas, redimensionadas o algo retocadas. Solo lee.
Result findSimilar(const Options &options, const Decoder &decoder, const tasks::CancelToken &cancel,
                   const std::function<void(const Progress &)> &onProgress = {});

/// La imagen pequeña para la huella, con Qt en este proceso (girada según su EXIF).
QImage decodeSmall(const QString &path);

} // namespace eu::similar
