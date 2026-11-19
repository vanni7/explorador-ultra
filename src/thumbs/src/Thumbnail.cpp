// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/thumbs/Thumbnail.h"

#include "MediaThumbnail.h"
#include "OfficeThumbnail.h"

#include "eu/core/NaturalCompare.h"
#include "eu/fs/DirectoryLister.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QImageReader>
#include <QPainter>
#include <QSet>
#include <QStringDecoder>
#include <QTextLayout>

#include <algorithm>

namespace eu::thumbs {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::thumbs", text);
}

/// Formatos de imagen que Qt sabe leer en esta instalación (complementos
/// incluidos: SVG, WebP, TIFF…), en minúsculas.
const QSet<QString> &readableImageSuffixes()
{
    static const QSet<QString> suffixes = [] {
        QSet<QString> set;
        const QList<QByteArray> formats = QImageReader::supportedImageFormats();
        for (const QByteArray &format : formats) {
            set.insert(QString::fromLatin1(format).toLower());
        }
        // Extensiones que no coinciden con el nombre del formato.
        if (set.contains(QStringLiteral("jpeg"))) {
            set.insert(QStringLiteral("jpg"));
            set.insert(QStringLiteral("jpe"));
            set.insert(QStringLiteral("jfif"));
        }
        if (set.contains(QStringLiteral("tiff"))) {
            set.insert(QStringLiteral("tif"));
        }
        if (set.contains(QStringLiteral("svg"))) {
            set.insert(QStringLiteral("svgz"));
        }
        // Qt PDF trae un complemento de imagen "pdf": se usa, pero como su
        // propia clase (ThumbnailSource::Pdf), no como una imagen más.
        set.remove(QStringLiteral("pdf"));
        return set;
    }();
    return suffixes;
}

bool hasPdfPlugin()
{
    static const bool available = QImageReader::supportedImageFormats().contains("pdf");
    return available;
}

const QSet<QString> &textSuffixes()
{
    static const QSet<QString> suffixes = [] {
        QSet<QString> set;
        const QStringList list = QStringLiteral(
            "txt text md markdown log csv tsv json xml yaml yml ini cfg conf toml tex rst srt nfo "
            "c h cpp hpp cc cxx hh py js ts tsx jsx java kt rs go rb php cs swift sh bash zsh bat cmd ps1 "
            "html htm css scss sql lua pl r m qml cmake mk diff patch")
                                     .split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString &suffix : list) {
            set.insert(suffix);
        }
        return set;
    }();
    return suffixes;
}

const QSet<QString> &videoSuffixes()
{
    // "ts" no: se confunde con TypeScript, que es más habitual en una carpeta.
    static const QSet<QString> suffixes{
        QStringLiteral("mp4"),  QStringLiteral("m4v"), QStringLiteral("mkv"), QStringLiteral("webm"),
        QStringLiteral("avi"),  QStringLiteral("mov"), QStringLiteral("wmv"), QStringLiteral("flv"),
        QStringLiteral("mpg"),  QStringLiteral("mpeg"), QStringLiteral("3gp"), QStringLiteral("m2ts"),
        QStringLiteral("mts"),  QStringLiteral("ogv"), QStringLiteral("vob"), QStringLiteral("divx"),
    };
    return suffixes;
}

const QSet<QString> &audioSuffixes()
{
    static const QSet<QString> suffixes{
        QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("ogg"), QStringLiteral("oga"),
        QStringLiteral("opus"), QStringLiteral("m4a"), QStringLiteral("aac"), QStringLiteral("wav"),
        QStringLiteral("wma"), QStringLiteral("aiff"), QStringLiteral("aif"), QStringLiteral("ape"),
        QStringLiteral("mka"), QStringLiteral("alac"),
    };
    return suffixes;
}

const QSet<QString> &officeSuffixes()
{
    static const QSet<QString> suffixes{
        QStringLiteral("odt"),  QStringLiteral("ods"),  QStringLiteral("odp"),  QStringLiteral("odg"),
        QStringLiteral("docx"), QStringLiteral("xlsx"), QStringLiteral("pptx"), QStringLiteral("doc"),
        QStringLiteral("xls"),  QStringLiteral("ppt"),  QStringLiteral("rtf"),
    };
    return suffixes;
}

/// Los formatos antiguos de Microsoft y RTF no llevan miniatura dentro:
/// solo LibreOffice sabe pintarlos.
bool needsLibreOffice(const QString &suffix)
{
    return suffix == QLatin1String("doc") || suffix == QLatin1String("xls") || suffix == QLatin1String("ppt")
        || suffix == QLatin1String("rtf");
}

const QSet<QString> &rawSuffixes()
{
    static const QSet<QString> suffixes{
        QStringLiteral("cr2"), QStringLiteral("cr3"), QStringLiteral("crw"), QStringLiteral("nef"),
        QStringLiteral("nrw"), QStringLiteral("arw"), QStringLiteral("srf"), QStringLiteral("sr2"),
        QStringLiteral("dng"), QStringLiteral("orf"), QStringLiteral("rw2"), QStringLiteral("raf"),
        QStringLiteral("pef"), QStringLiteral("srw"), QStringLiteral("x3f"), QStringLiteral("3fr"),
        QStringLiteral("erf"), QStringLiteral("kdc"), QStringLiteral("mrw"), QStringLiteral("rwl"),
        QStringLiteral("iiq"),
    };
    return suffixes;
}

/// Lado mayor a partir del cual una imagen se rechaza sin decodificarla: una
/// imagen de 100 000 × 100 000 píxeles ocuparía 40 GB en memoria.
constexpr int maxSourceSide = 30000;

} // namespace

ThumbnailSource thumbnailSourceFor(const QString &fileName)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix.isEmpty()) {
        return ThumbnailSource::None;
    }
    // RAW antes que imagen: algún complemento de Qt lee el TIFF de un DNG,
    // pero la vista previa de la cámara es mejor y más rápida.
    if (rawSuffixes().contains(suffix)) {
        return detail::hasRawSupport() ? ThumbnailSource::Raw : ThumbnailSource::None;
    }
    if (readableImageSuffixes().contains(suffix)) {
        return ThumbnailSource::Image;
    }
    if (officeSuffixes().contains(suffix)) {
        const bool possible = needsLibreOffice(suffix) ? !detail::libreOfficeProgram().isEmpty()
                                                       : detail::hasOfficeSupport();
        return possible ? ThumbnailSource::Office : ThumbnailSource::None;
    }
    if (suffix == QLatin1String("pdf")) {
        return hasPdfPlugin() ? ThumbnailSource::Pdf : ThumbnailSource::None;
    }
    if (textSuffixes().contains(suffix)) {
        return ThumbnailSource::Text;
    }
    if (detail::hasMediaSupport()) {
        if (videoSuffixes().contains(suffix)) {
            return ThumbnailSource::Video;
        }
        if (audioSuffixes().contains(suffix)) {
            return ThumbnailSource::Audio;
        }
    }
    return ThumbnailSource::None;
}

bool hasMediaThumbnails()
{
    return detail::hasMediaSupport();
}

ThumbnailSource thumbnailSourceFor(const ThumbnailRequest &request)
{
    return request.isDirectory ? ThumbnailSource::Folder : thumbnailSourceFor(request.path);
}

namespace {

core::Error cancelledError(const QString &path)
{
    return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), path, 0};
}

/// Formato único para la caché y para pintar rápido.
QImage normalized(QImage image)
{
    if (image.format() != QImage::Format_ARGB32_Premultiplied && image.format() != QImage::Format_RGB32) {
        image = image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied
                                                              : QImage::Format_RGB32);
    }
    return image;
}

core::Result<QImage> renderImage(const ThumbnailRequest &request, const tasks::CancelToken &cancel,
                                 const char *format = nullptr)
{
    QImageReader reader(request.path, format);
    // Gira según la etiqueta EXIF de orientación (fotos de móvil en vertical).
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);   // una extensión equivocada no impide leerla

    const QSize original = reader.size();
    if (original.isValid()
        && (original.width() > maxSourceSide || original.height() > maxSourceSide)) {
        return core::Error{core::ErrorCode::NotSupported, tr("La imagen es demasiado grande para la miniatura"),
                           request.path, 0};
    }
    const int maxSide = std::max(16, request.maxSide);
    // Si el formato sabe decodificar ya reducido (JPEG), se le pide así: es
    // mucho más rápido y gasta menos memoria que decodificar entera y reducir.
    if (original.isValid() && (original.width() > maxSide || original.height() > maxSide)
        && reader.supportsOption(QImageIOHandler::ScaledSize)) {
        reader.setScaledSize(original.scaled(maxSide, maxSide, Qt::KeepAspectRatio));
    }

    QImage image = reader.read();
    if (image.isNull()) {
        return core::Error{core::ErrorCode::Corrupted,
                           tr("No se pudo leer la imagen: %1").arg(reader.errorString()), request.path, 0};
    }
    if (cancel.isCancelled()) {
        return cancelledError(request.path);
    }
    if (image.width() > maxSide || image.height() > maxSide) {
        image = image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (format != nullptr && qstrcmp(format, "pdf") == 0 && image.hasAlphaChannel()) {
        // Una página de PDF sin fondo propio es papel blanco, no transparente.
        QImage page(image.size(), QImage::Format_RGB32);
        page.fill(Qt::white);
        QPainter painter(&page);
        painter.drawImage(0, 0, image);
        painter.end();
        image = page;
    }
    return normalized(image);
}

/// Las primeras líneas de un texto, pintadas sobre una hoja.
core::Result<QImage> renderText(const ThumbnailRequest &request, const tasks::CancelToken &cancel)
{
    QFile file(request.path);
    if (!file.open(QIODevice::ReadOnly)) {
        return core::Error{core::ErrorCode::PermissionDenied, file.errorString(), request.path, 0};
    }
    const QByteArray data = file.read(16 * 1024);   // basta para llenar la hoja
    if (data.contains('\0') && !data.startsWith("\xff\xfe") && !data.startsWith("\xfe\xff")) {
        return core::Error{core::ErrorCode::NotSupported, tr("No es un archivo de texto"), request.path, 0};
    }
    QString text;
    if (data.startsWith("\xff\xfe") || data.startsWith("\xfe\xff")) {
        text = QStringDecoder(QStringDecoder::Utf16).decode(data);
    } else {
        QStringDecoder utf8(QStringDecoder::Utf8);
        text = utf8.decode(data);
        if (utf8.hasError()) {
            text = QString::fromLatin1(data);
        }
    }
    if (cancel.isCancelled()) {
        return cancelledError(request.path);
    }
    // Hoja vertical, como un folio: alto = lado pedido.
    const int height = std::max(48, request.maxSide);
    const int width = height * 3 / 4;
    QImage page(width, height, QImage::Format_RGB32);
    page.fill(QColor(0xfc, 0xfc, 0xfa));
    QPainter painter(&page);
    painter.setPen(QColor(0xc8, 0xc8, 0xc4));
    painter.drawRect(0, 0, width - 1, height - 1);
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    // Unas 40 líneas por hoja, se vea como se vea: lo que importa es la forma.
    font.setPixelSize(std::max(3, height / 40));
    painter.setFont(font);
    painter.setPen(QColor(0x30, 0x30, 0x30));
    const int margin = std::max(3, width / 16);
    const QFontMetrics metrics(font);
    int y = margin + metrics.ascent();
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (QString line : lines) {
        if (y > height - margin) {
            break;
        }
        line.replace(QLatin1Char('\t'), QStringLiteral("    "));
        line.remove(QLatin1Char('\r'));
        painter.drawText(margin, y, metrics.elidedText(line, Qt::ElideRight, width - 2 * margin));
        y += metrics.lineSpacing();
    }
    painter.end();
    return page;
}

/// Hasta cuatro miniaturas de los primeros archivos de una carpeta, si son
/// todos de la misma clase.
core::Result<QImage> renderFolder(const ThumbnailRequest &request, const tasks::CancelToken &cancel)
{
    const core::Result<std::vector<fs::Entry>> listed = fs::listDirectoryAll(request.path);
    if (!listed.ok()) {
        return listed.error();
    }
    std::vector<fs::Entry> files;
    for (const fs::Entry &entry : listed.value()) {
        if (!entry.isDirectory && !entry.isHidden && !entry.statFailed) {
            files.push_back(entry);
        }
    }
    std::sort(files.begin(), files.end(), [](const fs::Entry &a, const fs::Entry &b) {
        return core::naturalCompare(a.name, b.name) < 0;
    });
    // ¿De qué clase son los primeros? Con que haya mezcla, se ve el icono.
    const size_t considered = std::min<size_t>(files.size(), 12);
    if (considered == 0) {
        return core::Error{core::ErrorCode::NotSupported, tr("Carpeta sin archivos"), request.path, 0};
    }
    const ThumbnailSource kind = thumbnailSourceFor(files.front().name);
    for (size_t i = 1; i < considered; ++i) {
        if (thumbnailSourceFor(files[i].name) != kind) {
            return core::Error{core::ErrorCode::NotSupported, tr("Contenido variado"), request.path, 0};
        }
    }
    if (kind == ThumbnailSource::None) {
        return core::Error{core::ErrorCode::NotSupported, tr("Sin miniaturas dentro"), request.path, 0};
    }
    const int side = std::max(32, request.maxSide);
    const int gap = std::max(2, side / 32);
    // Cuántas caben, y cómo repartirlas: una ocupa todo, dos van lado a lado,
    // tres o cuatro en cuadrícula de 2 × 2.
    const int wanted = int(std::min<size_t>(files.size(), 4));
    const int columns = wanted == 1 ? 1 : 2;
    const int rows = wanted <= 2 ? 1 : 2;
    const int cellWidth = (side - (columns - 1) * gap) / columns;
    const int cellHeight = (side - (rows - 1) * gap) / rows;
    std::vector<QImage> images;
    for (size_t i = 0; i < files.size() && int(images.size()) < wanted; ++i) {
        if (cancel.isCancelled()) {
            return cancelledError(request.path);
        }
        ThumbnailRequest child;
        child.path = request.path + QLatin1Char('/') + files[i].name;
        child.size = files[i].size;
        child.modifiedMs = files[i].modifiedMs;
        child.maxSide = std::max(cellWidth, cellHeight);
        const core::Result<QImage> image = renderThumbnail(child, cancel);
        if (image.ok()) {
            images.push_back(image.value());
        }
    }
    QImage grid(side, side, QImage::Format_ARGB32_Premultiplied);
    grid.fill(Qt::transparent);
    QPainter painter(&grid);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const int drawn = int(images.size());
    // Si alguna no se pudo leer, las que quedan se reparten en el mismo hueco.
    const int usedColumns = drawn == 1 ? 1 : columns;
    const int usedRows = drawn <= 2 ? 1 : rows;
    const int usedWidth = (side - (usedColumns - 1) * gap) / usedColumns;
    const int usedHeight = (side - (usedRows - 1) * gap) / usedRows;
    for (int i = 0; i < drawn; ++i) {
        QSize shown = images[size_t(i)].size();
        shown.scale(usedWidth, usedHeight, Qt::KeepAspectRatio);
        const int column = i % usedColumns;
        const int row = i / usedColumns;
        const QRect target(column * (usedWidth + gap) + (usedWidth - shown.width()) / 2,
                           row * (usedHeight + gap) + (usedHeight - shown.height()) / 2, shown.width(),
                           shown.height());
        painter.drawImage(target, images[size_t(i)]);
    }
    painter.end();
    if (drawn == 0) {
        return core::Error{core::ErrorCode::NotSupported, tr("Sin miniaturas dentro"), request.path, 0};
    }
    return grid;
}

} // namespace

core::Result<QImage> renderPdfPage(const QString &path, int page, const QSize &size, int *pageCount)
{
    if (!hasPdfPlugin()) {
        return core::Error{core::ErrorCode::NotSupported, tr("Esta compilación no lee PDF"), path, 0};
    }
    QImageReader reader(path, "pdf");
    const int count = reader.imageCount();
    if (pageCount != nullptr) {
        *pageCount = count;
    }
    if (count <= 0) {
        return core::Error{core::ErrorCode::Corrupted, tr("No se pudo abrir el PDF"), path, 0};
    }
    if (page < 0) {
        return QImage();
    }
    if (page >= count || !reader.jumpToImage(page)) {
        return core::Error{core::ErrorCode::NotFound, tr("El PDF no tiene esa página"), path, 0};
    }
    const QSize natural = reader.size();
    if (natural.isValid() && size.isValid() && !size.isEmpty()) {
        reader.setScaledSize(natural.scaled(size, Qt::KeepAspectRatio));
    }
    const QImage image = reader.read();
    if (image.isNull()) {
        return core::Error{core::ErrorCode::Corrupted, tr("No se pudo pintar la página: %1").arg(reader.errorString()),
                           path, 0};
    }
    // Papel blanco debajo: una página sin fondo propio no es transparente.
    QImage paper(image.size(), QImage::Format_RGB32);
    paper.fill(Qt::white);
    QPainter painter(&paper);
    painter.drawImage(0, 0, image);
    painter.end();
    return paper;
}

core::Result<QImage> renderThumbnail(const ThumbnailRequest &request, const tasks::CancelToken &cancel)
{
    if (cancel.isCancelled()) {
        return cancelledError(request.path);
    }
    switch (thumbnailSourceFor(request)) {
    case ThumbnailSource::Image:
        return renderImage(request, cancel);
    case ThumbnailSource::Pdf:
        return renderImage(request, cancel, "pdf");
    case ThumbnailSource::Text:
        return renderText(request, cancel);
    case ThumbnailSource::Folder:
        return renderFolder(request, cancel);
    case ThumbnailSource::Video:
        return detail::renderMedia(request, cancel, false);
    case ThumbnailSource::Audio:
        return detail::renderMedia(request, cancel, true);
    case ThumbnailSource::Office:
        return detail::renderOffice(request, cancel);
    case ThumbnailSource::Raw:
        return detail::renderRaw(request, cancel);
    case ThumbnailSource::None:
        break;
    }
    return core::Error{core::ErrorCode::NotSupported, tr("No hay miniatura para este tipo de archivo"),
                       request.path, 0};
}

} // namespace eu::thumbs
