// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "MediaThumbnail.h"

#include <QCoreApplication>
#include <QFile>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>

#ifdef EU_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libswscale/swscale.h>
}
#endif

#include <algorithm>
#include <memory>
#include <mutex>

namespace eu::thumbs::detail {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::thumbs", text);
}

QString durationText(qint64 milliseconds)
{
    const qint64 seconds = (milliseconds + 500) / 1000;
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds / 60) % 60;
    const qint64 rest = seconds % 60;
    return hours > 0 ? QStringLiteral("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QLatin1Char('0')).arg(rest, 2, 10, QLatin1Char('0'))
                     : QStringLiteral("%1:%2").arg(minutes).arg(rest, 2, 10, QLatin1Char('0'));
}

/// La duración, en blanco sobre una etiqueta oscura en la esquina inferior derecha.
void drawDuration(QImage &image, qint64 milliseconds)
{
    if (milliseconds <= 0 || image.isNull()) {
        return;
    }
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont font;
    font.setPixelSize(std::max(9, std::min(image.width(), image.height()) / 9));
    font.setBold(true);
    painter.setFont(font);
    const QString text = durationText(milliseconds);
    const QFontMetrics metrics(font);
    const int padding = std::max(2, font.pixelSize() / 4);
    const QRect box(image.width() - metrics.horizontalAdvance(text) - 3 * padding,
                    image.height() - metrics.height() - 3 * padding, metrics.horizontalAdvance(text) + 2 * padding,
                    metrics.height() + 2 * padding);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 170));
    painter.drawRoundedRect(box, padding, padding);
    painter.setPen(Qt::white);
    painter.drawText(box, Qt::AlignCenter, text);
}

/// Carátula genérica para el audio sin imagen propia: una nota sobre un degradado.
QImage genericAudioCover(int side)
{
    QImage image(side, side, QImage::Format_RGB32);
    QPainter painter(&image);
    QLinearGradient gradient(0, 0, side, side);
    gradient.setColorAt(0, QColor(0x5b, 0x3f, 0x8c));
    gradient.setColorAt(1, QColor(0x2a, 0x6f, 0x97));
    painter.fillRect(image.rect(), gradient);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 220));
    // Una corchea dibujada a mano: no depende de que la fuente tenga el signo.
    const qreal unit = side / 10.0;
    painter.drawEllipse(QPointF(4 * unit, 6.6 * unit), 1.3 * unit, 1.0 * unit);
    painter.drawRect(QRectF(5.05 * unit, 2.4 * unit, 0.45 * unit, 4.2 * unit));
    QPainterPath flag;
    flag.moveTo(5.5 * unit, 2.4 * unit);
    flag.cubicTo(6.6 * unit, 3.0 * unit, 7.2 * unit, 3.6 * unit, 6.8 * unit, 4.9 * unit);
    flag.cubicTo(6.6 * unit, 4.0 * unit, 6.1 * unit, 3.6 * unit, 5.5 * unit, 3.4 * unit);
    painter.drawPath(flag);
    return image;
}

} // namespace

#ifndef EU_HAVE_FFMPEG

bool hasMediaSupport()
{
    return false;
}

core::Result<QImage> renderMedia(const ThumbnailRequest &request, const tasks::CancelToken &, bool)
{
    return core::Error{core::ErrorCode::NotSupported, tr("Esta compilación no lee video ni audio"), request.path, 0};
}

#else

namespace {

struct FormatDeleter
{
    void operator()(AVFormatContext *context) const { avformat_close_input(&context); }
};
struct CodecDeleter
{
    void operator()(AVCodecContext *context) const { avcodec_free_context(&context); }
};
struct FrameDeleter
{
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
struct PacketDeleter
{
    void operator()(AVPacket *packet) const { av_packet_free(&packet); }
};
struct ScaleDeleter
{
    void operator()(SwsContext *context) const { sws_freeContext(context); }
};

/// FFmpeg pregunta esto mientras lee: así cancelar no espera a una unidad lenta.
int interruptRequested(void *opaque)
{
    return static_cast<const tasks::CancelToken *>(opaque)->isCancelled() ? 1 : 0;
}

core::Error mediaError(const QString &path, const QString &what)
{
    return core::Error{core::ErrorCode::Corrupted, what, path, 0};
}

/// Convierte un fotograma decodificado en una imagen que quepa en `maxSide`,
/// respetando la proporción de píxel del video.
QImage toImage(const AVFrame *frame, int maxSide)
{
    if (frame->width <= 0 || frame->height <= 0) {
        return {};
    }
    double width = frame->width;
    if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0) {
        width = width * frame->sample_aspect_ratio.num / frame->sample_aspect_ratio.den;
    }
    QSize size(int(width + 0.5), frame->height);
    size.scale(std::min(maxSide, size.width()), std::min(maxSide, size.height()), Qt::KeepAspectRatio);
    size = size.expandedTo(QSize(1, 1));
    std::unique_ptr<SwsContext, ScaleDeleter> scaler(
        sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format), size.width(), size.height(),
                       AV_PIX_FMT_RGB32, SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!scaler) {
        return {};
    }
    // AV_PIX_FMT_RGB32 guarda los píxeles como QImage::Format_RGB32.
    QImage image(size, QImage::Format_RGB32);
    uint8_t *destination[4] = {image.bits(), nullptr, nullptr, nullptr};
    int strides[4] = {int(image.bytesPerLine()), 0, 0, 0};
    sws_scale(scaler.get(), frame->data, frame->linesize, 0, frame->height, destination, strides);
    return image;
}

/// Giro que el móvil anotó al grabar (los videos verticales van tumbados).
double rotationOf(const AVStream *stream)
{
    const AVPacketSideData *data = av_packet_side_data_get(stream->codecpar->coded_side_data,
                                                           stream->codecpar->nb_coded_side_data,
                                                           AV_PKT_DATA_DISPLAYMATRIX);
    if (data == nullptr || data->size < 9 * 4) {
        return 0;
    }
    return av_display_rotation_get(reinterpret_cast<const int32_t *>(data->data));
}

QImage fitted(const QImage &image, int maxSide)
{
    if (image.width() <= maxSide && image.height() <= maxSide) {
        return image.convertToFormat(QImage::Format_RGB32);
    }
    return image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_RGB32);
}

} // namespace

bool hasMediaSupport()
{
    return true;
}

core::Result<QImage> renderMedia(const ThumbnailRequest &request, const tasks::CancelToken &cancel, bool audio)
{
    // FFmpeg avisa por la consola de cada archivo raro; aquí un archivo raro
    // es normal (se devuelve un error y se ve el icono), así que se calla.
    static std::once_flag quiet;
    std::call_once(quiet, [] { av_log_set_level(AV_LOG_FATAL); });
    const int maxSide = std::max(16, request.maxSide);
    AVFormatContext *raw = avformat_alloc_context();
    if (raw == nullptr) {
        return mediaError(request.path, tr("Sin memoria"));
    }
    raw->interrupt_callback.callback = &interruptRequested;
    raw->interrupt_callback.opaque = const_cast<tasks::CancelToken *>(&cancel);
    // Si falla, avformat_open_input libera el contexto.
    if (avformat_open_input(&raw, QFile::encodeName(request.path).constData(), nullptr, nullptr) < 0) {
        return cancel.isCancelled() ? core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), request.path, 0}
                                    : mediaError(request.path, tr("No se pudo abrir el archivo multimedia"));
    }
    std::unique_ptr<AVFormatContext, FormatDeleter> format(raw);
    if (avformat_find_stream_info(format.get(), nullptr) < 0) {
        return mediaError(request.path, tr("No se entiende el archivo multimedia"));
    }
    const qint64 durationMs = format->duration != AV_NOPTS_VALUE ? format->duration / (AV_TIME_BASE / 1000) : 0;

    // Carátula incrustada (audio, y algunos videos).
    QImage cover;
    for (unsigned i = 0; i < format->nb_streams && cover.isNull(); ++i) {
        const AVStream *stream = format->streams[i];
        if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) && stream->attached_pic.size > 0) {
            cover = QImage::fromData(stream->attached_pic.data, stream->attached_pic.size);
        }
    }
    if (audio) {
        QImage image = cover.isNull() ? genericAudioCover(maxSide) : fitted(cover, maxSide);
        drawDuration(image, durationMs);
        return image;
    }

    const AVCodec *decoder = nullptr;
    const int streamIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (streamIndex < 0 || decoder == nullptr
        || (format->streams[streamIndex]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        if (!cover.isNull()) {
            QImage image = fitted(cover, maxSide);
            drawDuration(image, durationMs);
            return image;
        }
        return mediaError(request.path, tr("El archivo no tiene video"));
    }
    AVStream *stream = format->streams[streamIndex];
    std::unique_ptr<AVCodecContext, CodecDeleter> codec(avcodec_alloc_context3(decoder));
    if (!codec || avcodec_parameters_to_context(codec.get(), stream->codecpar) < 0) {
        return mediaError(request.path, tr("No se pudo preparar el decodificador"));
    }
    codec->thread_count = 1;   // ya corre en un hilo del planificador
    if (avcodec_open2(codec.get(), decoder, nullptr) < 0) {
        return mediaError(request.path, tr("No hay decodificador para este video"));
    }

    // El segundo 3 (SPEC §5.5), o la mitad si el video dura menos.
    qint64 targetMs = 3000;
    if (durationMs > 0 && durationMs <= 3000) {
        targetMs = durationMs / 2;
    }
    const int64_t startTime = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    const int64_t target = av_rescale_q(targetMs, AVRational{1, 1000}, stream->time_base) + startTime;
    if (av_seek_frame(format.get(), streamIndex, target, AVSEEK_FLAG_BACKWARD) >= 0) {
        avcodec_flush_buffers(codec.get());
    }

    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> chosen(av_frame_alloc());
    bool haveFrame = false;
    bool reachedTarget = false;
    int packetsRead = 0;
    bool draining = false;
    // Un video dañado no debe tener la tarea leyendo para siempre.
    while (!reachedTarget && packetsRead < 3000) {
        if (cancel.isCancelled()) {
            return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), request.path, 0};
        }
        if (!draining) {
            const int read = av_read_frame(format.get(), packet.get());
            if (read < 0) {
                draining = true;
                avcodec_send_packet(codec.get(), nullptr);   // pide lo que quede dentro
            } else {
                ++packetsRead;
                if (packet->stream_index == streamIndex) {
                    avcodec_send_packet(codec.get(), packet.get());
                }
                av_packet_unref(packet.get());
            }
        }
        while (true) {
            const int received = avcodec_receive_frame(codec.get(), frame.get());
            if (received < 0) {
                break;
            }
            av_frame_unref(chosen.get());
            av_frame_move_ref(chosen.get(), frame.get());
            haveFrame = true;
            const int64_t timestamp = chosen->best_effort_timestamp;
            if (timestamp == AV_NOPTS_VALUE || timestamp >= target) {
                reachedTarget = true;
                break;
            }
        }
        if (draining && !reachedTarget) {
            break;   // se acabó el video antes: vale el último fotograma
        }
    }
    if (!haveFrame) {
        return mediaError(request.path, tr("No se pudo decodificar ningún fotograma"));
    }
    QImage image = toImage(chosen.get(), maxSide);
    if (image.isNull()) {
        return mediaError(request.path, tr("No se pudo convertir el fotograma"));
    }
    const double rotation = rotationOf(stream);
    if (std::abs(rotation) > 1) {
        // av_display_rotation_get da el giro en sentido antihorario.
        image = image.transformed(QTransform().rotate(-rotation));
        if (image.width() > maxSide || image.height() > maxSide) {
            image = image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }
    drawDuration(image, durationMs);
    return image;
}

#endif

} // namespace eu::thumbs::detail
