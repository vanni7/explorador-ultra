// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/meta/Metadata.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QSet>

#ifdef EU_HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
}
#endif
#ifdef EU_HAVE_EXIV2
#include <exiv2/exiv2.hpp>
#endif

#include <memory>
#include <mutex>

namespace eu::meta {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::meta", text);
}

void add(Section &section, const QString &label, const QString &value)
{
    const QString trimmed = value.trimmed();
    if (!trimmed.isEmpty()) {
        section.fields.append({label, trimmed});
    }
}

void addSection(QList<Section> &sections, Section section)
{
    if (!section.fields.isEmpty()) {
        sections.append(std::move(section));
    }
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

const QSet<QString> &mediaSuffixes()
{
    static const QSet<QString> suffixes = [] {
        QSet<QString> set;
        const QStringList list = QStringLiteral(
            "mp4 m4v mkv webm avi mov wmv flv mpg mpeg 3gp m2ts mts ogv vob divx "
            "mp3 flac ogg oga opus m4a aac wav wma aiff aif ape mka alac")
                                     .split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString &suffix : list) {
            set.insert(suffix);
        }
        return set;
    }();
    return suffixes;
}

/// Dimensiones y formato de una imagen (o páginas de un PDF), sin decodificarla.
void readImage(const QString &path, const QString &suffix, QList<Section> &sections)
{
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    if (!reader.canRead()) {
        return;
    }
    if (suffix == QLatin1String("pdf")) {
        Section document{tr("Documento"), {}};
        const int pages = reader.imageCount();
        if (pages > 0) {
            add(document, tr("Páginas"), QLocale().toString(pages));
        }
        addSection(sections, std::move(document));
        return;
    }
    Section image{tr("Imagen"), {}};
    const QSize size = reader.size();
    if (size.isValid()) {
        add(image, tr("Dimensiones"), tr("%1 × %2 píxeles").arg(size.width()).arg(size.height()));
        add(image, tr("Megapíxeles"),
            QLocale().toString(double(size.width()) * size.height() / 1e6, 'f', 1));
    }
    add(image, tr("Formato"), QString::fromLatin1(reader.format()).toUpper());
    const int frames = reader.imageCount();
    if (frames > 1 && reader.supportsAnimation()) {
        add(image, tr("Fotogramas"), QLocale().toString(frames));
    }
    addSection(sections, std::move(image));
}

#ifdef EU_HAVE_EXIV2
/// EXIF de una foto: cámara, objetivo, fecha y ajustes de la toma, GPS.
void readExif(const QString &path, QList<Section> &sections)
{
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        Exiv2::XmpParser::initialize();
        Exiv2::LogMsg::setLevel(Exiv2::LogMsg::mute);   // un archivo raro no es un aviso
    });
    try {
#ifdef Q_OS_WIN
        auto image = Exiv2::ImageFactory::open(path.toStdWString());
#else
        auto image = Exiv2::ImageFactory::open(std::string(QFile::encodeName(path).constData()));
#endif
        if (!image) {
            return;
        }
        image->readMetadata();
        const Exiv2::ExifData &exif = image->exifData();
        if (exif.empty()) {
            return;
        }
        // El valor tal como lo escribiría una persona ("1/125 s", "F2.8").
        const auto text = [&](const char *key) -> QString {
            const auto found = exif.findKey(Exiv2::ExifKey(key));
            return found == exif.end() ? QString() : QString::fromStdString(found->print(&exif));
        };
        Section camera{tr("Cámara"), {}};
        QString model = text("Exif.Image.Model");
        const QString make = text("Exif.Image.Make");
        if (!make.isEmpty() && !model.startsWith(make, Qt::CaseInsensitive)) {
            model = make + QLatin1Char(' ') + model;
        }
        add(camera, tr("Cámara"), model);
        add(camera, tr("Objetivo"), text("Exif.Photo.LensModel"));
        QString taken = text("Exif.Photo.DateTimeOriginal");
        // "2024:03:12 16:57:40" -> "2024-03-12 16:57:40"
        if (taken.size() >= 10 && taken[4] == QLatin1Char(':') && taken[7] == QLatin1Char(':')) {
            taken[4] = QLatin1Char('-');
            taken[7] = QLatin1Char('-');
        }
        add(camera, tr("Fecha de la foto"), taken);
        add(camera, tr("Exposición"), text("Exif.Photo.ExposureTime"));
        add(camera, tr("Apertura"), text("Exif.Photo.FNumber"));
        add(camera, tr("ISO"), text("Exif.Photo.ISOSpeedRatings"));
        add(camera, tr("Distancia focal"), text("Exif.Photo.FocalLength"));
        add(camera, tr("Flash"), text("Exif.Photo.Flash"));
        add(camera, tr("Programa"), text("Exif.Image.Software"));
        // GPS en grados decimales, que es lo que entienden los mapas.
        const auto coordinate = [&](const char *valueKey, const char *refKey) -> QString {
            const auto value = exif.findKey(Exiv2::ExifKey(valueKey));
            if (value == exif.end() || value->count() < 3) {
                return {};
            }
            const double degrees = value->toFloat(0) + value->toFloat(1) / 60.0 + value->toFloat(2) / 3600.0;
            const QString ref = text(refKey);
            const bool negative = ref.startsWith(QLatin1Char('S')) || ref.startsWith(QLatin1Char('W'))
                || ref.contains(QLatin1String("South")) || ref.contains(QLatin1String("West"));
            return QString::number(negative ? -degrees : degrees, 'f', 6);
        };
        const QString latitude = coordinate("Exif.GPSInfo.GPSLatitude", "Exif.GPSInfo.GPSLatitudeRef");
        const QString longitude = coordinate("Exif.GPSInfo.GPSLongitude", "Exif.GPSInfo.GPSLongitudeRef");
        if (!latitude.isEmpty() && !longitude.isEmpty()) {
            add(camera, tr("Ubicación (GPS)"), latitude + QStringLiteral(", ") + longitude);
        }
        addSection(sections, std::move(camera));
    } catch (const std::exception &) {
        // Archivo que Exiv2 no entiende: sin EXIF, sin más.
    }
}
#endif

#ifdef EU_HAVE_FFMPEG
int interruptRequested(void *opaque)
{
    return static_cast<const tasks::CancelToken *>(opaque)->isCancelled() ? 1 : 0;
}

QString dictionaryValue(const AVDictionary *dictionary, const char *key)
{
    const AVDictionaryEntry *entry = av_dict_get(dictionary, key, nullptr, 0);
    return entry != nullptr ? QString::fromUtf8(entry->value) : QString();
}

/// Duración, códecs, resolución y etiquetas de un archivo de audio o video.
void readMedia(const QString &path, const tasks::CancelToken &cancel, QList<Section> &sections)
{
    static std::once_flag quiet;
    std::call_once(quiet, [] { av_log_set_level(AV_LOG_FATAL); });
    AVFormatContext *raw = avformat_alloc_context();
    if (raw == nullptr) {
        return;
    }
    raw->interrupt_callback.callback = &interruptRequested;
    raw->interrupt_callback.opaque = const_cast<tasks::CancelToken *>(&cancel);
    if (avformat_open_input(&raw, QFile::encodeName(path).constData(), nullptr, nullptr) < 0) {
        return;
    }
    const auto closer = [](AVFormatContext *context) { avformat_close_input(&context); };
    std::unique_ptr<AVFormatContext, decltype(closer)> format(raw, closer);
    if (avformat_find_stream_info(format.get(), nullptr) < 0) {
        return;
    }

    // Etiquetas (ID3, Vorbis, MP4…): están en el contenedor o en la pista de audio.
    Section tags{tr("Etiquetas"), {}};
    const auto tag = [&](const char *key) {
        QString value = dictionaryValue(format->metadata, key);
        for (unsigned i = 0; value.isEmpty() && i < format->nb_streams; ++i) {
            value = dictionaryValue(format->streams[i]->metadata, key);
        }
        return value;
    };
    add(tags, tr("Título"), tag("title"));
    add(tags, tr("Artista"), tag("artist"));
    add(tags, tr("Álbum"), tag("album"));
    add(tags, tr("Artista del álbum"), tag("album_artist"));
    add(tags, tr("Año"), tag("date"));
    add(tags, tr("Pista"), tag("track"));
    add(tags, tr("Género"), tag("genre"));
    add(tags, tr("Compositor"), tag("composer"));
    add(tags, tr("Comentario"), tag("comment"));

    Section general{tr("Multimedia"), {}};
    if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
        add(general, tr("Duración"), durationText(format->duration / (AV_TIME_BASE / 1000)));
    }
    if (format->iformat != nullptr && format->iformat->long_name != nullptr) {
        add(general, tr("Contenedor"), QString::fromUtf8(format->iformat->long_name));
    }
    if (format->bit_rate > 0) {
        add(general, tr("Tasa de bits"), tr("%1 kb/s").arg(format->bit_rate / 1000));
    }

    Section video{tr("Video"), {}};
    Section audio{tr("Audio"), {}};
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream *stream = format->streams[i];
        const AVCodecParameters *codec = stream->codecpar;
        const QString codecName = QString::fromUtf8(avcodec_get_name(codec->codec_id));
        if (codec->codec_type == AVMEDIA_TYPE_VIDEO && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)
            && video.fields.isEmpty()) {
            add(video, tr("Códec"), codecName);
            add(video, tr("Resolución"), tr("%1 × %2").arg(codec->width).arg(codec->height));
            const AVRational rate = stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
            if (rate.num > 0 && rate.den > 0) {
                add(video, tr("Fotogramas por segundo"), QLocale().toString(av_q2d(rate), 'f', 2));
            }
        } else if (codec->codec_type == AVMEDIA_TYPE_AUDIO && audio.fields.isEmpty()) {
            add(audio, tr("Códec"), codecName);
            const int channels = codec->ch_layout.nb_channels;
            add(audio, tr("Canales"), channels == 1 ? tr("1 (mono)") : channels == 2 ? tr("2 (estéreo)")
                                                                                       : QString::number(channels));
            if (codec->sample_rate > 0) {
                add(audio, tr("Frecuencia"), tr("%1 Hz").arg(QLocale().toString(codec->sample_rate)));
            }
            if (codec->bit_rate > 0) {
                add(audio, tr("Tasa de bits"), tr("%1 kb/s").arg(codec->bit_rate / 1000));
            }
        }
    }
    addSection(sections, std::move(tags));
    addSection(sections, std::move(general));
    addSection(sections, std::move(video));
    addSection(sections, std::move(audio));
}
#endif

} // namespace

bool hasExifSupport()
{
#ifdef EU_HAVE_EXIV2
    return true;
#else
    return false;
#endif
}

bool hasMediaSupport()
{
#ifdef EU_HAVE_FFMPEG
    return true;
#else
    return false;
#endif
}

QList<Section> readMetadata(const QString &path, const tasks::CancelToken &cancel)
{
    QList<Section> sections;
    const QFileInfo info(path);
    if (!info.isFile() || cancel.isCancelled()) {
        return sections;
    }
    const QString suffix = info.suffix().toLower();
    if (mediaSuffixes().contains(suffix)) {
#ifdef EU_HAVE_FFMPEG
        readMedia(path, cancel, sections);
#endif
        return sections;
    }
    readImage(path, suffix, sections);
#ifdef EU_HAVE_EXIV2
    if (!sections.isEmpty() && sections.first().title == tr("Imagen") && !cancel.isCancelled()) {
        readExif(path, sections);
    }
#endif
    return sections;
}

} // namespace eu::meta
