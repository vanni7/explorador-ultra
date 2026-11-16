// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/thumbs/Thumbnail.h"
#include "eu/thumbs/ThumbnailCache.h"
#include "eu/thumbs/ThumbnailService.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QGuiApplication>
#include <QImageReader>
#include <QPainter>
#include <QPdfWriter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cstring>

#ifdef EU_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
}
#endif

using namespace eu;
using namespace eu::thumbs;

namespace {

#ifdef EU_HAVE_FFMPEG
/// Escribe un video MJPEG (en contenedor MOV) hecho de tramos de colores
/// lisos: así se sabe qué fotograma debería salir en la miniatura.
/// `rotation`: el giro que anotaría un móvil (90 = grabado en vertical).
bool writeColorVideo(const QString &path, const QList<QPair<QColor, int>> &segments, int fps, QSize size,
                     int rotation = 0)
{
    AVFormatContext *out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, "mov", QFile::encodeName(path).constData()) < 0) {
        return false;
    }
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    AVStream *stream = avformat_new_stream(out, nullptr);
    AVCodecContext *codec = avcodec_alloc_context3(encoder);
    codec->width = size.width();
    codec->height = size.height();
    codec->time_base = AVRational{1, fps};
    codec->framerate = AVRational{fps, 1};
    codec->pix_fmt = AV_PIX_FMT_YUVJ420P;
    if (out->oformat->flags & AVFMT_GLOBALHEADER) {
        codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    bool ok = avcodec_open2(codec, encoder, nullptr) >= 0
        && avcodec_parameters_from_context(stream->codecpar, codec) >= 0;
    stream->time_base = codec->time_base;
    if (ok && rotation != 0) {
        AVPacketSideData *side = av_packet_side_data_new(&stream->codecpar->coded_side_data,
                                                         &stream->codecpar->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX, 9 * 4, 0);
        av_display_rotation_set(reinterpret_cast<int32_t *>(side->data), rotation);
    }
    ok = ok && avio_open(&out->pb, QFile::encodeName(path).constData(), AVIO_FLAG_WRITE) >= 0
        && avformat_write_header(out, nullptr) >= 0;
    AVFrame *frame = av_frame_alloc();
    frame->format = codec->pix_fmt;
    frame->width = codec->width;
    frame->height = codec->height;
    ok = ok && av_frame_get_buffer(frame, 0) >= 0;
    AVPacket *packet = av_packet_alloc();
    const auto drain = [&] {
        while (avcodec_receive_packet(codec, packet) >= 0) {
            av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
            packet->stream_index = stream->index;
            av_interleaved_write_frame(out, packet);
        }
    };
    int64_t pts = 0;
    for (const auto &[color, frames] : segments) {
        // De RGB a YUV (rango completo, como usa JPEG).
        const double r = color.red(), g = color.green(), b = color.blue();
        const auto y = uint8_t(std::clamp(0.299 * r + 0.587 * g + 0.114 * b, 0.0, 255.0));
        const auto u = uint8_t(std::clamp(128 - 0.168736 * r - 0.331264 * g + 0.5 * b, 0.0, 255.0));
        const auto v = uint8_t(std::clamp(128 + 0.5 * r - 0.418688 * g - 0.081312 * b, 0.0, 255.0));
        for (int i = 0; ok && i < frames; ++i) {
            av_frame_make_writable(frame);
            for (int row = 0; row < frame->height; ++row) {
                memset(frame->data[0] + row * frame->linesize[0], y, size_t(frame->width));
            }
            for (int row = 0; row < (frame->height + 1) / 2; ++row) {
                memset(frame->data[1] + row * frame->linesize[1], u, size_t((frame->width + 1) / 2));
                memset(frame->data[2] + row * frame->linesize[2], v, size_t((frame->width + 1) / 2));
            }
            frame->pts = pts++;
            ok = avcodec_send_frame(codec, frame) >= 0;
            drain();
        }
    }
    avcodec_send_frame(codec, nullptr);
    drain();
    if (ok) {
        av_write_trailer(out);
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codec);
    avio_closep(&out->pb);
    avformat_free_context(out);
    return ok;
}

/// Escribe un FLAC de silencio con `seconds` de duración y, si se da, una
/// carátula incrustada (como las de los discos).
bool writeFlac(const QString &path, double seconds, const QImage &cover)
{
    AVFormatContext *out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, "flac", QFile::encodeName(path).constData()) < 0) {
        return false;
    }
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    AVStream *audio = avformat_new_stream(out, nullptr);
    AVCodecContext *codec = avcodec_alloc_context3(encoder);
    codec->sample_fmt = AV_SAMPLE_FMT_S16;
    codec->sample_rate = 8000;
    av_channel_layout_default(&codec->ch_layout, 1);
    codec->time_base = AVRational{1, 8000};
    bool ok = avcodec_open2(codec, encoder, nullptr) >= 0
        && avcodec_parameters_from_context(audio->codecpar, codec) >= 0;
    audio->time_base = codec->time_base;
    QByteArray picture;
    AVStream *pictureStream = nullptr;
    if (!cover.isNull()) {
        QBuffer buffer(&picture);
        buffer.open(QIODevice::WriteOnly);
        cover.save(&buffer, "png");
        pictureStream = avformat_new_stream(out, nullptr);
        pictureStream->disposition = AV_DISPOSITION_ATTACHED_PIC;
        pictureStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        pictureStream->codecpar->codec_id = AV_CODEC_ID_PNG;
        pictureStream->codecpar->width = cover.width();
        pictureStream->codecpar->height = cover.height();
    }
    ok = ok && avio_open(&out->pb, QFile::encodeName(path).constData(), AVIO_FLAG_WRITE) >= 0
        && avformat_write_header(out, nullptr) >= 0;
    AVPacket *packet = av_packet_alloc();
    if (ok && pictureStream != nullptr) {
        // La carátula va antes que el audio.
        av_new_packet(packet, int(picture.size()));
        memcpy(packet->data, picture.constData(), size_t(picture.size()));
        packet->stream_index = pictureStream->index;
        packet->flags |= AV_PKT_FLAG_KEY;
        ok = av_interleaved_write_frame(out, packet) >= 0;
        av_packet_unref(packet);
    }
    AVFrame *frame = av_frame_alloc();
    frame->format = codec->sample_fmt;
    frame->nb_samples = codec->frame_size > 0 ? codec->frame_size : 1024;
    av_channel_layout_copy(&frame->ch_layout, &codec->ch_layout);
    frame->sample_rate = codec->sample_rate;
    ok = ok && av_frame_get_buffer(frame, 0) >= 0;
    const auto drain = [&] {
        while (avcodec_receive_packet(codec, packet) >= 0) {
            av_packet_rescale_ts(packet, codec->time_base, audio->time_base);
            packet->stream_index = audio->index;
            av_interleaved_write_frame(out, packet);
        }
    };
    const int64_t total = int64_t(seconds * codec->sample_rate);
    for (int64_t done = 0; ok && done < total; done += frame->nb_samples) {
        av_frame_make_writable(frame);
        memset(frame->data[0], 0, size_t(frame->nb_samples) * 2);
        frame->pts = done;
        ok = avcodec_send_frame(codec, frame) >= 0;
        drain();
    }
    avcodec_send_frame(codec, nullptr);
    drain();
    if (ok) {
        av_write_trailer(out);
    }
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codec);
    avio_closep(&out->pb);
    avformat_free_context(out);
    return ok;
}
#endif

/// Imagen de prueba: mitad izquierda roja y mitad derecha azul, para poder
/// saber hacia dónde se giró.
QImage testImage(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    QPainter painter(&image);
    painter.fillRect(0, 0, width / 2, height, Qt::red);
    painter.fillRect(width / 2, 0, width - width / 2, height, Qt::blue);
    return image;
}

QByteArray jpegBytes(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "jpg", 90);
    return bytes;
}

/// Inserta en un JPEG una etiqueta EXIF de orientación (como hacen los
/// móviles al hacer una foto en vertical: los píxeles van tumbados).
QByteArray withExifOrientation(const QByteArray &jpeg, quint16 orientation)
{
    QByteArray tiff;
    tiff += QByteArray("MM\0*", 4);                                   // big endian
    tiff += QByteArray("\0\0\0\x08", 4);                              // primer IFD en el byte 8
    tiff += QByteArray("\0\x01", 2);                                  // una entrada
    tiff += QByteArray("\x01\x12", 2);                                // etiqueta Orientation
    tiff += QByteArray("\0\x03", 2);                                  // tipo SHORT
    tiff += QByteArray("\0\0\0\x01", 4);                              // un valor
    tiff += char(orientation >> 8);
    tiff += char(orientation & 0xff);
    tiff += QByteArray("\0\0", 2);
    tiff += QByteArray("\0\0\0\0", 4);                                // no hay más IFD
    const QByteArray payload = QByteArray("Exif\0\0", 6) + tiff;
    const int length = int(payload.size()) + 2;
    QByteArray segment("\xff\xe1", 2);
    segment += char(length >> 8);
    segment += char(length & 0xff);
    segment += payload;
    return jpeg.left(2) + segment + jpeg.mid(2);                      // justo después de SOI
}

void writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(bytes);
}

ThumbnailRequest requestFor(const QString &path, int maxSide = 128)
{
    const QFileInfo info(path);
    ThumbnailRequest request;
    request.path = path;
    request.size = info.size();
    request.modifiedMs = info.lastModified().toMSecsSinceEpoch();
    request.maxSide = maxSide;
    return request;
}

bool isReddish(QRgb color)
{
    return qRed(color) > 180 && qBlue(color) < 80;
}

bool isBluish(QRgb color)
{
    return qBlue(color) > 180 && qRed(color) < 80;
}

} // namespace

class TestThumbs : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    tasks::Scheduler m_scheduler;

    QString file(const QString &name) const { return m_dir.filePath(name); }

private slots:
    void knowsWhichFilesHaveAThumbnail()
    {
        QCOMPARE(thumbnailSourceFor(QStringLiteral("foto.JPG")), ThumbnailSource::Image);
        QCOMPARE(thumbnailSourceFor(QStringLiteral("a.jpeg")), ThumbnailSource::Image);
        QCOMPARE(thumbnailSourceFor(QStringLiteral("dibujo.svg")), ThumbnailSource::Image);
        QCOMPARE(thumbnailSourceFor(QStringLiteral("x.png")), ThumbnailSource::Image);
        QCOMPARE(thumbnailSourceFor(QStringLiteral("copia.zip")), ThumbnailSource::None);
        QCOMPARE(thumbnailSourceFor(QStringLiteral("programa.exe")), ThumbnailSource::None);
        QCOMPARE(thumbnailSourceFor(QStringLiteral("sin-extension")), ThumbnailSource::None);
    }

    void keepsTheProportionAndNeverEnlarges()
    {
        QVERIFY(testImage(800, 400).save(file(QStringLiteral("ancha.png"))));
        auto thumbnail = renderThumbnail(requestFor(file(QStringLiteral("ancha.png")), 128), {});
        QVERIFY(thumbnail.ok());
        QCOMPARE(thumbnail.value().size(), QSize(128, 64));

        QVERIFY(testImage(40, 30).save(file(QStringLiteral("pequena.png"))));
        thumbnail = renderThumbnail(requestFor(file(QStringLiteral("pequena.png")), 128), {});
        QVERIFY(thumbnail.ok());
        QCOMPARE(thumbnail.value().size(), QSize(40, 30));   // no se agranda

        // Un JPEG grande se decodifica ya reducido y queda dentro del tamaño.
        writeBytes(file(QStringLiteral("grande.jpg")), jpegBytes(testImage(4000, 3000)));
        thumbnail = renderThumbnail(requestFor(file(QStringLiteral("grande.jpg")), 256), {});
        QVERIFY(thumbnail.ok());
        QVERIFY(thumbnail.value().width() <= 256 && thumbnail.value().height() <= 256);
        QVERIFY(thumbnail.value().width() >= 250);
    }

    void followsTheExifOrientation()
    {
        // Píxeles tumbados (ancha, rojo a la izquierda) con orientación 6: la
        // foto real es vertical, con el rojo arriba.
        writeBytes(file(QStringLiteral("vertical.jpg")),
                   withExifOrientation(jpegBytes(testImage(400, 200)), 6));
        const auto thumbnail = renderThumbnail(requestFor(file(QStringLiteral("vertical.jpg")), 128), {});
        QVERIFY(thumbnail.ok());
        const QImage image = thumbnail.value();
        QCOMPARE(image.size(), QSize(64, 128));
        QVERIFY(isReddish(image.pixel(32, 10)));
        QVERIFY(isBluish(image.pixel(32, 118)));
    }

    void rendersSvg()
    {
        writeBytes(file(QStringLiteral("dibujo.svg")),
                   "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
                   "<rect width='100' height='100' fill='red'/><rect x='100' width='100' height='100' fill='blue'/>"
                   "</svg>");
        const auto thumbnail = renderThumbnail(requestFor(file(QStringLiteral("dibujo.svg")), 64), {});
        QVERIFY2(thumbnail.ok(), thumbnail.ok() ? "" : qPrintable(thumbnail.error().message));
        QCOMPARE(thumbnail.value().size(), QSize(64, 32));
        QVERIFY(isReddish(thumbnail.value().pixel(10, 16)));
    }

    void brokenFilesFailWithoutCrashing()
    {
        writeBytes(file(QStringLiteral("rota.jpg")), QByteArray("\xff\xd8\xff\xe0" "basura", 10) + QByteArray(500, 'x'));
        auto thumbnail = renderThumbnail(requestFor(file(QStringLiteral("rota.jpg"))), {});
        QVERIFY(!thumbnail.ok());
        writeBytes(file(QStringLiteral("vacia.png")), QByteArray());
        thumbnail = renderThumbnail(requestFor(file(QStringLiteral("vacia.png"))), {});
        QVERIFY(!thumbnail.ok());
        // Un PNG que dice medir 100 000 × 100 000 se rechaza sin intentar decodificarlo.
        const QByteArray huge = QByteArray::fromHex("89504e470d0a1a0a0000000d49484452000186a0000186a00806000000a8520bc80000000b49444154789c63604005000010000139bd8f650000000049454e44ae426082");
        writeBytes(file(QStringLiteral("enorme.png")), huge);
        thumbnail = renderThumbnail(requestFor(file(QStringLiteral("enorme.png"))), {});
        QVERIFY(!thumbnail.ok());
        QCOMPARE(thumbnail.error().code, core::ErrorCode::NotSupported);   // el tope, no un fallo al leer
        // Una extensión equivocada no impide leerla.
        QVERIFY(testImage(50, 50).save(file(QStringLiteral("disfrazada.jpg")), "png"));
        QVERIFY(renderThumbnail(requestFor(file(QStringLiteral("disfrazada.jpg"))), {}).ok());
    }

    void pdfShowsTheFirstPage()
    {
        if (!QImageReader::supportedImageFormats().contains("pdf")) {
            QCOMPARE(thumbnailSourceFor(QStringLiteral("libro.pdf")), ThumbnailSource::None);
            QSKIP("Esta instalación de Qt no tiene el complemento de PDF.");
        }
        QCOMPARE(thumbnailSourceFor(QStringLiteral("libro.PDF")), ThumbnailSource::Pdf);
        const QString path = file(QStringLiteral("libro.pdf"));
        {
            QPdfWriter writer(path);
            writer.setPageSize(QPageSize(QPageSize::A4));
            QPainter painter(&writer);
            const QRect page = painter.viewport();
            painter.fillRect(QRect(0, 0, page.width(), page.height() / 2), Qt::red);   // mitad de arriba roja
            writer.newPage();
            painter.fillRect(page, Qt::blue);                                        // la segunda no se ve
        }
        const auto thumbnail = renderThumbnail(requestFor(path, 200), {});
        QVERIFY2(thumbnail.ok(), thumbnail.ok() ? "" : qPrintable(thumbnail.error().message));
        const QImage image = thumbnail.value();
        QCOMPARE(image.height(), 200);                 // A4 en vertical
        QVERIFY(image.width() > 130 && image.width() < 150);
        QVERIFY(isReddish(image.pixel(image.width() / 2, 40)));
        const QRgb bottom = image.pixel(image.width() / 2, 160);
        QVERIFY(qRed(bottom) > 240 && qGreen(bottom) > 240 && qBlue(bottom) > 240);   // papel blanco
    }

    void textShowsItsFirstLines()
    {
        QCOMPARE(thumbnailSourceFor(QStringLiteral("main.cpp")), ThumbnailSource::Text);
        QCOMPARE(thumbnailSourceFor(QStringLiteral("LEEME.md")), ThumbnailSource::Text);
        QString text;
        for (int i = 0; i < 60; ++i) {
            text += QStringLiteral("Línea %1 con texto de ejemplo, ñandú y acentos\n").arg(i);
        }
        writeBytes(file(QStringLiteral("notas.txt")), text.toUtf8());
        const auto thumbnail = renderThumbnail(requestFor(file(QStringLiteral("notas.txt")), 256), {});
        QVERIFY2(thumbnail.ok(), thumbnail.ok() ? "" : qPrintable(thumbnail.error().message));
        const QImage image = thumbnail.value();
        QCOMPARE(image.size(), QSize(192, 256));   // una hoja vertical
        // Hay tinta arriba a la izquierda (el texto) y la hoja es clara.
        int dark = 0;
        for (int y = 0; y < 60; ++y) {
            for (int x = 0; x < 120; ++x) {
                dark += qGray(image.pixel(x, y)) < 128 ? 1 : 0;
            }
        }
        QVERIFY(dark > 50);
        QVERIFY(qGray(image.pixel(190, 254)) > 200 || qGray(image.pixel(150, 250)) > 200);

        // Un binario con extensión de texto no se pinta como texto.
        writeBytes(file(QStringLiteral("falso.txt")), QByteArray("ab\0\0cd", 6) + QByteArray(100, '\0'));
        QVERIFY(!renderThumbnail(requestFor(file(QStringLiteral("falso.txt"))), {}).ok());
    }

    void foldersShowWhatTheyContain()
    {
        const QString photos = file(QStringLiteral("fotos"));
        QVERIFY(QDir().mkpath(photos));
        for (int i = 0; i < 5; ++i) {
            QVERIFY(testImage(120, 80).save(photos + QStringLiteral("/foto%1.png").arg(i)));
        }
        QVERIFY(QDir().mkpath(photos + QStringLiteral("/subcarpeta")));   // las subcarpetas no cuentan
        ThumbnailRequest request;
        request.path = photos;
        request.isDirectory = true;
        request.maxSide = 128;
        QCOMPARE(thumbnailSourceFor(request), ThumbnailSource::Folder);
        const auto grid = renderThumbnail(request, {});
        QVERIFY2(grid.ok(), grid.ok() ? "" : qPrintable(grid.error().message));
        QCOMPARE(grid.value().size(), QSize(128, 128));
        // Cuatro miniaturas en cuadrícula: hay color en las cuatro esquinas interiores.
        for (const QPoint point : {QPoint(20, 32), QPoint(84, 32), QPoint(20, 96), QPoint(84, 96)}) {
            QVERIFY2(qAlpha(grid.value().pixel(point)) > 0, qPrintable(QStringLiteral("%1,%2").arg(point.x()).arg(point.y())));
        }

        // Con un solo archivo, ocupa todo el hueco (no una esquina).
        const QString single = file(QStringLiteral("un-libro"));
        QVERIFY(QDir().mkpath(single));
        QVERIFY(testImage(100, 100).save(single + QStringLiteral("/portada.png")));
        request.path = single;
        const auto alone = renderThumbnail(request, {});
        QVERIFY(alone.ok());
        QVERIFY(qAlpha(alone.value().pixel(10, 64)) > 0 && qAlpha(alone.value().pixel(118, 64)) > 0);

        // Contenido mezclado, vacío o solo con subcarpetas: el icono de carpeta.
        const QString mixed = file(QStringLiteral("mezcla"));
        QVERIFY(QDir().mkpath(mixed));
        QVERIFY(testImage(50, 50).save(mixed + QStringLiteral("/a.png")));
        writeBytes(mixed + QStringLiteral("/b.zip"), "PK");
        request.path = mixed;
        QVERIFY(!renderThumbnail(request, {}).ok());
        const QString empty = file(QStringLiteral("vacia"));
        QVERIFY(QDir().mkpath(empty + QStringLiteral("/solo-subcarpeta")));
        request.path = empty;
        QVERIFY(!renderThumbnail(request, {}).ok());

        // En la caché, la carpeta y un archivo del mismo nombre no se confunden.
        ThumbnailRequest asFile = request;
        asFile.isDirectory = false;
        QVERIFY(ThumbnailCache::keyOf(request) != ThumbnailCache::keyOf(asFile));
    }

    void videoShowsTheFrameAtSecondThree()
    {
#ifndef EU_HAVE_FFMPEG
        QCOMPARE(thumbnailSourceFor(QStringLiteral("pelicula.mp4")), ThumbnailSource::None);
        QSKIP("Compilado sin FFmpeg: video y audio se ven con su icono.");
#else
        QVERIFY(hasMediaThumbnails());
        QCOMPARE(thumbnailSourceFor(QStringLiteral("pelicula.MP4")), ThumbnailSource::Video);
        QCOMPARE(thumbnailSourceFor(QStringLiteral("cancion.mp3")), ThumbnailSource::Audio);
        QCOMPARE(thumbnailSourceFor(QStringLiteral("app.ts")), ThumbnailSource::Text);   // TypeScript, no video

        // Dos segundos rojo y tres azul: en el segundo 3 se ve azul.
        const QString path = file(QStringLiteral("colores.mov"));
        QVERIFY(writeColorVideo(path, {{Qt::red, 20}, {Qt::blue, 30}}, 10, QSize(160, 120)));
        auto thumbnail = renderThumbnail(requestFor(path, 128), {});
        QVERIFY2(thumbnail.ok(), thumbnail.ok() ? "" : qPrintable(thumbnail.error().message));
        QCOMPARE(thumbnail.value().size(), QSize(128, 96));
        QVERIFY(isBluish(thumbnail.value().pixel(20, 20)));

        // Más corto que 3 s: un fotograma de la mitad.
        const QString shortPath = file(QStringLiteral("corto.mov"));
        QVERIFY(writeColorVideo(shortPath, {{Qt::red, 10}}, 10, QSize(160, 120)));
        thumbnail = renderThumbnail(requestFor(shortPath, 128), {});
        QVERIFY2(thumbnail.ok(), thumbnail.ok() ? "" : qPrintable(thumbnail.error().message));
        QVERIFY(isReddish(thumbnail.value().pixel(20, 20)));

        // Grabado en vertical con el móvil: la miniatura sale vertical.
        const QString rotated = file(QStringLiteral("vertical.mov"));
        QVERIFY(writeColorVideo(rotated, {{Qt::red, 40}}, 10, QSize(160, 120), 90));
        thumbnail = renderThumbnail(requestFor(rotated, 128), {});
        QVERIFY2(thumbnail.ok(), thumbnail.ok() ? "" : qPrintable(thumbnail.error().message));
        QCOMPARE(thumbnail.value().size(), QSize(96, 128));

        // Roto, vacío o cancelado: error, sin colgarse.
        writeBytes(file(QStringLiteral("roto.mp4")), QByteArray(4096, 'z'));
        QVERIFY(!renderThumbnail(requestFor(file(QStringLiteral("roto.mp4"))), {}).ok());
        writeBytes(file(QStringLiteral("vacio.mkv")), QByteArray());
        QVERIFY(!renderThumbnail(requestFor(file(QStringLiteral("vacio.mkv"))), {}).ok());
        tasks::CancelSource source;
        source.cancel();
        const auto cancelled = renderThumbnail(requestFor(path), source.token());
        QVERIFY(!cancelled.ok());
        QCOMPARE(cancelled.error().code, core::ErrorCode::Cancelled);
#endif
    }

    void audioShowsItsCover()
    {
#ifndef EU_HAVE_FFMPEG
        QSKIP("Compilado sin FFmpeg.");
#else
        QImage cover(300, 300, QImage::Format_RGB32);
        cover.fill(Qt::red);
        const QString withCover = file(QStringLiteral("disco.flac"));
        QVERIFY(writeFlac(withCover, 2.5, cover));
        auto thumbnail = renderThumbnail(requestFor(withCover, 128), {});
        QVERIFY2(thumbnail.ok(), thumbnail.ok() ? "" : qPrintable(thumbnail.error().message));
        QCOMPARE(thumbnail.value().size(), QSize(128, 128));
        QVERIFY(isReddish(thumbnail.value().pixel(10, 10)));
        // La duración va escrita abajo a la derecha, sobre una etiqueta oscura.
        const QRgb label = thumbnail.value().pixel(120, 122);
        QVERIFY2(qRed(label) < 150 || qGray(label) > 200, "debería haber una etiqueta con la duración");

        // Sin carátula, una genérica (no un error: así se ve la duración).
        const QString plain = file(QStringLiteral("sin-caratula.flac"));
        QVERIFY(writeFlac(plain, 1.0, QImage()));
        thumbnail = renderThumbnail(requestFor(plain, 128), {});
        QVERIFY2(thumbnail.ok(), thumbnail.ok() ? "" : qPrintable(thumbnail.error().message));
        QCOMPARE(thumbnail.value().size(), QSize(128, 128));
#endif
    }

    void cacheKeepsThumbnailsInMemoryAndOnDisk()
    {
        QTemporaryDir cacheDir;
        QVERIFY(testImage(300, 200).save(file(QStringLiteral("cache.png"))));
        const ThumbnailRequest request = requestFor(file(QStringLiteral("cache.png")));
        QImage image = renderThumbnail(request, {}).value();
        {
            ThumbnailCache cache(cacheDir.path());
            QCOMPARE(cache.lookupMemory(request).state, ThumbnailCache::State::Missing);
            cache.store(request, image);
            QCOMPARE(cache.lookupMemory(request).state, ThumbnailCache::State::Ready);
            QVERIFY(cache.memoryUsed() > 0);
        }
        // Otra caché sobre la misma carpeta (otra sesión) la encuentra en disco.
        ThumbnailCache cache(cacheDir.path());
        QCOMPARE(cache.lookupMemory(request).state, ThumbnailCache::State::Missing);
        const ThumbnailCache::Entry loaded = cache.load(request);
        QCOMPARE(loaded.state, ThumbnailCache::State::Ready);
        QCOMPARE(loaded.image.size(), image.size());
        QCOMPARE(cache.lookupMemory(request).state, ThumbnailCache::State::Ready);

        // Si el archivo cambia (otra fecha u otro tamaño), la vieja no vale.
        ThumbnailRequest changed = request;
        changed.modifiedMs += 1000;
        QCOMPARE(cache.load(changed).state, ThumbnailCache::State::Missing);
        changed = request;
        changed.maxSide = 256;
        QCOMPARE(cache.load(changed).state, ThumbnailCache::State::Missing);

        // Los fallos también se recuerdan.
        ThumbnailRequest broken = requestFor(file(QStringLiteral("rota.jpg")));
        cache.storeFailure(broken);
        ThumbnailCache another(cacheDir.path());
        QCOMPARE(another.load(broken).state, ThumbnailCache::State::Failed);

        // Recortar el disco borra lo que sobra.
        QVERIFY(cache.pruneDisk(0) >= 1);
        ThumbnailCache empty(cacheDir.path());
        QCOMPARE(empty.load(request).state, ThumbnailCache::State::Missing);
    }

    void memoryHasABudget()
    {
        ThumbnailCache cache(QString(), 1024 * 1024);   // 1 MB, solo memoria
        const QImage image = testImage(256, 256);        // 256 kB cada una
        for (int i = 0; i < 20; ++i) {
            ThumbnailRequest request;
            request.path = QStringLiteral("/x/%1.png").arg(i);
            cache.store(request, image);
        }
        QVERIFY(cache.memoryUsed() <= 1024 * 1024);
        ThumbnailRequest last;
        last.path = QStringLiteral("/x/19.png");
        QCOMPARE(cache.lookupMemory(last).state, ThumbnailCache::State::Ready);
        ThumbnailRequest first;
        first.path = QStringLiteral("/x/0.png");
        QCOMPARE(cache.lookupMemory(first).state, ThumbnailCache::State::Missing);
    }

    void serviceGeneratesOnceAndNotifies()
    {
        QTemporaryDir cacheDir;
        ThumbnailCache cache(cacheDir.path());
        ThumbnailService service(m_scheduler, cache);
        QSignalSpy ready(&service, &ThumbnailService::ready);

        QVERIFY(testImage(500, 500).save(file(QStringLiteral("servicio.png"))));
        const ThumbnailRequest request = requestFor(file(QStringLiteral("servicio.png")));
        QCOMPARE(service.request(request).state, ThumbnailCache::State::Missing);
        QCOMPARE(service.request(request).state, ThumbnailCache::State::Missing);   // no se pide dos veces
        QCOMPARE(service.pendingCount(), 1);
        QVERIFY(ready.wait(10000));
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.first().first().toString(), request.path);
        const ThumbnailCache::Entry entry = service.request(request);
        QCOMPARE(entry.state, ThumbnailCache::State::Ready);
        QCOMPARE(entry.image.size(), QSize(128, 128));

        // Un tipo sin miniatura no pide nada.
        ThumbnailRequest archive;
        archive.path = file(QStringLiteral("datos.zip"));
        QCOMPARE(service.request(archive).state, ThumbnailCache::State::Failed);
        QCOMPARE(service.pendingCount(), 0);

        // Un archivo roto avisa igual, y queda como fallido.
        ready.clear();
        const ThumbnailRequest broken = requestFor(file(QStringLiteral("rota.jpg")));
        service.request(broken);
        QVERIFY(ready.wait(10000));
        QCOMPARE(service.request(broken).state, ThumbnailCache::State::Failed);
    }

    void obsoleteRequestsAreCancelled()
    {
        // Un solo hilo de miniaturas ocupado: lo demás queda en cola.
        tasks::SchedulerConfig config;
        config.threads[tasks::Category::Thumbnails] = 1;
        tasks::Scheduler scheduler(config);
        std::atomic<bool> release{false};
        tasks::TaskHandle blocker = scheduler.submit(tasks::Category::Thumbnails, tasks::Priority::UserInteraction,
                                                     [&](const tasks::CancelToken &) {
            while (!release) {
                QThread::msleep(1);
            }
        });
        ThumbnailCache cache{QString()};
        ThumbnailService service(scheduler, cache);
        QSignalSpy ready(&service, &ThumbnailService::ready);
        QStringList paths;
        for (int i = 0; i < 5; ++i) {
            const QString path = file(QStringLiteral("cola%1.png").arg(i));
            QVERIFY(testImage(64, 64).save(path));
            paths.append(path);
            service.request(requestFor(path));
        }
        QCOMPARE(service.pendingCount(), 5);
        // Al desplazarse solo queda a la vista la última.
        service.retainOnly({paths.last()});
        QCOMPARE(service.pendingCount(), 1);
        release = true;
        QVERIFY(blocker.waitFor(std::chrono::seconds(5)));
        QVERIFY(ready.wait(10000));
        QTest::qWait(200);
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.first().first().toString(), paths.last());
        QCOMPARE(cache.lookupMemory(requestFor(paths.first())).state, ThumbnailCache::State::Missing);
    }
};

// Pintar texto en una imagen necesita las fuentes, y las fuentes una
// QGuiApplication (sin ventanas: la plataforma "offscreen" de las pruebas).
int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    TestThumbs test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "test_thumbs.moc"
