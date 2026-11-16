// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
//
// eu-thumbnailer: genera miniaturas en un proceso aparte (SPEC §7.1, §21).
//
// Decodificar un PDF, un video o una foto RAW dañados (o hechos a propósito
// para romper el decodificador) puede cerrar el programa que lo intenta. Así
// lo que se cierra es este proceso, no el explorador.
//
// Protocolo: una petición por línea en la entrada estándar, en JSON:
//   {"path":"…","size":123,"mtime":456,"maxSide":256,"dir":false,"out":"…/x.png"}
// y una respuesta por línea en la salida estándar:
//   {"ok":true}  o  {"ok":false,"code":3,"message":"…"}
// La imagen se deja en "out" (PNG sin comprimir, que es lo más rápido).

#include "eu/thumbs/Thumbnail.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

namespace {

void reply(const QJsonObject &object)
{
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char *argv[])
{
    // Sin ventanas: solo se pinta en imágenes (las fuentes sí hacen falta).
    qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef Q_OS_UNIX
    // Un archivo hecho para agotar la memoria no se lleva el equipo por
    // delante: este proceso no puede pasar de 3 GB.
    struct rlimit limit;
    limit.rlim_cur = limit.rlim_max = rlim_t(3) * 1024 * 1024 * 1024;
    setrlimit(RLIMIT_AS, &limit);
#endif
    QGuiApplication application(argc, argv);

    // Solo para las pruebas: provocar un cierre o un cuelgue con un archivo concreto.
    const QString crashOn = qEnvironmentVariable("EU_THUMBNAILER_CRASH_ON");
    const QString hangOn = qEnvironmentVariable("EU_THUMBNAILER_HANG_ON");

    std::string line;
    while (std::getline(std::cin, line)) {
        const QJsonObject request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        eu::thumbs::ThumbnailRequest thumbnail;
        thumbnail.path = request.value(QStringLiteral("path")).toString();
        thumbnail.size = qint64(request.value(QStringLiteral("size")).toDouble());
        thumbnail.modifiedMs = qint64(request.value(QStringLiteral("mtime")).toDouble());
        thumbnail.maxSide = request.value(QStringLiteral("maxSide")).toInt(256);
        thumbnail.isDirectory = request.value(QStringLiteral("dir")).toBool();
        const QString out = request.value(QStringLiteral("out")).toString();
        if (!crashOn.isEmpty() && thumbnail.path.contains(crashOn)) {
            std::abort();
        }
        if (!hangOn.isEmpty() && thumbnail.path.contains(hangOn)) {
            QThread::sleep(3600);
        }
        const eu::core::Result<QImage> image = eu::thumbs::renderThumbnail(thumbnail, {});
        if (!image.ok()) {
            reply({{QStringLiteral("ok"), false}, {QStringLiteral("code"), int(image.error().code)},
                   {QStringLiteral("message"), image.error().message}});
            continue;
        }
        QSaveFile file(out);
        if (!file.open(QIODevice::WriteOnly) || !image.value().save(&file, "png", 100) || !file.commit()) {
            reply({{QStringLiteral("ok"), false}, {QStringLiteral("code"), int(eu::core::ErrorCode::IoError)},
                   {QStringLiteral("message"), QStringLiteral("No se pudo escribir la miniatura")}});
            continue;
        }
        reply({{QStringLiteral("ok"), true}});
    }
    return 0;
}
