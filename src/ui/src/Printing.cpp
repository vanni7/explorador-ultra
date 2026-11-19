// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/Printing.h"

#include "eu/content/DocumentPreview.h"
#include "eu/content/TextExtractor.h"
#include "eu/thumbs/Isolated.h"
#include "eu/thumbs/Thumbnail.h"

#include <QAbstractTextDocumentLayout>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHash>
#include <QMessageBox>
#include <QPaintEngine>
#include <QPainter>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QPushButton>
#include <QStringDecoder>
#include <QTextDocument>
#include <QUrl>

#include <algorithm>

namespace eu::ui {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::ui::Printing", text);
}

/// Texto que se imprime como mucho: un registro de varios GB no se pagina entero.
constexpr qint64 maxTextBytes = 20LL * 1000 * 1000;
/// Lado mayor con que se pinta una página o una imagen al imprimir (≈ A4 a
/// 300 ppp) y en la vista previa (≈ 110 ppp: la vista previa guarda cada
/// página entera en memoria, y un libro de cientos de páginas no debe ocupar GB).
constexpr int maxRasterSide = 3508;
constexpr int maxPreviewSide = 1300;

/// true si se está pintando para la vista previa (la grabación de la página), no para la impresora.
bool isPreview(const QPainter &painter)
{
    return painter.paintEngine() != nullptr && painter.paintEngine()->type() == QPaintEngine::Picture;
}

/// Un documento que solo carga las imágenes que trae su vista previa.
class PrintDocument : public QTextDocument
{
public:
    QHash<QString, QImage> images;

protected:
    QVariant loadResource(int type, const QUrl &name) override
    {
        if (type == QTextDocument::ImageResource) {
            const auto image = images.constFind(name.toString());
            if (image != images.cend()) {
                return *image;
            }
        }
        return {};
    }
};

/// Las hojas de cálculo, como tablas HTML (una por hoja).
QString sheetsHtml(const content::DocumentPreview &preview)
{
    QString html = QStringLiteral("<html><body>");
    for (const content::SheetPreview &sheet : preview.sheets) {
        html += QStringLiteral("<h3>%1</h3><table border=\"1\" cellspacing=\"0\" cellpadding=\"2\">")
                    .arg(sheet.name.toHtmlEscaped());
        html += QStringLiteral("<tr><th></th>");
        for (int column = 1; column <= sheet.columns; ++column) {
            html += QStringLiteral("<th>%1</th>").arg(content::spreadsheetColumnName(column));
        }
        html += QStringLiteral("</tr>");
        for (qsizetype row = 0; row < sheet.cells.size(); ++row) {
            html += QStringLiteral("<tr><th>%1</th>").arg(sheet.rowNumbers.value(row));
            const QStringList &cells = sheet.cells.at(row);
            const QByteArray flags = sheet.numeric.value(row);
            for (int column = 0; column < sheet.columns; ++column) {
                const bool number = column < flags.size() && flags.at(column) == '1';
                html += number ? QStringLiteral("<td align=\"right\">") : QStringLiteral("<td>");
                html += cells.value(column).toHtmlEscaped();
                html += QStringLiteral("</td>");
            }
            html += QStringLiteral("</tr>");
        }
        html += QStringLiteral("</table>");
    }
    html += QStringLiteral("</body></html>");
    return html;
}

/// Imprime un QTextDocument con las páginas pedidas (QTextDocument::print ya
/// pagina y respeta fromPage/toPage).
bool printDocument(QTextDocument &document, QPrinter *printer)
{
    document.print(printer);
    return true;
}

/// Pinta `image` en la página entera del `painter`, sin agrandar más de lo
/// que cabe y centrada.
void drawFitted(QPainter &painter, const QImage &image)
{
    const QRect area = painter.viewport();
    QSize size = image.size();
    size.scale(area.size(), Qt::KeepAspectRatio);
    const QRect target(area.x() + (area.width() - size.width()) / 2, area.y() + (area.height() - size.height()) / 2,
                       size.width(), size.height());
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(target, image);
}

} // namespace

PrintKind printKindFor(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        return PrintKind::None;
    }
    thumbs::ThumbnailRequest request;
    request.path = path;
    request.size = info.size();
    switch (thumbs::thumbnailSourceFor(request)) {
    case thumbs::ThumbnailSource::Image:
    case thumbs::ThumbnailSource::Raw:
        return PrintKind::Image;
    case thumbs::ThumbnailSource::Pdf:
        return PrintKind::Pdf;
    default:
        break;
    }
    if (content::hasDocumentPreview(path)) {
        return PrintKind::Document;
    }
    if (content::textSourceFor(path) == content::TextSource::Plain) {
        return PrintKind::Text;
    }
    return PrintKind::None;
}

bool isPotentiallyDangerous(const QString &path)
{
    static const QStringList risky{
        QStringLiteral("exe"),    QStringLiteral("msi"),     QStringLiteral("bat"),     QStringLiteral("cmd"),
        QStringLiteral("com"),    QStringLiteral("scr"),     QStringLiteral("pif"),     QStringLiteral("ps1"),
        QStringLiteral("vbs"),    QStringLiteral("vbe"),     QStringLiteral("js"),      QStringLiteral("jse"),
        QStringLiteral("wsf"),    QStringLiteral("hta"),     QStringLiteral("jar"),     QStringLiteral("reg"),
        QStringLiteral("lnk"),    QStringLiteral("sh"),      QStringLiteral("run"),     QStringLiteral("bin"),
        QStringLiteral("appimage"), QStringLiteral("desktop"), QStringLiteral("deb"),   QStringLiteral("rpm"),
        QStringLiteral("app"),    QStringLiteral("command"), QStringLiteral("pkg"),     QStringLiteral("dmg"),
    };
    const QFileInfo info(path);
    return risky.contains(info.suffix().toLower()) || (info.isExecutable() && !info.isDir() && info.suffix().isEmpty());
}

bool printFile(const QString &path, PrintKind kind, QPrinter *printer, QString *error)
{
    const tasks::CancelToken noCancel;
    switch (kind) {
    case PrintKind::None:
        *error = tr("No se sabe imprimir este tipo de archivo.");
        return false;
    case PrintKind::Image: {
        const QFileInfo info(path);
        thumbs::ThumbnailRequest request;
        request.path = path;
        request.size = info.size();
        request.modifiedMs = info.lastModified().toMSecsSinceEpoch();
        QPainter painter;
        if (!painter.begin(printer)) {
            *error = tr("No se pudo empezar a imprimir.");
            return false;
        }
        request.maxSide = isPreview(painter) ? maxPreviewSide : maxRasterSide;
        const core::Result<QImage> image = thumbs::renderThumbnailIsolated(request, noCancel);
        if (!image.ok()) {
            *error = image.error().message;
            painter.end();
            return false;
        }
        drawFitted(painter, image.value());
        return painter.end();
    }
    case PrintKind::Pdf: {
        int pages = 0;
        const core::Result<QImage> counted = thumbs::renderPdfPageIsolated(path, -1, {}, &pages, noCancel);
        if (!counted.ok()) {
            *error = counted.error().message;
            return false;
        }
        int first = printer->fromPage() > 0 ? printer->fromPage() : 1;
        int last = printer->toPage() > 0 ? printer->toPage() : pages;
        first = std::clamp(first, 1, pages);
        last = std::clamp(last, first, pages);
        QPainter painter;
        if (!painter.begin(printer)) {
            *error = tr("No se pudo empezar a imprimir.");
            return false;
        }
        const int side = isPreview(painter) ? maxPreviewSide : maxRasterSide;
        QSize area = painter.viewport().size();
        if (area.width() > side || area.height() > side) {
            area.scale(side, side, Qt::KeepAspectRatio);
        }
        for (int page = first; page <= last; ++page) {
            if (page > first) {
                printer->newPage();
            }
            const core::Result<QImage> rendered = thumbs::renderPdfPageIsolated(path, page - 1, area, nullptr, noCancel);
            if (!rendered.ok()) {
                *error = rendered.error().message;
                painter.end();
                return false;
            }
            drawFitted(painter, rendered.value());
        }
        return painter.end();
    }
    case PrintKind::Text: {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = file.errorString();
            return false;
        }
        const QByteArray bytes = file.read(maxTextBytes);
        QStringDecoder utf8(QStringDecoder::Utf8);
        QString text = utf8.decode(bytes);
        if (utf8.hasError()) {
            text = QString::fromLatin1(bytes);
        }
        QTextDocument document;
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (suffix == QLatin1String("md") || suffix == QLatin1String("markdown")) {
            document.setMarkdown(text);
        } else {
            QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            font.setPointSizeF(9);
            document.setDefaultFont(font);
            document.setPlainText(text);
        }
        return printDocument(document, printer);
    }
    case PrintKind::Document: {
        const core::Result<content::DocumentPreview> preview = thumbs::documentPreviewIsolated(path, noCancel);
        if (!preview.ok()) {
            *error = preview.error().message;
            return false;
        }
        PrintDocument document;
        for (qsizetype i = 0; i < preview.value().images.size(); ++i) {
            document.images.insert(QStringLiteral("eu-imagen:%1").arg(i), QImage::fromData(preview.value().images.at(i)));
        }
        document.setHtml(preview.value().isSpreadsheet() ? sheetsHtml(preview.value()) : preview.value().html);
        return printDocument(document, printer);
    }
    }
    return false;
}

void printWithPreview(const QString &path, QWidget *parent)
{
    const QString name = QFileInfo(path).fileName();
    const PrintKind kind = printKindFor(path);
    if (kind == PrintKind::None) {
        if (isPotentiallyDangerous(path)) {
            QMessageBox::warning(parent, tr("Imprimir"),
                                 tr("«%1» es un programa, un guion o un instalador: no se imprime ni se abre desde "
                                    "aquí.").arg(name));
            return;
        }
        QMessageBox box(QMessageBox::Information, tr("Imprimir"),
                        tr("Explorador Ultra no sabe imprimir «%1». Puedes abrirlo con su programa e imprimir desde "
                           "allí.").arg(name),
                        QMessageBox::Cancel, parent);
        QPushButton *open = box.addButton(tr("Abrir con su programa"), QMessageBox::AcceptRole);
        box.exec();
        if (box.clickedButton() == open) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        }
        return;
    }
    if (kind == PrintKind::Pdf) {
        // Un PDF que no se puede abrir se dice ya, sin abrir una vista previa vacía.
        const core::Result<QImage> counted = thumbs::renderPdfPageIsolated(path, -1, {}, nullptr, {});
        if (!counted.ok()) {
            QMessageBox::warning(parent, tr("Imprimir"), tr("No se puede imprimir «%1»: %2").arg(name, counted.error().message));
            return;
        }
    }
    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(name);
    QPrintPreviewDialog preview(&printer, parent);
    preview.setWindowTitle(tr("Imprimir «%1»").arg(name));
    QObject::connect(&preview, &QPrintPreviewDialog::paintRequested, &preview, [path, kind, &preview](QPrinter *target) {
        QString error;
        if (!printFile(path, kind, target, &error)) {
            QMessageBox::warning(&preview, tr("Imprimir"), tr("No se pudo preparar la impresión: %1").arg(error));
        }
    });
    preview.exec();
}

} // namespace eu::ui
