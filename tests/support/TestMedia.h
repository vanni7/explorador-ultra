// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Archivos de prueba fabricados en la propia prueba (video, audio, ZIP), para
// no depender de muestras ni del programa ffmpeg.

#include <QBuffer>
#include <QColor>
#include <QFile>
#include <QImage>
#include <QList>
#include <QPair>
#include <QSize>
#include <QString>

#include <algorithm>
#include <cstring>

#ifdef EU_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
}
#endif
#ifdef EU_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

namespace eu::testsupport {

/// Escribe un WAV (PCM de 16 bits, mono, 8000 Hz) con un pitido de 440 Hz y
/// `seconds` de duración. No necesita FFmpeg: la cabecera se escribe a mano.
inline bool writeWav(const QString &path, double seconds)
{
    constexpr quint32 rate = 8000;
    const quint32 samples = quint32(seconds * rate);
    const quint32 dataBytes = samples * 2;
    QByteArray bytes;
    const auto put32 = [&bytes](quint32 value) {
        for (int shift = 0; shift < 32; shift += 8) {
            bytes.append(char((value >> shift) & 0xff));
        }
    };
    const auto put16 = [&bytes](quint16 value) {
        bytes.append(char(value & 0xff));
        bytes.append(char((value >> 8) & 0xff));
    };
    bytes.append("RIFF");
    put32(36 + dataBytes);
    bytes.append("WAVEfmt ");
    put32(16);           // tamaño del bloque fmt
    put16(1);            // PCM
    put16(1);            // un canal
    put32(rate);
    put32(rate * 2);     // bytes por segundo
    put16(2);            // bytes por muestra
    put16(16);           // bits por muestra
    bytes.append("data");
    put32(dataBytes);
    for (quint32 i = 0; i < samples; ++i) {
        // Onda cuadrada de 440 Hz, flojita: sirve igual y no necesita <cmath>.
        const bool high = (i * 440 * 2 / rate) % 2 == 0;
        put16(quint16(qint16(high ? 3000 : -3000)));
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

#ifdef EU_HAVE_LIBARCHIVE
/// Escribe un ZIP con estas entradas (los documentos de oficina lo son).
inline bool writeZip(const QString &path, const QList<QPair<QString, QByteArray>> &entries)
{
    struct archive *zip = archive_write_new();
    archive_write_set_format_zip(zip);
    bool ok = archive_write_open_filename(zip, QFile::encodeName(path).constData()) == ARCHIVE_OK;
    for (const auto &[name, data] : entries) {
        struct archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.toUtf8().constData());
        archive_entry_set_size(entry, data.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        ok = ok && archive_write_header(zip, entry) == ARCHIVE_OK
            && archive_write_data(zip, data.constData(), size_t(data.size())) == data.size();
        archive_entry_free(entry);
    }
    ok = archive_write_close(zip) == ARCHIVE_OK && ok;
    archive_write_free(zip);
    return ok;
}
#endif

#ifdef EU_HAVE_FFMPEG
/// Escribe un video MJPEG (en contenedor MOV) hecho de tramos de colores
/// lisos: así se sabe qué fotograma debería salir en la miniatura.
/// `rotation`: el giro que anotaría un móvil (90 = grabado en vertical).
inline bool writeColorVideo(const QString &path, const QList<QPair<QColor, int>> &segments, int fps, QSize size,
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
inline bool writeFlac(const QString &path, double seconds, const QImage &cover,
                      const QList<QPair<QByteArray, QByteArray>> &tags = {})
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
    for (const auto &[key, value] : tags) {
        av_dict_set(&out->metadata, key.constData(), value.constData(), 0);
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

} // namespace eu::testsupport
