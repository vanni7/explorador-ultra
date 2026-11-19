// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
//
// eu-thumbnailer: genera miniaturas en un proceso aparte (SPEC §7.1, §21).
//
// Decodificar un PDF, un video o una foto RAW dañados (o hechos a propósito
// para romper el decodificador) puede cerrar el programa que lo intenta. Así
// lo que se cierra es este proceso, no el explorador.
//
// Protocolo: una petición por línea en la entrada estándar, en JSON, y una
// respuesta por línea en la salida estándar.
//   Miniatura: {"path":"…","size":123,"mtime":456,"maxSide":256,"dir":false,"out":"…/x.png"}
//              -> {"ok":true}  o  {"ok":false,"code":3,"message":"…"}
//              La imagen se deja en "out" (PNG sin comprimir, que es lo más rápido).
//   Metadatos: {"op":"meta","path":"…"}
//              -> {"ok":true,"sections":[{"title":"Cámara","fields":[["ISO","200"],…]}]}
//   Texto:     {"op":"text","path":"…","out":"…"} -> el texto en UTF-8 en "out".
//   Documento: {"op":"preview","path":"…","out":"…"} -> la vista previa de un
//              documento de oficina (content::documentPreview) en JSON en "out".

#include "eu/content/DocumentPreview.h"
#include "eu/content/TextExtractor.h"
#include "eu/meta/Metadata.h"
#include "eu/thumbs/Thumbnail.h"

#include <QFile>
#include <QJsonArray>
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

// Con los sanitizadores (solo en pruebas) no se pone límite de memoria: ASan
// reserva una "memoria sombra" de varios TB que ya lo superaría al arrancar.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define EU_UNDER_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define EU_UNDER_SANITIZER 1
#endif
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
#if defined(Q_OS_UNIX) && !defined(EU_UNDER_SANITIZER)
    // Un archivo hecho para agotar la memoria no se lleva el equipo por
    // delante: este proceso no puede usar más de 3 GB. RLIMIT_DATA y no
    // RLIMIT_AS: el lector de PDF reserva mucho espacio de direcciones que no
    // llega a usar, y con RLIMIT_AS no podía abrir ningún PDF.
    struct rlimit limit;
    limit.rlim_cur = limit.rlim_max = rlim_t(3) * 1024 * 1024 * 1024;
    setrlimit(RLIMIT_DATA, &limit);
#endif
    QGuiApplication application(argc, argv);

    // Solo para las pruebas: provocar un cierre o un cuelgue con un archivo concreto.
    const QString crashOn = qEnvironmentVariable("EU_THUMBNAILER_CRASH_ON");
    const QString hangOn = qEnvironmentVariable("EU_THUMBNAILER_HANG_ON");

    std::string line;
    while (std::getline(std::cin, line)) {
        const QJsonObject request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const QString path = request.value(QStringLiteral("path")).toString();
        if (!crashOn.isEmpty() && path.contains(crashOn)) {
            std::abort();
        }
        if (!hangOn.isEmpty() && path.contains(hangOn)) {
            QThread::sleep(3600);
        }
        if (request.value(QStringLiteral("op")).toString() == QLatin1String("text")) {
            // Texto: {"op":"text","path":"…","out":"…"} -> el texto en UTF-8 en "out".
            const eu::core::Result<QString> text = eu::content::extractText(path, {});
            if (!text.ok()) {
                reply({{QStringLiteral("ok"), false}, {QStringLiteral("code"), int(text.error().code)},
                       {QStringLiteral("message"), text.error().message}});
                continue;
            }
            QSaveFile file(request.value(QStringLiteral("out")).toString());
            if (!file.open(QIODevice::WriteOnly) || file.write(text.value().toUtf8()) < 0 || !file.commit()) {
                reply({{QStringLiteral("ok"), false}, {QStringLiteral("code"), int(eu::core::ErrorCode::IoError)},
                       {QStringLiteral("message"), QStringLiteral("No se pudo escribir el texto")}});
                continue;
            }
            reply({{QStringLiteral("ok"), true}});
            continue;
        }
        if (request.value(QStringLiteral("op")).toString() == QLatin1String("preview")) {
            const eu::core::Result<eu::content::DocumentPreview> preview = eu::content::documentPreview(path, {});
            if (!preview.ok()) {
                reply({{QStringLiteral("ok"), false}, {QStringLiteral("code"), int(preview.error().code)},
                       {QStringLiteral("message"), preview.error().message}});
                continue;
            }
            QSaveFile file(request.value(QStringLiteral("out")).toString());
            if (!file.open(QIODevice::WriteOnly)
                || file.write(QJsonDocument(eu::content::documentPreviewToJson(preview.value()))
                                  .toJson(QJsonDocument::Compact)) < 0
                || !file.commit()) {
                reply({{QStringLiteral("ok"), false}, {QStringLiteral("code"), int(eu::core::ErrorCode::IoError)},
                       {QStringLiteral("message"), QStringLiteral("No se pudo escribir la vista previa")}});
                continue;
            }
            reply({{QStringLiteral("ok"), true}});
            continue;
        }
        if (request.value(QStringLiteral("op")).toString() == QLatin1String("meta")) {
            // Metadatos: {"op":"meta","path":"…"} -> {"ok":true,"sections":[{"title":…,"fields":[[l,v],…]}]}
            QJsonArray sections;
            for (const eu::meta::Section &section : eu::meta::readMetadata(path, {})) {
                QJsonArray fields;
                for (const eu::meta::Field &field : section.fields) {
                    fields.append(QJsonArray{field.label, field.value});
                }
                sections.append(QJsonObject{{QStringLiteral("title"), section.title},
                                            {QStringLiteral("fields"), fields}});
            }
            reply({{QStringLiteral("ok"), true}, {QStringLiteral("sections"), sections}});
            continue;
        }
        eu::thumbs::ThumbnailRequest thumbnail;
        thumbnail.path = request.value(QStringLiteral("path")).toString();
        thumbnail.size = qint64(request.value(QStringLiteral("size")).toDouble());
        thumbnail.modifiedMs = qint64(request.value(QStringLiteral("mtime")).toDouble());
        thumbnail.maxSide = request.value(QStringLiteral("maxSide")).toInt(256);
        thumbnail.isDirectory = request.value(QStringLiteral("dir")).toBool();
        const QString out = request.value(QStringLiteral("out")).toString();
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
