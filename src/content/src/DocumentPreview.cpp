// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/content/DocumentPreview.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QLocale>
#include <QMap>
#include <QRegularExpression>
#include <QTime>
#include <QXmlStreamReader>

#ifdef EU_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

using namespace Qt::StringLiterals;

namespace eu::content {

namespace {

QString tr(const char *text, const char *disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("eu::content", text, disambiguation, n);
}

enum class Kind { None, Word, Excel, OpenText, OpenSheet };

Kind kindOf(const QString &fileName)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix == "docx"_L1 || suffix == "docm"_L1 || suffix == "dotx"_L1 || suffix == "dotm"_L1) {
        return Kind::Word;
    }
    if (suffix == "xlsx"_L1 || suffix == "xlsm"_L1 || suffix == "xltx"_L1 || suffix == "xltm"_L1) {
        return Kind::Excel;
    }
    if (suffix == "odt"_L1 || suffix == "ott"_L1) {
        return Kind::OpenText;
    }
    if (suffix == "ods"_L1 || suffix == "ots"_L1) {
        return Kind::OpenSheet;
    }
    return Kind::None;
}

#ifdef EU_HAVE_LIBARCHIVE

core::Error cancelledError(const QString &path)
{
    return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), path, 0};
}

/// Las entradas del ZIP cuyo nombre cumple `wanted`. De cada una se lee como
/// mucho `capFor(nombre)` bytes (de una mayor queda el principio) y en total
/// `maxTotal`: un XML que se infla sin medida (bomba) no llena la memoria.
core::Result<QHash<QString, QByteArray>> readParts(const QString &path, const QRegularExpression &wanted,
                                                   const std::function<qint64(const QString &)> &capFor,
                                                   qint64 maxTotal, const tasks::CancelToken &cancel)
{
    struct Deleter
    {
        void operator()(struct archive *handle) const { archive_read_free(handle); }
    };
    std::unique_ptr<struct archive, Deleter> handle(archive_read_new());
    archive_read_support_format_zip(handle.get());
#ifdef Q_OS_WIN
    // En Windows, con la ruta en UTF-16: con la de 8 bits fallarían los nombres con acentos.
    const int opened = archive_read_open_filename_w(
        handle.get(), reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(path).utf16()), 64 * 1024);
#else
    const int opened = archive_read_open_filename(handle.get(), QFile::encodeName(path).constData(), 64 * 1024);
#endif
    if (opened != ARCHIVE_OK) {
        return core::Error{core::ErrorCode::Corrupted, tr("No se pudo abrir el documento (no es un ZIP válido)"),
                           path, 0};
    }
    QHash<QString, QByteArray> parts;
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    qint64 total = 0;
    struct archive_entry *entry = nullptr;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(handle.get(), &entry)) == ARCHIVE_OK || status == ARCHIVE_WARN) {
        if (cancel.isCancelled()) {
            return cancelledError(path);
        }
        const QString name = QString::fromUtf8(archive_entry_pathname(entry));
        if (!wanted.match(name).hasMatch() || parts.contains(name)) {
            archive_read_data_skip(handle.get());
            continue;
        }
        const qint64 cap = capFor(name);
        QByteArray data;
        for (;;) {
            const la_ssize_t read = archive_read_data(handle.get(), buffer.data(), size_t(buffer.size()));
            if (read < 0) {
                return core::Error{core::ErrorCode::Corrupted, tr("El documento está dañado"), path, 0};
            }
            if (read == 0) {
                break;
            }
            const qint64 room = std::max<qint64>(0, std::min(cap - data.size(), maxTotal - total));
            const qint64 take = std::min<qint64>(read, room);
            data.append(buffer.constData(), qsizetype(take));
            total += take;
            if (take < read) {
                break;   // de lo que es mayor, solo el principio
            }
            if (cancel.isCancelled()) {
                return cancelledError(path);
            }
        }
        parts.insert(name, data);
        if (total >= maxTotal) {
            break;
        }
    }
    if (status == ARCHIVE_FATAL && parts.isEmpty()) {
        return core::Error{core::ErrorCode::Corrupted, tr("El documento está dañado"), path, 0};
    }
    return parts;
}

/// El texto escapado para HTML, sin perder los espacios seguidos.
QString escaped(const QString &text)
{
    QString out = text.toHtmlEscaped();
    out.replace(u"  "_s, u" &nbsp;"_s);
    return out;
}

QString imagePlaceholder()
{
    return u"<i>[%1]</i>"_s.arg(tr("imagen"));
}

/// Una imagen del documento decodificada y reducida al tamaño con que se ve
/// (el doble como mucho, para pantallas de alta densidad), en PNG si tiene
/// transparencia o en JPEG si no. `shown` entra con el tamaño que le da el
/// documento (o vacío) y sale con el que se mostrará. Vacío si no se puede
/// leer (EMF, WMF, dañada).
QByteArray reducedImage(const QByteArray &data, QSize *shown, int maxWidth)
{
    QBuffer buffer;
    buffer.setData(data);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {};
    }
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QSize original = reader.size();
    if (!original.isValid() || original.isEmpty()) {
        return {};
    }
    QSize display = shown->isValid() && !shown->isEmpty() ? *shown : original;
    if (display.width() > maxWidth) {
        display = QSize(maxWidth, std::max(1, int(qint64(display.height()) * maxWidth / display.width())));
    }
    const QSize wanted(display.width() * 2, display.height() * 2);
    if (original.width() > wanted.width() || original.height() > wanted.height()) {
        reader.setScaledSize(original.scaled(wanted, Qt::KeepAspectRatio));
    }
    const QImage image = reader.read();
    if (image.isNull()) {
        return {};
    }
    QByteArray out;
    QBuffer outBuffer(&out);
    if (!outBuffer.open(QIODevice::WriteOnly)) {
        return {};
    }
    const bool saved = image.hasAlphaChannel() ? image.save(&outBuffer, "PNG") : image.save(&outBuffer, "JPEG", 85);
    if (!saved) {
        return {};
    }
    *shown = display;
    return out;
}

/// Una longitud de OpenDocument («16.99cm», «2in», «120pt»…) en píxeles a 96 ppp; 0 si no se entiende.
int lengthToPixels(QStringView length)
{
    static const QRegularExpression pattern(u"^\\s*([0-9]*\\.?[0-9]+)\\s*(cm|mm|in|pt|pc|px)?\\s*$"_s);
    const QRegularExpressionMatch match = pattern.matchView(length);
    if (!match.hasMatch()) {
        return 0;
    }
    const double value = match.captured(1).toDouble();
    const QString unit = match.captured(2);
    const double factor = unit == "cm"_L1 ? 96.0 / 2.54
                          : unit == "mm"_L1 ? 96.0 / 25.4
                          : unit == "in"_L1 ? 96.0
                          : unit == "pt"_L1 ? 96.0 / 72.0
                          : unit == "pc"_L1 ? 16.0
                                            : 1.0;
    return int(std::clamp(value * factor, 0.0, 10000.0));
}

/// EMU (la unidad de los dibujos de Word: 914 400 por pulgada) en píxeles a 96 ppp.
int emuToPixels(QStringView emu)
{
    return int(std::clamp<qint64>(emu.toLongLong() / 9525, 0, 10000));
}

struct Format
{
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;

    Format merged(const Format &other) const
    {
        return {bold || other.bold, italic || other.italic, underline || other.underline, strike || other.strike};
    }
};

/// Va armando el HTML de un documento de texto: párrafos (también los que
/// aparecen dentro de otro, como los cuadros de texto), títulos, listas y
/// tablas, con topes de texto y de celdas.
class HtmlBuilder
{
public:
    struct Paragraph
    {
        QString text;
        int heading = 0;     ///< 1–6; 0 si es un párrafo normal.
        QString align;
        int indent = -1;     ///< Nivel de lista (0 = primero); -1 si no es de una lista.
        bool bullet = false;
        bool emitted = false;
    };

    explicit HtmlBuilder(const PreviewLimits &limits) : m_limits(limits) {}

    bool full() const { return m_truncated; }

    void startParagraph() { startParagraph(Paragraph()); }

    void startParagraph(const Paragraph &paragraph)
    {
        if (!m_paragraphs.isEmpty()) {
            flush(m_paragraphs.last(), false);   // un cuadro de texto dentro de otro párrafo
        }
        m_paragraphs.append(paragraph);
    }

    Paragraph *current() { return m_paragraphs.isEmpty() ? nullptr : &m_paragraphs.last(); }

    void endParagraph()
    {
        if (!m_paragraphs.isEmpty()) {
            flush(m_paragraphs.last(), true);
            m_paragraphs.removeLast();
        }
    }

    void text(const QString &text, const Format &format)
    {
        Paragraph *paragraph = current();
        if (paragraph == nullptr || text.isEmpty() || m_truncated) {
            return;
        }
        QString piece = escaped(text);
        if (format.bold) {
            piece = "<b>"_L1 + piece + "</b>"_L1;
        }
        if (format.italic) {
            piece = "<i>"_L1 + piece + "</i>"_L1;
        }
        if (format.underline) {
            piece = "<u>"_L1 + piece + "</u>"_L1;
        }
        if (format.strike) {
            piece = "<s>"_L1 + piece + "</s>"_L1;
        }
        paragraph->text += piece;
        m_chars += text.size();
        if (m_chars > m_limits.maxChars) {
            m_truncated = true;
        }
    }

    void raw(const QString &html)
    {
        if (Paragraph *paragraph = current()) {
            paragraph->text += html;
        }
    }

    /// Añade la imagen al párrafo. false si no se pudo (no se sabe leer, o se
    /// pasó de los topes): quien llama decide si deja «[imagen]».
    bool image(const QByteArray &data, QSize shown)
    {
        if (current() == nullptr || data.isEmpty() || m_images.size() >= m_limits.maxImages
            || m_imageBytes >= m_limits.maxImageBytes || data.size() > m_limits.maxImageFileBytes) {
            return false;
        }
        const QByteArray reduced = reducedImage(data, &shown, m_limits.maxImageWidth);
        if (reduced.isEmpty()) {
            return false;
        }
        m_images.append(reduced);
        m_imageBytes += reduced.size();
        raw(u"<img src=\"eu-imagen:%1\" width=\"%2\" height=\"%3\"/>"_s.arg(m_images.size() - 1)
                .arg(shown.width())
                .arg(shown.height()));
        return true;
    }

    QList<QByteArray> images() const { return m_images; }

    void pageBreak()
    {
        if (Paragraph *paragraph = current()) {
            flush(*paragraph, false);
        }
        m_body += u"<hr/>"_s;
    }

    void startTable()
    {
        if (Paragraph *paragraph = current()) {
            flush(*paragraph, false);
        }
        m_body += u"<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\">"_s;
        ++m_openTables;
    }

    void endTable()
    {
        if (m_openTables > 0) {
            m_body += u"</table>"_s;
            --m_openTables;
        }
    }

    void startRow() { m_body += u"<tr>"_s; }
    void endRow() { m_body += u"</tr>"_s; }

    void startCell(int span = 1)
    {
        m_body += u"<td valign=\"top\">"_s;
        m_cells.append(m_body.size() - 1);   // dónde va un colspan que llegue después
        setCellSpan(span);
        if (++m_cellCount > m_limits.maxTableCells) {
            m_truncated = true;
        }
    }

    void setCellSpan(int span)
    {
        if (!m_cells.isEmpty() && span > 1 && span < 1000) {
            m_body.insert(m_cells.last(), u" colspan=\"%1\""_s.arg(span));
        }
    }

    void endCell()
    {
        m_body += u"</td>"_s;
        if (!m_cells.isEmpty()) {
            m_cells.removeLast();
        }
    }

    QString finish()
    {
        while (!m_paragraphs.isEmpty()) {
            endParagraph();
        }
        while (m_openTables > 0) {
            endTable();
        }
        QString html = u"<html><head><style>p { margin-top: 0px; margin-bottom: 6px; }</style></head><body>"_s;
        html += m_body;
        if (m_truncated) {
            html += u"<hr/><p><i>%1</i></p>"_s.arg(tr("[… solo se muestra el principio del documento]").toHtmlEscaped());
        }
        html += u"</body></html>"_s;
        return html;
    }

private:
    void flush(Paragraph &paragraph, bool final)
    {
        // Un párrafo vacío cuenta como línea en blanco, salvo los restos de
        // uno partido por un cuadro de texto o un salto de página.
        if (paragraph.text.isEmpty() && (paragraph.emitted || !final)) {
            return;
        }
        const int heading = std::clamp(paragraph.heading, 0, 6);
        QString open = heading > 0 ? u"<h%1"_s.arg(heading) : u"<p"_s;
        if (!paragraph.align.isEmpty()) {
            open += u" align=\"%1\""_s.arg(paragraph.align);
        }
        if (paragraph.indent >= 0) {
            open += u" style=\"margin-left: %1px\""_s.arg(20 * (std::min(paragraph.indent, 8) + 1));
        }
        m_body += open + u'>';
        if (paragraph.bullet && !paragraph.emitted) {
            m_body += u"•&nbsp;"_s;
        }
        m_body += paragraph.text;
        m_body += heading > 0 ? u"</h%1>"_s.arg(heading) : u"</p>"_s;
        paragraph.text.clear();
        paragraph.emitted = true;
    }

    QString m_body;
    QList<Paragraph> m_paragraphs;
    QList<qsizetype> m_cells;
    int m_openTables = 0;
    QList<QByteArray> m_images;
    qint64 m_imageBytes = 0;
    qint64 m_chars = 0;
    int m_cellCount = 0;
    const PreviewLimits &m_limits;
    bool m_truncated = false;
};

// ---------------------------------------------------------------- Word

int headingFromStyleName(const QString &lowerName)
{
    if (lowerName == "title"_L1) {
        return 1;
    }
    if (lowerName.startsWith("heading "_L1)) {
        bool ok = false;
        const int level = lowerName.mid(8).toInt(&ok);
        if (ok && level >= 1) {
            return std::min(level + 1, 6);
        }
    }
    return 0;
}

/// Sin styles.xml: por el identificador del estilo, en inglés o como lo
/// guarda Word en español («Ttulo1» es «Título 1»).
int headingFromStyleId(const QString &id)
{
    static const QRegularExpression pattern(u"^(?:heading|title|ttulo|t[ií]tulo)(\\d?)$"_s,
                                            QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(id);
    if (!match.hasMatch()) {
        return 0;
    }
    return match.captured(1).isEmpty() ? 1 : std::min(match.captured(1).toInt() + 1, 6);
}

/// Nivel de título de cada estilo de párrafo de word/styles.xml (siguiendo
/// los estilos en los que se basa).
QHash<QString, int> wordHeadings(const QByteArray &stylesXml)
{
    struct Style
    {
        QString name;
        QString basedOn;
        int outline = -1;
    };
    QHash<QString, Style> styles;
    QXmlStreamReader reader(stylesXml);
    QString current;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isEndElement() && reader.qualifiedName() == "w:style"_L1) {
            current.clear();
            continue;
        }
        if (!reader.isStartElement()) {
            continue;
        }
        const QStringView name = reader.qualifiedName();
        const QXmlStreamAttributes attributes = reader.attributes();
        if (name == "w:style"_L1) {
            current = attributes.value(u"w:type"_s) == "paragraph"_L1 ? attributes.value(u"w:styleId"_s).toString()
                                                                       : QString();
            if (!current.isEmpty()) {
                styles.insert(current, {});
            }
        } else if (!current.isEmpty()) {
            const QString value = attributes.value(u"w:val"_s).toString();
            if (name == "w:name"_L1) {
                styles[current].name = value.toLower();
            } else if (name == "w:basedOn"_L1) {
                styles[current].basedOn = value;
            } else if (name == "w:outlineLvl"_L1) {
                styles[current].outline = value.toInt();
            }
        }
    }
    QHash<QString, int> levels;
    for (auto it = styles.cbegin(); it != styles.cend(); ++it) {
        QString id = it.key();
        for (int step = 0; step < 8; ++step) {
            const auto style = styles.constFind(id);
            if (style == styles.cend()) {
                break;
            }
            int level = headingFromStyleName(style->name);
            if (level == 0 && style->outline >= 0 && style->outline < 9) {
                level = std::min(style->outline + 2, 6);
            }
            if (level > 0) {
                levels.insert(it.key(), level);
                break;
            }
            id = style->basedOn;
        }
    }
    return levels;
}

QString alignFromWord(QStringView value)
{
    if (value == "center"_L1) {
        return u"center"_s;
    }
    if (value == "right"_L1 || value == "end"_L1) {
        return u"right"_s;
    }
    if (value == "both"_L1 || value == "distribute"_L1) {
        return u"justify"_s;
    }
    return {};
}

bool wordToggle(QStringView value)
{
    return value.isEmpty() || !(value == "0"_L1 || value == "false"_L1 || value == "off"_L1);
}

/// Las partes del ZIP a las que apuntan los identificadores de
/// word/_rels/document.xml.rels (las imágenes). Las externas no: verlo
/// nunca carga nada de fuera.
QHash<QString, QString> wordTargets(const QByteArray &relationsXml)
{
    QHash<QString, QString> targets;
    QXmlStreamReader reader(relationsXml);
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement() || reader.name() != "Relationship"_L1) {
            continue;
        }
        const QXmlStreamAttributes attributes = reader.attributes();
        if (attributes.value(u"TargetMode"_s) == "External"_L1) {
            continue;
        }
        const QString target = attributes.value(u"Target"_s).toString();
        targets.insert(attributes.value(u"Id"_s).toString(),
                       QDir::cleanPath(target.startsWith(u'/') ? target.mid(1) : "word/"_L1 + target));
    }
    return targets;
}

QString wordToHtml(const QHash<QString, QByteArray> &files, const PreviewLimits &limits,
                   const tasks::CancelToken &cancel, bool *truncated, QList<QByteArray> *images)
{
    const QHash<QString, int> headings = wordHeadings(files.value(u"word/styles.xml"_s));
    const QHash<QString, QString> targets = wordTargets(files.value(u"word/_rels/document.xml.rels"_s));
    HtmlBuilder builder(limits);
    QXmlStreamReader reader(files.value(u"word/document.xml"_s));
    QSize extent;   // el tamaño del último dibujo (<wp:extent>), para su imagen
    Format run;
    bool inRun = false;
    bool inRunProperties = false;
    bool inParagraphProperties = false;
    bool noList = false;
    int events = 0;
    while (!reader.atEnd() && !builder.full()) {
        if ((++events & 0xFFF) == 0 && cancel.isCancelled()) {
            break;
        }
        reader.readNext();
        if (reader.isEndElement()) {
            const QStringView name = reader.qualifiedName();
            if (name == "w:p"_L1) {
                builder.endParagraph();
            } else if (name == "w:pPr"_L1) {
                inParagraphProperties = false;
                if (HtmlBuilder::Paragraph *paragraph = builder.current(); paragraph != nullptr && noList) {
                    paragraph->indent = -1;
                    paragraph->bullet = false;
                }
            } else if (name == "w:r"_L1) {
                inRun = false;
                inRunProperties = false;
            } else if (name == "w:rPr"_L1) {
                inRunProperties = false;
            } else if (name == "w:tbl"_L1) {
                builder.endTable();
            } else if (name == "w:tr"_L1) {
                builder.endRow();
            } else if (name == "w:tc"_L1) {
                builder.endCell();
            }
            continue;
        }
        if (!reader.isStartElement()) {
            continue;
        }
        const QStringView name = reader.qualifiedName();
        const QXmlStreamAttributes attributes = reader.attributes();
        const QStringView value = attributes.value(u"w:val"_s);
        // Lo que no se ve: la versión de reserva de un dibujo (repetiría su
        // texto), lo borrado con control de cambios y los cambios de formato.
        if (name == "mc:Fallback"_L1 || name == "w:del"_L1 || name == "w:moveFrom"_L1 || name == "w:sectPr"_L1
            || name == "w:rPrChange"_L1 || name == "w:pPrChange"_L1 || name == "w:instrText"_L1) {
            reader.skipCurrentElement();
            continue;
        }
        if (name == "w:p"_L1) {
            builder.startParagraph();
            noList = false;
        } else if (name == "w:pPr"_L1) {
            inParagraphProperties = true;
        } else if (inParagraphProperties) {
            HtmlBuilder::Paragraph *paragraph = builder.current();
            if (paragraph == nullptr) {
                continue;
            }
            if (name == "w:pStyle"_L1) {
                const QString style = value.toString();
                paragraph->heading = headings.contains(style) ? headings.value(style) : headingFromStyleId(style);
            } else if (name == "w:jc"_L1) {
                paragraph->align = alignFromWord(value);
            } else if (name == "w:numPr"_L1) {
                paragraph->indent = std::max(paragraph->indent, 0);
                paragraph->bullet = true;
            } else if (name == "w:ilvl"_L1) {
                paragraph->indent = std::clamp(value.toInt(), 0, 8);
            } else if (name == "w:numId"_L1 && value == "0"_L1) {
                noList = true;   // "sin lista" explícito
            } else if (name == "w:rPr"_L1) {
                reader.skipCurrentElement();   // formato de la marca de párrafo
            }
        } else if (name == "w:r"_L1) {
            run = {};
            inRun = true;
        } else if (name == "w:rPr"_L1 && inRun) {
            inRunProperties = true;
        } else if (inRunProperties) {
            if (name == "w:b"_L1) {
                run.bold = wordToggle(value);
            } else if (name == "w:i"_L1) {
                run.italic = wordToggle(value);
            } else if (name == "w:u"_L1) {
                run.underline = value != "none"_L1 && value != "0"_L1;
            } else if (name == "w:strike"_L1 || name == "w:dstrike"_L1) {
                run.strike = wordToggle(value);
            }
        } else if (name == "w:t"_L1) {
            builder.text(reader.readElementText(QXmlStreamReader::SkipChildElements), run);
        } else if (name == "w:tab"_L1 && inRun) {
            builder.raw(u"&nbsp;&nbsp;&nbsp;&nbsp;"_s);
        } else if (name == "w:br"_L1 || name == "w:cr"_L1) {
            if (attributes.value(u"w:type"_s) == "page"_L1) {
                builder.pageBreak();
            } else {
                builder.raw(u"<br/>"_s);
            }
        } else if (name == "w:noBreakHyphen"_L1) {
            builder.text(u"-"_s, run);
        } else if (name == "wp:extent"_L1) {
            extent = QSize(emuToPixels(attributes.value(u"cx"_s)), emuToPixels(attributes.value(u"cy"_s)));
        } else if (name == "a:blip"_L1 || name == "v:imagedata"_L1) {
            QString id;
            for (const QXmlStreamAttribute &attribute : attributes) {
                if (attribute.name() == "embed"_L1 || (name == "v:imagedata"_L1 && attribute.name() == "id"_L1)) {
                    id = attribute.value().toString();
                }
            }
            const auto target = targets.constFind(id);
            if (target == targets.cend() || !builder.image(files.value(*target), extent)) {
                builder.raw(imagePlaceholder());
            }
            extent = QSize();
        } else if (name == "w:tbl"_L1) {
            builder.startTable();
        } else if (name == "w:tr"_L1) {
            builder.startRow();
        } else if (name == "w:tc"_L1) {
            builder.startCell();
        } else if (name == "w:gridSpan"_L1) {
            builder.setCellSpan(value.toInt());
        }
    }
    *truncated = builder.full();
    *images = builder.images();
    return builder.finish();
}

// ------------------------------------------------------- OpenDocument texto

QString odtToHtml(const QHash<QString, QByteArray> &files, const PreviewLimits &limits,
                  const tasks::CancelToken &cancel, bool *truncated, QList<QByteArray> *images)
{
    const QByteArray contentXml = files.value(u"content.xml"_s);
    QSize frameSize;           // el de <draw:frame>, para su imagen
    int frameImages = 0;       // <draw:image> dentro del marco (SVG y su copia en PNG, por ejemplo)
    bool frameShown = false;   // ya se puso una
    struct Style
    {
        Format format;
        QString align;
        bool title = false;
    };
    QHash<QString, Style> styles;
    QString currentStyle;
    HtmlBuilder builder(limits);
    QList<Format> formats;   // el del párrafo y los de los <text:span> abiertos
    int listDepth = 0;
    bool pendingBullet = false;
    QXmlStreamReader reader(contentXml);
    int events = 0;
    while (!reader.atEnd() && !builder.full()) {
        if ((++events & 0xFFF) == 0 && cancel.isCancelled()) {
            break;
        }
        reader.readNext();
        if (reader.isCharacters()) {
            if (builder.current() != nullptr && !formats.isEmpty()) {
                builder.text(reader.text().toString(), formats.last());
            }
            continue;
        }
        if (reader.isEndElement()) {
            const QStringView name = reader.qualifiedName();
            if (name == "style:style"_L1) {
                currentStyle.clear();
            } else if (name == "text:p"_L1 || name == "text:h"_L1) {
                builder.endParagraph();
                if (!formats.isEmpty()) {
                    formats.removeLast();
                }
            } else if (name == "text:span"_L1) {
                if (formats.size() > 1) {
                    formats.removeLast();
                }
            } else if (name == "text:list"_L1) {
                listDepth = std::max(0, listDepth - 1);
            } else if (name == "table:table"_L1) {
                builder.endTable();
            } else if (name == "table:table-row"_L1) {
                builder.endRow();
            } else if (name == "table:table-cell"_L1) {
                builder.endCell();
            } else if (name == "draw:frame"_L1) {
                if (frameImages > 0 && !frameShown) {
                    builder.raw(imagePlaceholder());
                }
                frameImages = 0;
            }
            continue;
        }
        if (!reader.isStartElement()) {
            continue;
        }
        const QStringView name = reader.qualifiedName();
        const QXmlStreamAttributes attributes = reader.attributes();
        // Comentarios, notas al pie (su texto iría en medio del párrafo),
        // cambios registrados y lo tapado por una celda combinada.
        if (name == "office:annotation"_L1 || name == "text:note-body"_L1 || name == "text:tracked-changes"_L1
            || name == "table:covered-table-cell"_L1 || name == "office:forms"_L1) {
            reader.skipCurrentElement();
            continue;
        }
        if (name == "style:style"_L1) {
            currentStyle = attributes.value(u"style:name"_s).toString();
            const QString parent = attributes.value(u"style:parent-style-name"_s).toString();
            Style style = styles.value(parent);
            style.title = style.title || parent == "Title"_L1;
            styles.insert(currentStyle, style);
        } else if (name == "style:text-properties"_L1 && !currentStyle.isEmpty()) {
            Format &format = styles[currentStyle].format;
            const QStringView weight = attributes.value(u"fo:font-weight"_s);
            format.bold = weight == "bold"_L1 || weight.toInt() >= 600;
            const QStringView slant = attributes.value(u"fo:font-style"_s);
            format.italic = slant == "italic"_L1 || slant == "oblique"_L1;
            const QStringView underline = attributes.value(u"style:text-underline-style"_s);
            format.underline = !underline.isEmpty() && underline != "none"_L1;
            const QStringView strike = attributes.value(u"style:text-line-through-style"_s);
            format.strike = !strike.isEmpty() && strike != "none"_L1;
        } else if (name == "style:paragraph-properties"_L1 && !currentStyle.isEmpty()) {
            const QStringView align = attributes.value(u"fo:text-align"_s);
            styles[currentStyle].align = align == "center"_L1                       ? u"center"_s
                                         : (align == "end"_L1 || align == "right"_L1) ? u"right"_s
                                         : align == "justify"_L1                    ? u"justify"_s
                                                                                    : QString();
        } else if (name == "text:p"_L1 || name == "text:h"_L1) {
            const QString styleName = attributes.value(u"text:style-name"_s).toString();
            const Style style = styles.value(styleName);
            HtmlBuilder::Paragraph paragraph;
            if (name == "text:h"_L1) {
                const int level = attributes.value(u"text:outline-level"_s).toInt();
                paragraph.heading = std::min(std::max(level, 1) + 1, 6);
            } else if (style.title || styleName == "Title"_L1) {
                paragraph.heading = 1;
            }
            paragraph.align = style.align;
            if (listDepth > 0) {
                paragraph.indent = listDepth - 1;
                paragraph.bullet = pendingBullet;
                pendingBullet = false;
            }
            builder.startParagraph(paragraph);
            formats.append(style.format);
        } else if (name == "text:span"_L1) {
            const Format outer = formats.isEmpty() ? Format{} : formats.last();
            formats.append(outer.merged(styles.value(attributes.value(u"text:style-name"_s).toString()).format));
        } else if (name == "text:list"_L1) {
            ++listDepth;
        } else if (name == "text:list-item"_L1) {
            pendingBullet = true;
        } else if (name == "text:s"_L1) {
            const int count = std::clamp(attributes.value(u"text:c"_s).toInt(), 1, 200);
            if (!formats.isEmpty()) {
                builder.text(QString(count, u' '), formats.last());
            }
        } else if (name == "text:tab"_L1) {
            builder.raw(u"&nbsp;&nbsp;&nbsp;&nbsp;"_s);
        } else if (name == "text:line-break"_L1) {
            builder.raw(u"<br/>"_s);
        } else if (name == "draw:frame"_L1) {
            frameSize = QSize(lengthToPixels(attributes.value(u"svg:width"_s)),
                              lengthToPixels(attributes.value(u"svg:height"_s)));
            frameImages = 0;
            frameShown = false;
        } else if (name == "draw:image"_L1) {
            ++frameImages;
            const QString href = attributes.value(u"xlink:href"_s).toString();
            // Solo las que van dentro del archivo («Pictures/…»): nada de fuera.
            if (!frameShown && !href.contains(u':')) {
                frameShown = builder.image(files.value(QDir::cleanPath(href)), frameSize);
            }
        } else if (name == "table:table"_L1) {
            builder.startTable();
        } else if (name == "table:table-row"_L1) {
            builder.startRow();
        } else if (name == "table:table-cell"_L1) {
            builder.startCell(attributes.value(u"table:number-columns-spanned"_s).toInt());
        }
    }
    *truncated = builder.full();
    *images = builder.images();
    return builder.finish();
}

// ---------------------------------------------------------------- Excel

enum class NumberKind { Plain, Percent, Date, Time, DateTime };

/// Formatos de número que Excel trae de serie (por su número).
NumberKind kindOfBuiltinFormat(int id)
{
    if (id == 9 || id == 10) {
        return NumberKind::Percent;
    }
    if ((id >= 14 && id <= 17) || (id >= 27 && id <= 36) || (id >= 50 && id <= 58)) {
        return NumberKind::Date;
    }
    if ((id >= 18 && id <= 21) || (id >= 45 && id <= 47)) {
        return NumberKind::Time;
    }
    if (id == 22) {
        return NumberKind::DateTime;
    }
    return NumberKind::Plain;
}

/// Qué es un formato propio («dd/mm/yyyy», «0.0%», «[h]:mm»…): se quitan los
/// textos entre comillas, los caracteres escapados y los corchetes que no son
/// horas (colores, idioma) y se miran las letras que quedan.
NumberKind kindOfCustomFormat(const QString &code)
{
    QString bare;
    bool quoted = false;
    for (qsizetype i = 0; i < code.size(); ++i) {
        const QChar c = code.at(i);
        if (quoted) {
            quoted = c != u'"';
            continue;
        }
        if (c == u'"') {
            quoted = true;
        } else if (c == u'\\' || c == u'_' || c == u'*') {
            ++i;   // un carácter literal, el ancho de uno o el relleno
        } else if (c == u'[') {
            const qsizetype close = code.indexOf(u']', i);
            if (close < 0) {
                break;
            }
            const QString inside = code.mid(i + 1, close - i - 1).toLower();
            if (!inside.isEmpty() && inside.count(u'h') + inside.count(u'm') + inside.count(u's') == inside.size()) {
                bare += inside;   // [h], [mm], [ss]: horas transcurridas
            }
            i = close;
        } else if (c == u';') {
            break;   // basta con la primera sección (la de los positivos)
        } else {
            bare += c.toLower();
        }
    }
    if (bare.contains(u'%')) {
        return NumberKind::Percent;
    }
    const bool time = bare.contains(u'h') || bare.contains(u's');
    const bool date = bare.contains(u'y') || bare.contains(u'd')
        || (bare.contains(u'm') && !time && !bare.contains(u'0') && !bare.contains(u'#'));
    if (date && time) {
        return NumberKind::DateTime;
    }
    if (date) {
        return NumberKind::Date;
    }
    return time ? NumberKind::Time : NumberKind::Plain;
}

/// Qué es el número de cada formato de celda (el atributo s de las celdas).
QList<NumberKind> excelCellKinds(const QByteArray &stylesXml)
{
    QHash<int, QString> customFormats;
    QList<int> cellFormats;
    QXmlStreamReader reader(stylesXml);
    bool inCellFormats = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QStringView name = reader.name();
            if (name == "numFmt"_L1) {
                customFormats.insert(reader.attributes().value(u"numFmtId"_s).toInt(),
                                     reader.attributes().value(u"formatCode"_s).toString());
            } else if (name == "cellXfs"_L1) {
                inCellFormats = true;
            } else if (name == "xf"_L1 && inCellFormats) {
                cellFormats.append(reader.attributes().value(u"numFmtId"_s).toInt());
            }
        } else if (reader.isEndElement() && reader.name() == "cellXfs"_L1) {
            inCellFormats = false;
        }
    }
    QList<NumberKind> kinds;
    kinds.reserve(cellFormats.size());
    for (const int id : std::as_const(cellFormats)) {
        kinds.append(customFormats.contains(id) ? kindOfCustomFormat(customFormats.value(id))
                                                : kindOfBuiltinFormat(id));
    }
    return kinds;
}

/// El texto de un <si> o un <is>: el de sus <t>, sin las guías fonéticas (<rPh>).
QString richText(QXmlStreamReader &reader)
{
    QString text;
    int depth = 1;
    while (depth > 0 && !reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.name() == "rPh"_L1) {
                reader.skipCurrentElement();
            } else if (reader.name() == "t"_L1) {
                text += reader.readElementText(QXmlStreamReader::SkipChildElements);
            } else {
                ++depth;
            }
        } else if (reader.isEndElement()) {
            --depth;
        }
    }
    return text;
}

QStringList excelSharedStrings(const QByteArray &xml)
{
    QStringList strings;
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == "si"_L1) {
            strings.append(richText(reader));
        }
    }
    return strings;
}

struct SheetRef
{
    QString name;
    QString part;
};

/// Las hojas visibles, en el orden de las pestañas, con la parte del ZIP de cada una.
QList<SheetRef> excelSheets(const QByteArray &workbookXml, const QByteArray &relationsXml, bool *date1904)
{
    QHash<QString, QString> targets;
    QXmlStreamReader relations(relationsXml);
    while (!relations.atEnd()) {
        relations.readNext();
        if (relations.isStartElement() && relations.name() == "Relationship"_L1) {
            const QString target = relations.attributes().value(u"Target"_s).toString();
            targets.insert(relations.attributes().value(u"Id"_s).toString(),
                           QDir::cleanPath(target.startsWith(u'/') ? target.mid(1) : "xl/"_L1 + target));
        }
    }
    QList<SheetRef> sheets;
    QXmlStreamReader reader(workbookXml);
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement()) {
            continue;
        }
        const QXmlStreamAttributes attributes = reader.attributes();
        if (reader.name() == "workbookPr"_L1) {
            const QStringView value = attributes.value(u"date1904"_s);
            *date1904 = value == "1"_L1 || value == "true"_L1;
        } else if (reader.name() == "sheet"_L1) {
            const QStringView state = attributes.value(u"state"_s);
            if (state == "hidden"_L1 || state == "veryHidden"_L1) {
                continue;
            }
            QString id;
            for (const QXmlStreamAttribute &attribute : attributes) {
                if (attribute.name() == "id"_L1) {   // r:id, sea cual sea el prefijo
                    id = attribute.value().toString();
                }
            }
            sheets.append({attributes.value(u"name"_s).toString(), targets.value(id)});
        }
    }
    return sheets;
}

int columnFromReference(QStringView reference)
{
    int column = 0;
    for (const QChar c : reference) {
        if (c >= u'A' && c <= u'Z') {
            column = column * 26 + (c.unicode() - u'A' + 1);
        } else if (c >= u'a' && c <= u'z') {
            column = column * 26 + (c.unicode() - u'a' + 1);
        } else {
            break;
        }
        if (column > 100000) {
            return 0;
        }
    }
    return column;
}

int rowFromReference(QStringView reference)
{
    qsizetype start = 0;
    while (start < reference.size() && reference.at(start).isLetter()) {
        ++start;
    }
    return reference.mid(start).toInt();
}

QString plainNumber(double value)
{
    QLocale locale;
    locale.setNumberOptions(QLocale::OmitGroupSeparator);
    if (std::abs(value) < 1e15 && value == std::floor(value)) {
        return locale.toString(qlonglong(value));
    }
    return locale.toString(value, 'g', 12);
}

QString numberText(double value, NumberKind kind, bool date1904)
{
    if (kind == NumberKind::Plain) {
        return plainNumber(value);
    }
    if (kind == NumberKind::Percent) {
        return plainNumber(value * 100) + u'%';
    }
    if (value < 0 || value > 2958465) {   // fuera de 1900–9999: no es una fecha
        return plainNumber(value);
    }
    // Excel cuenta los días desde el 31-dic-1899 y da por bueno el 29-feb-1900
    // (que no existió): desde el 1-mar-1900 la cuenta empieza un día antes.
    const QDate base = date1904 ? QDate(1904, 1, 1) : (value < 60 ? QDate(1899, 12, 31) : QDate(1899, 12, 30));
    double whole = std::floor(value);
    qint64 seconds = std::llround((value - whole) * 86400.0);
    if (seconds >= 86400) {
        whole += 1;
        seconds -= 86400;
    }
    const QDate date = base.addDays(qint64(whole));
    const QTime time = QTime(0, 0).addSecs(int(seconds));
    const QString timeText = time.toString(seconds % 60 != 0 ? u"HH:mm:ss"_s : u"HH:mm"_s);
    if (kind == NumberKind::Time) {
        return timeText;
    }
    const QLocale locale;
    QString format = locale.dateFormat(QLocale::ShortFormat);
    if (!format.contains("yyyy"_L1)) {
        format.replace(u"yy"_s, u"yyyy"_s);   // el año entero: «15/03/2023», no «15/03/23»
    }
    const QString dateText = locale.toString(date, format);
    return kind == NumberKind::Date ? dateText : dateText + u' ' + timeText;
}

void putCell(SheetPreview &sheet, int rowIndex, int column, const QString &text, bool number)
{
    QStringList &cells = sheet.cells[rowIndex];
    QByteArray &flags = sheet.numeric[rowIndex];
    while (cells.size() < column) {
        cells.append(QString());
        flags.append('0');
    }
    cells[column - 1] = text;
    flags[column - 1] = number ? '1' : '0';
    sheet.columns = std::max(sheet.columns, column);
}

SheetPreview readExcelSheet(const QByteArray &xml, const QStringList &shared, const QList<NumberKind> &kinds,
                            bool date1904, const PreviewLimits &limits, const tasks::CancelToken &cancel)
{
    SheetPreview sheet;
    QXmlStreamReader reader(xml);
    int row = 0;
    int column = 0;
    int rowIndex = -1;   // la de `sheet` donde van las celdas de esta fila; -1 si aún no hay
    QString type;
    int style = 0;
    QString value;
    QString inlineText;
    QString formula;
    bool inCell = false;
    int events = 0;
    while (!reader.atEnd()) {
        if ((++events & 0xFFF) == 0 && cancel.isCancelled()) {
            break;
        }
        reader.readNext();
        if (reader.isStartElement()) {
            const QStringView name = reader.name();
            const QXmlStreamAttributes attributes = reader.attributes();
            if (name == "dimension"_L1) {
                const QStringView reference = attributes.value(u"ref"_s);
                sheet.totalRows = rowFromReference(reference.mid(reference.indexOf(u':') + 1));
            } else if (name == "row"_L1) {
                if (sheet.rowNumbers.size() >= limits.maxRows) {
                    sheet.moreRows = true;
                    break;
                }
                const int number = attributes.value(u"r"_s).toInt();
                row = number > row ? number : row + 1;
                column = 0;
                rowIndex = -1;
            } else if (name == "c"_L1) {
                const int number = columnFromReference(attributes.value(u"r"_s));
                column = number > 0 ? number : column + 1;
                type = attributes.value(u"t"_s).toString();
                style = attributes.value(u"s"_s).toInt();
                value.clear();
                inlineText.clear();
                formula.clear();
                inCell = true;
            } else if (inCell && name == "v"_L1) {
                value = reader.readElementText(QXmlStreamReader::SkipChildElements);
            } else if (inCell && name == "is"_L1) {
                inlineText = richText(reader);
            } else if (inCell && name == "f"_L1) {
                // Se muestra el resultado guardado (<v>); la fórmula, solo si no lo hay.
                formula = reader.readElementText(QXmlStreamReader::SkipChildElements);
            }
            continue;
        }
        if (!reader.isEndElement() || reader.name() != "c"_L1 || !inCell) {
            continue;
        }
        inCell = false;
        QString text;
        bool number = false;
        if (type == "s"_L1) {
            bool ok = false;
            const int index = value.toInt(&ok);
            text = ok && index >= 0 && index < shared.size() ? shared.at(index) : QString();
        } else if (type == "inlineStr"_L1) {
            text = inlineText;
        } else if (type == "b"_L1) {
            text = value == "1"_L1 ? tr("VERDADERO") : tr("FALSO");
        } else if (type == "str"_L1 || type == "e"_L1 || type == "d"_L1) {
            text = value;
        } else if (!value.isEmpty()) {
            bool ok = false;
            const double parsed = value.toDouble(&ok);
            number = ok;
            text = ok ? numberText(parsed, style >= 0 && style < kinds.size() ? kinds.at(style) : NumberKind::Plain,
                                   date1904)
                      : value;
        }
        if (text.isEmpty() && value.isEmpty() && !formula.isEmpty()) {
            // Guardada por un programa que no la calculó (p. ej. una biblioteca
            // que genera hojas): se muestra la fórmula, como al editarla.
            text = u'=' + formula;
        }
        if (text.isEmpty()) {
            continue;
        }
        if (column > limits.maxColumns) {
            sheet.moreColumns = true;
            continue;
        }
        if (rowIndex < 0) {
            sheet.rowNumbers.append(row);
            sheet.cells.append(QStringList());
            sheet.numeric.append(QByteArray());
            rowIndex = int(sheet.cells.size() - 1);
        }
        putCell(sheet, rowIndex, column, text, number);
    }
    // De una hoja muy grande solo se leyó el principio: hay más filas.
    if (reader.error() == QXmlStreamReader::PrematureEndOfDocumentError && !sheet.rowNumbers.isEmpty()) {
        sheet.moreRows = true;
    }
    return sheet;
}

// ------------------------------------------------------- OpenDocument cálculo

QList<SheetPreview> readOpenSheets(const QByteArray &contentXml, const PreviewLimits &limits,
                                   const tasks::CancelToken &cancel, int *moreSheets)
{
    QList<SheetPreview> sheets;
    QXmlStreamReader reader(contentXml);
    int row = 0;
    int column = 0;
    int rowRepeat = 1;
    int columnRepeat = 1;
    bool inCell = false;
    int paragraphDepth = 0;
    int paragraphsInCell = 0;
    bool numeric = false;
    QString cellText;
    QMap<int, QPair<QString, bool>> rowCells;
    bool skippingSheet = false;
    int events = 0;
    const auto repeatOf = [](QStringView text) { return std::clamp(text.toInt(), 1, 10000000); };
    while (!reader.atEnd()) {
        if ((++events & 0xFFF) == 0 && cancel.isCancelled()) {
            break;
        }
        reader.readNext();
        if (reader.isCharacters()) {
            if (inCell && paragraphDepth > 0) {
                cellText += reader.text();
            }
            continue;
        }
        const QStringView name = reader.qualifiedName();
        if (reader.isStartElement()) {
            const QXmlStreamAttributes attributes = reader.attributes();
            if (name == "office:annotation"_L1) {
                reader.skipCurrentElement();
            } else if (name == "table:table"_L1) {
                if (sheets.size() >= limits.maxSheets) {
                    ++*moreSheets;
                    reader.skipCurrentElement();
                    continue;
                }
                SheetPreview sheet;
                sheet.name = attributes.value(u"table:name"_s).toString();
                sheets.append(sheet);
                row = 0;
                skippingSheet = false;
                inCell = false;
                paragraphDepth = 0;
            } else if (sheets.isEmpty() || skippingSheet) {
                continue;
            } else if (name == "table:table-row"_L1) {
                rowRepeat = repeatOf(attributes.value(u"table:number-rows-repeated"_s));
                column = 0;
                rowCells.clear();
            } else if (name == "table:table-cell"_L1 || name == "table:covered-table-cell"_L1) {
                columnRepeat = repeatOf(attributes.value(u"table:number-columns-repeated"_s));
                const QStringView type = attributes.value(u"office:value-type"_s);
                numeric = type == "float"_L1 || type == "percentage"_L1 || type == "currency"_L1 || type == "date"_L1
                    || type == "time"_L1;
                inCell = true;
                paragraphsInCell = 0;
                cellText.clear();
            } else if (inCell && name == "text:p"_L1) {
                if (paragraphsInCell++ > 0) {
                    cellText += u'\n';
                }
                ++paragraphDepth;
            } else if (inCell && name == "text:s"_L1) {
                cellText += QString(std::clamp(attributes.value(u"text:c"_s).toInt(), 1, 200), u' ');
            } else if (inCell && name == "text:tab"_L1) {
                cellText += u'\t';
            } else if (inCell && name == "text:line-break"_L1) {
                cellText += u'\n';
            }
            continue;
        }
        if (!reader.isEndElement() || sheets.isEmpty() || skippingSheet) {
            continue;
        }
        if (name == "text:p"_L1 && inCell) {
            paragraphDepth = std::max(0, paragraphDepth - 1);
        } else if (name == "table:table-cell"_L1 || name == "table:covered-table-cell"_L1) {
            inCell = false;
            if (!cellText.isEmpty()) {
                for (int copy = 0; copy < columnRepeat; ++copy) {
                    if (column + copy + 1 > limits.maxColumns) {
                        sheets.last().moreColumns = true;
                        break;
                    }
                    rowCells.insert(column + copy + 1, {cellText, numeric});
                }
            }
            column = std::min(column + columnRepeat, 10000000);
        } else if (name == "table:table-row"_L1) {
            SheetPreview &sheet = sheets.last();
            if (!rowCells.isEmpty()) {
                for (int copy = 0; copy < rowRepeat; ++copy) {
                    if (sheet.rowNumbers.size() >= limits.maxRows) {
                        sheet.moreRows = true;
                        skippingSheet = true;   // el resto de esta hoja ya no se muestra
                        break;
                    }
                    sheet.rowNumbers.append(row + copy + 1);
                    sheet.cells.append(QStringList());
                    sheet.numeric.append(QByteArray());
                    const int rowIndex = int(sheet.cells.size() - 1);
                    for (auto cell = rowCells.cbegin(); cell != rowCells.cend(); ++cell) {
                        putCell(sheet, rowIndex, cell.key(), cell->first, cell->second);
                    }
                }
            }
            row = std::min(row + rowRepeat, 100000000);
        }
    }
    return sheets;
}

#endif // EU_HAVE_LIBARCHIVE

} // namespace

bool hasDocumentPreview(const QString &fileName)
{
#ifdef EU_HAVE_LIBARCHIVE
    return kindOf(fileName) != Kind::None;
#else
    Q_UNUSED(fileName);
    return false;
#endif
}

QString spreadsheetColumnName(int column)
{
    QString name;
    while (column > 0) {
        const int rest = (column - 1) % 26;
        name.prepend(QChar(u'A' + rest));
        column = (column - 1) / 26;
    }
    return name;
}

core::Result<DocumentPreview> documentPreview(const QString &path, const tasks::CancelToken &cancel,
                                              const PreviewLimits &limits)
{
#ifndef EU_HAVE_LIBARCHIVE
    Q_UNUSED(cancel);
    Q_UNUSED(limits);
    return core::Error{core::ErrorCode::NotSupported, tr("Esta compilación no lee documentos de oficina"), path, 0};
#else
    const Kind kind = kindOf(path);
    if (kind == Kind::None) {
        return core::Error{core::ErrorCode::NotSupported, tr("No se sabe mostrar este tipo de documento"), path, 0};
    }
    const QFileInfo info(path);
    if (!info.isFile()) {
        return core::Error{core::ErrorCode::NotFound, tr("No es un archivo"), path, 0};
    }
    if (info.size() > limits.maxFileBytes) {
        return core::Error{core::ErrorCode::NotSupported, tr("Demasiado grande para la vista previa"), path, 0};
    }
    // Una imagen mayor que el tope no se mostrará: se lee solo un poco (y no se decodifica).
    const auto mediaCap = [&limits](const QString &name) {
        return name.endsWith(".xml"_L1) || name.endsWith(".rels"_L1) ? limits.maxPartBytes
                                                                     : limits.maxImageFileBytes + 1;
    };
    DocumentPreview preview;
    switch (kind) {
    case Kind::Word: {
        const auto parts = readParts(
            path, QRegularExpression(u"^word/(document\\.xml|styles\\.xml|_rels/document\\.xml\\.rels|media/[^/]+)$"_s),
            mediaCap, limits.maxTotalBytes, cancel);
        if (!parts.ok()) {
            return parts.error();
        }
        if (!parts.value().contains(u"word/document.xml"_s)) {
            return core::Error{core::ErrorCode::Corrupted, tr("No es un documento de Word válido"), path, 0};
        }
        preview.html = wordToHtml(parts.value(), limits, cancel, &preview.truncated, &preview.images);
        break;
    }
    case Kind::OpenText:
    case Kind::OpenSheet: {
        const auto parts = readParts(path,
                                     QRegularExpression(kind == Kind::OpenText ? u"^(content\\.xml|Pictures/[^/]+)$"_s
                                                                               : u"^content\\.xml$"_s),
                                     mediaCap, limits.maxTotalBytes, cancel);
        if (!parts.ok()) {
            return parts.error();
        }
        if (!parts.value().contains(u"content.xml"_s)) {
            return core::Error{core::ErrorCode::Corrupted, tr("No es un documento OpenDocument válido"), path, 0};
        }
        if (kind == Kind::OpenText) {
            preview.html = odtToHtml(parts.value(), limits, cancel, &preview.truncated, &preview.images);
        } else {
            preview.sheets = readOpenSheets(parts.value().value(u"content.xml"_s), limits, cancel, &preview.moreSheets);
        }
        break;
    }
    case Kind::Excel: {
        const auto parts = readParts(
            path,
            QRegularExpression(
                u"^xl/(workbook\\.xml|_rels/workbook\\.xml\\.rels|sharedStrings\\.xml|styles\\.xml|worksheets/[^/]+\\.xml)$"_s),
            [&limits](const QString &name) {
                return name.startsWith("xl/worksheets/"_L1) ? limits.maxSheetBytes : limits.maxPartBytes;
            },
            limits.maxTotalBytes, cancel);
        if (!parts.ok()) {
            return parts.error();
        }
        const QHash<QString, QByteArray> &files = parts.value();
        bool date1904 = false;
        QList<SheetRef> sheets = excelSheets(files.value(u"xl/workbook.xml"_s),
                                             files.value(u"xl/_rels/workbook.xml.rels"_s), &date1904);
        if (sheets.isEmpty()) {
            // Sin libro (o sin hojas visibles en él): las hojas que haya, por su número.
            QList<QPair<int, QString>> found;
            static const QRegularExpression sheetPart(u"^xl/worksheets/sheet(\\d+)\\.xml$"_s);
            for (auto it = files.cbegin(); it != files.cend(); ++it) {
                const QRegularExpressionMatch match = sheetPart.match(it.key());
                if (match.hasMatch()) {
                    found.append({match.captured(1).toInt(), it.key()});
                }
            }
            std::sort(found.begin(), found.end());
            for (const auto &[number, part] : std::as_const(found)) {
                sheets.append({tr("Hoja %1").arg(number), part});
            }
        }
        const QStringList shared = excelSharedStrings(files.value(u"xl/sharedStrings.xml"_s));
        const QList<NumberKind> kinds = excelCellKinds(files.value(u"xl/styles.xml"_s));
        for (const SheetRef &sheet : std::as_const(sheets)) {
            if (cancel.isCancelled()) {
                break;
            }
            if (!files.contains(sheet.part)) {
                continue;   // una hoja de gráfico, o se pasó del tope total
            }
            if (preview.sheets.size() >= limits.maxSheets) {
                ++preview.moreSheets;
                continue;
            }
            SheetPreview read = readExcelSheet(files.value(sheet.part), shared, kinds, date1904, limits, cancel);
            read.name = sheet.name;
            preview.sheets.append(read);
        }
        if (preview.sheets.isEmpty() && !cancel.isCancelled()) {
            return core::Error{core::ErrorCode::Corrupted, tr("No es un libro de Excel válido"), path, 0};
        }
        break;
    }
    case Kind::None:
        break;
    }
    if (cancel.isCancelled()) {
        return cancelledError(path);
    }
    for (const SheetPreview &sheet : std::as_const(preview.sheets)) {
        preview.truncated = preview.truncated || sheet.moreRows || sheet.moreColumns;
    }
    preview.truncated = preview.truncated || preview.moreSheets > 0;
    return preview;
#endif
}

QJsonObject documentPreviewToJson(const DocumentPreview &preview)
{
    QJsonArray sheets;
    for (const SheetPreview &sheet : preview.sheets) {
        QJsonArray rows;
        for (qsizetype i = 0; i < sheet.cells.size(); ++i) {
            QJsonArray row{sheet.rowNumbers.value(i), QString::fromLatin1(sheet.numeric.value(i))};
            for (const QString &cell : sheet.cells.at(i)) {
                row.append(cell);
            }
            rows.append(row);
        }
        sheets.append(QJsonObject{{u"name"_s, sheet.name},
                                  {u"columns"_s, sheet.columns},
                                  {u"moreRows"_s, sheet.moreRows},
                                  {u"moreColumns"_s, sheet.moreColumns},
                                  {u"totalRows"_s, sheet.totalRows},
                                  {u"rows"_s, rows}});
    }
    QJsonArray images;
    for (const QByteArray &image : preview.images) {
        images.append(QString::fromLatin1(image.toBase64()));
    }
    return {{u"html"_s, preview.html},
            {u"images"_s, images},
            {u"sheets"_s, sheets},
            {u"moreSheets"_s, preview.moreSheets},
            {u"truncated"_s, preview.truncated}};
}

DocumentPreview documentPreviewFromJson(const QJsonObject &object)
{
    DocumentPreview preview;
    preview.html = object.value(u"html"_s).toString();
    const QJsonArray images = object.value(u"images"_s).toArray();
    for (const QJsonValue &image : images) {
        preview.images.append(QByteArray::fromBase64(image.toString().toLatin1()));
    }
    preview.moreSheets = object.value(u"moreSheets"_s).toInt();
    preview.truncated = object.value(u"truncated"_s).toBool();
    const QJsonArray sheets = object.value(u"sheets"_s).toArray();
    for (const QJsonValue &item : sheets) {
        const QJsonObject json = item.toObject();
        SheetPreview sheet;
        sheet.name = json.value(u"name"_s).toString();
        sheet.columns = json.value(u"columns"_s).toInt();
        sheet.moreRows = json.value(u"moreRows"_s).toBool();
        sheet.moreColumns = json.value(u"moreColumns"_s).toBool();
        sheet.totalRows = json.value(u"totalRows"_s).toInt();
        const QJsonArray rows = json.value(u"rows"_s).toArray();
        for (const QJsonValue &rowValue : rows) {
            const QJsonArray row = rowValue.toArray();
            sheet.rowNumbers.append(row.at(0).toInt());
            sheet.numeric.append(row.at(1).toString().toLatin1());
            QStringList cells;
            for (qsizetype i = 2; i < row.size(); ++i) {
                cells.append(row.at(i).toString());
            }
            sheet.cells.append(cells);
        }
        preview.sheets.append(sheet);
    }
    return preview;
}

} // namespace eu::content
