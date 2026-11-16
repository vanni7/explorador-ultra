// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/content/TextExtractor.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QXmlStreamReader>

#ifdef EU_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif
#ifdef EU_HAVE_QTPDF
#include <QPdfDocument>
#include <QPdfSelection>
#endif

#include <algorithm>
#include <memory>

namespace eu::content {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::content", text);
}

const QSet<QString> &plainSuffixes()
{
    static const QSet<QString> suffixes = [] {
        QSet<QString> set;
        const QStringList list = QStringLiteral(
            "txt text md markdown log csv tsv json xml yaml yml ini cfg conf toml tex rst srt nfo html htm css "
            "c h cpp hpp cc cxx hh py js ts tsx jsx java kt rs go rb php cs swift sh bash zsh bat cmd ps1 "
            "sql lua pl r m qml cmake mk diff patch")
                                     .split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString &suffix : list) {
            set.insert(suffix);
        }
        return set;
    }();
    return suffixes;
}

core::Error tooBig(const QString &path)
{
    return core::Error{core::ErrorCode::NotSupported, tr("El archivo es demasiado grande para leer su texto"), path, 0};
}

QString decodePlain(const QByteArray &data)
{
    if (data.startsWith("\xff\xfe") || data.startsWith("\xfe\xff")) {
        return QStringDecoder(QStringDecoder::Utf16).decode(data);
    }
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8.decode(data);
    if (utf8.hasError()) {
        text = QString::fromLatin1(data);   // texto antiguo de Windows
    }
    return text;
}

core::Result<QString> readPlain(const QString &path, const ExtractLimits &limits)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return core::Error{core::ErrorCode::PermissionDenied, file.errorString(), path, 0};
    }
    // Un texto enorme (un registro) se lee hasta el tope, no entero.
    const QByteArray data = file.read(limits.maxTextChars * 2);
    if (data.left(8192).contains('\0') && !data.startsWith("\xff\xfe") && !data.startsWith("\xfe\xff")) {
        return core::Error{core::ErrorCode::NotSupported, tr("No es un archivo de texto"), path, 0};
    }
    QString text = decodePlain(data);
    if (text.size() > limits.maxTextChars) {
        text.truncate(qsizetype(limits.maxTextChars));
    }
    return text;
}

/// RTF: se quitan los códigos de formato y se queda el texto.
QString rtfToText(const QByteArray &rtf)
{
    QString out;
    int depth = 0;
    QList<int> skipFrom;   // profundidades de los grupos que se saltan ({\*…}, fuentes, colores…)
    int unicodeSkip = 1;
    int pendingSkip = 0;
    for (qsizetype i = 0; i < rtf.size(); ++i) {
        const char c = rtf[i];
        const bool skipping = !skipFrom.isEmpty();
        if (c == '{') {
            ++depth;
            continue;
        }
        if (c == '}') {
            if (!skipFrom.isEmpty() && skipFrom.last() == depth) {
                skipFrom.removeLast();
            }
            --depth;
            continue;
        }
        if (c == '\\') {
            if (i + 1 >= rtf.size()) {
                break;
            }
            const char next = rtf[i + 1];
            if (next == '\\' || next == '{' || next == '}') {
                if (!skipping) {
                    out += QLatin1Char(next);
                }
                ++i;
                continue;
            }
            if (next == '\'' && i + 3 < rtf.size()) {
                // Carácter en hexadecimal (página de códigos de Windows 1252).
                bool ok = false;
                const int code = QByteArray(rtf.constData() + i + 2, 2).toInt(&ok, 16);
                if (ok && !skipping) {
                    if (pendingSkip > 0) {
                        --pendingSkip;
                    } else {
                        out += QString::fromLatin1(QByteArray(1, char(code)));
                    }
                }
                i += 3;
                continue;
            }
            if (next == '*') {
                skipFrom.append(depth);   // destino opcional: se ignora entero
                ++i;
                continue;
            }
            // Palabra de control: letras y un número opcional.
            qsizetype j = i + 1;
            while (j < rtf.size() && std::isalpha(static_cast<unsigned char>(rtf[j]))) {
                ++j;
            }
            const QByteArray word = rtf.mid(i + 1, j - i - 1);
            qsizetype k = j;
            if (k < rtf.size() && (rtf[k] == '-' || std::isdigit(static_cast<unsigned char>(rtf[k])))) {
                ++k;
                while (k < rtf.size() && std::isdigit(static_cast<unsigned char>(rtf[k]))) {
                    ++k;
                }
            }
            const int number = rtf.mid(j, k - j).toInt();
            if (k < rtf.size() && rtf[k] == ' ') {
                ++k;   // el espacio que separa la palabra de control del texto
            }
            i = k - 1;
            if (word == "fonttbl" || word == "colortbl" || word == "stylesheet" || word == "info"
                || word == "pict" || word == "header" || word == "footer") {
                skipFrom.append(depth);
            } else if (skipping) {
                // nada
            } else if (word == "par" || word == "line" || word == "row") {
                out += QLatin1Char('\n');
            } else if (word == "tab" || word == "cell") {
                out += QLatin1Char('\t');
            } else if (word == "uc") {
                unicodeSkip = number;
            } else if (word == "u") {
                out += QChar(char16_t(number < 0 ? number + 65536 : number));
                pendingSkip = unicodeSkip;   // se salta el carácter de repuesto que sigue
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            continue;
        }
        if (!skipping) {
            if (pendingSkip > 0) {
                --pendingSkip;
            } else {
                out += QLatin1Char(c);
            }
        }
    }
    return out;
}

#ifdef EU_HAVE_LIBARCHIVE
/// Lee del ZIP las entradas cuyo nombre cumple `wanted`, en orden de nombre
/// natural (slide2 antes que slide10), como mucho `maxBytes` en total.
QList<QByteArray> readZipEntries(const QString &path, const QRegularExpression &wanted, qint64 maxBytes,
                                 const tasks::CancelToken &cancel)
{
    struct Deleter
    {
        void operator()(struct archive *handle) const { archive_read_free(handle); }
    };
    std::unique_ptr<struct archive, Deleter> handle(archive_read_new());
    archive_read_support_format_zip(handle.get());
    if (archive_read_open_filename(handle.get(), QFile::encodeName(path).constData(), 64 * 1024) != ARCHIVE_OK) {
        return {};
    }
    QList<QPair<QString, QByteArray>> found;
    qint64 total = 0;
    struct archive_entry *entry = nullptr;
    while (archive_read_next_header(handle.get(), &entry) == ARCHIVE_OK && !cancel.isCancelled()) {
        const QString name = QString::fromUtf8(archive_entry_pathname(entry));
        if (!wanted.match(name).hasMatch()) {
            archive_read_data_skip(handle.get());
            continue;
        }
        QByteArray data;
        char buffer[64 * 1024];
        la_ssize_t read = 0;
        while ((read = archive_read_data(handle.get(), buffer, sizeof(buffer))) > 0) {
            total += read;
            if (total > maxBytes) {
                return {};   // un documento cuyo XML se infla sin medida: bomba
            }
            data.append(buffer, read);
        }
        found.append({name, data});
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        // "slide2.xml" antes que "slide10.xml".
        static const QRegularExpression number(QStringLiteral("(\\d+)\\.xml$"));
        const auto ma = number.match(a.first);
        const auto mb = number.match(b.first);
        if (ma.hasMatch() && mb.hasMatch() && a.first.left(ma.capturedStart()) == b.first.left(mb.capturedStart())) {
            return ma.captured(1).toInt() < mb.captured(1).toInt();
        }
        return a.first < b.first;
    });
    QList<QByteArray> result;
    for (const auto &pair : found) {
        result.append(pair.second);
    }
    return result;
}

/// El texto de un XML de oficina: lo que hay dentro de los elementos de
/// texto (`textElements`), con un salto de línea al cerrar cada párrafo
/// (`paragraphElements`) y un tabulador en los tabuladores.
QString xmlText(const QByteArray &xml, const QSet<QString> &textElements, const QSet<QString> &paragraphElements,
                const QSet<QString> &tabElements, bool allTextInside)
{
    QString out;
    QXmlStreamReader reader(xml);
    int insideText = 0;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QString name = reader.qualifiedName().toString();
            if (textElements.contains(name)) {
                ++insideText;
            } else if (tabElements.contains(name)) {
                out += QLatin1Char('\t');
            } else if (name == QLatin1String("text:s")) {
                // OpenDocument guarda los espacios seguidos como <text:s text:c="3"/>.
                out += QString(std::max(1, reader.attributes().value(QStringLiteral("text:c")).toInt()),
                               QLatin1Char(' '));
            } else if (name == QLatin1String("text:line-break")) {
                out += QLatin1Char('\n');
            }
        } else if (reader.isEndElement()) {
            const QString name = reader.qualifiedName().toString();
            if (textElements.contains(name) && insideText > 0) {
                --insideText;
            }
            if (paragraphElements.contains(name)) {
                out += QLatin1Char('\n');
            }
        } else if (reader.isCharacters() && (insideText > 0 || allTextInside) && !reader.isWhitespace()) {
            out += reader.text();
        } else if (reader.isCharacters() && insideText > 0) {
            out += reader.text();
        }
    }
    return out;
}
#endif

} // namespace

TextSource textSourceFor(const QString &fileName)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix.isEmpty()) {
        return TextSource::None;
    }
    if (plainSuffixes().contains(suffix)) {
        return TextSource::Plain;
    }
    if (suffix == QLatin1String("rtf")) {
        return TextSource::Rtf;
    }
#ifdef EU_HAVE_QTPDF
    if (suffix == QLatin1String("pdf")) {
        return TextSource::Pdf;
    }
#endif
#ifdef EU_HAVE_LIBARCHIVE
    if (suffix == QLatin1String("docx")) {
        return TextSource::Word;
    }
    if (suffix == QLatin1String("odt") || suffix == QLatin1String("ods") || suffix == QLatin1String("odp")) {
        return TextSource::OpenText;
    }
    if (suffix == QLatin1String("xlsx")) {
        return TextSource::Excel;
    }
    if (suffix == QLatin1String("pptx")) {
        return TextSource::PowerPoint;
    }
#endif
    return TextSource::None;
}

core::Result<QString> extractText(const QString &path, const tasks::CancelToken &cancel, const ExtractLimits &limits)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        return core::Error{core::ErrorCode::NotFound, tr("No es un archivo"), path, 0};
    }
    const TextSource source = textSourceFor(path);
    if (source == TextSource::None) {
        return core::Error{core::ErrorCode::NotSupported, tr("No se sabe leer el texto de este tipo"), path, 0};
    }
    if (source == TextSource::Plain) {
        return readPlain(path, limits);
    }
    if (info.size() > limits.maxFileBytes) {
        return tooBig(path);
    }
    if (cancel.isCancelled()) {
        return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), path, 0};
    }
    QString text;
    switch (source) {
    case TextSource::Rtf: {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return core::Error{core::ErrorCode::PermissionDenied, file.errorString(), path, 0};
        }
        text = rtfToText(file.readAll());
        break;
    }
    case TextSource::Pdf: {
#ifdef EU_HAVE_QTPDF
        QPdfDocument document;
        if (document.load(path) != QPdfDocument::Error::None) {
            return core::Error{core::ErrorCode::Corrupted, tr("No se pudo abrir el PDF"), path, 0};
        }
        for (int page = 0; page < document.pageCount() && text.size() < limits.maxTextChars; ++page) {
            if (cancel.isCancelled()) {
                return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), path, 0};
            }
            QString pageText = document.getAllText(page).text();
            // PDFium marca con U+FFFE los guiones de fin de línea y algunos PDF
            // traen guiones blandos (U+00AD): se quitan para que las palabras
            // partidas se encuentren enteras ("afron-tamiento").
            pageText.remove(QChar(0xFFFE));
            pageText.remove(QChar(0x00AD));
            text += pageText;
            text += QLatin1Char('\n');
        }
#endif
        break;
    }
#ifdef EU_HAVE_LIBARCHIVE
    case TextSource::Word: {
        const QList<QByteArray> parts = readZipEntries(
            path, QRegularExpression(QStringLiteral("^word/(document|footnotes|endnotes)\\.xml$")),
            limits.maxTextChars * 4, cancel);
        for (const QByteArray &part : parts) {
            text += xmlText(part, {QStringLiteral("w:t")}, {QStringLiteral("w:p")}, {QStringLiteral("w:tab")}, false);
        }
        break;
    }
    case TextSource::OpenText: {
        const QList<QByteArray> parts = readZipEntries(path, QRegularExpression(QStringLiteral("^content\\.xml$")),
                                                       limits.maxTextChars * 4, cancel);
        for (const QByteArray &part : parts) {
            text += xmlText(part, {QStringLiteral("text:p"), QStringLiteral("text:h")},
                            {QStringLiteral("text:p"), QStringLiteral("text:h")}, {QStringLiteral("text:tab")}, false);
        }
        break;
    }
    case TextSource::Excel: {
        // Los textos de las celdas están en sharedStrings; los escritos
        // directamente en la celda, en cada hoja.
        const QList<QByteArray> parts = readZipEntries(
            path, QRegularExpression(QStringLiteral("^xl/(sharedStrings|worksheets/sheet\\d+)\\.xml$")),
            limits.maxTextChars * 4, cancel);
        for (const QByteArray &part : parts) {
            text += xmlText(part, {QStringLiteral("t")}, {QStringLiteral("si"), QStringLiteral("is")}, {}, false);
        }
        break;
    }
    case TextSource::PowerPoint: {
        const QList<QByteArray> parts = readZipEntries(
            path, QRegularExpression(QStringLiteral("^ppt/slides/slide\\d+\\.xml$")), limits.maxTextChars * 4, cancel);
        for (const QByteArray &part : parts) {
            text += xmlText(part, {QStringLiteral("a:t")}, {QStringLiteral("a:p")}, {}, false);
        }
        break;
    }
#endif
    default:
        return core::Error{core::ErrorCode::NotSupported, tr("No se sabe leer el texto de este tipo"), path, 0};
    }
    if (cancel.isCancelled()) {
        return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), path, 0};
    }
    if (text.size() > limits.maxTextChars) {
        text.truncate(qsizetype(limits.maxTextChars));
    }
    return text;
}

Snippet snippetAround(const QString &text, const QString &needle, Qt::CaseSensitivity sensitivity, int context)
{
    Snippet snippet;
    if (needle.isEmpty()) {
        return snippet;
    }
    const qsizetype at = text.indexOf(needle, 0, sensitivity);
    if (at < 0) {
        return snippet;
    }
    qsizetype from = std::max<qsizetype>(0, at - context);
    qsizetype to = std::min<qsizetype>(text.size(), at + needle.size() + context);
    // No cortar palabras por la mitad si se puede evitar.
    while (from > 0 && from > at - context - 15 && !text[from - 1].isSpace()) {
        --from;
    }
    while (to < text.size() && to < at + needle.size() + context + 15 && !text[to].isSpace()) {
        ++to;
    }
    QString piece = text.mid(from, to - from);
    // Los saltos de línea y tabuladores, como espacios: el fragmento va en una línea.
    for (QChar &character : piece) {
        if (character == QLatin1Char('\n') || character == QLatin1Char('\r') || character == QLatin1Char('\t')) {
            character = QLatin1Char(' ');
        }
    }
    const QString prefix = from > 0 ? QStringLiteral("…") : QString();
    snippet.text = prefix + piece + (to < text.size() ? QStringLiteral("…") : QString());
    snippet.start = int(prefix.size() + (at - from));
    snippet.length = int(needle.size());
    return snippet;
}

} // namespace eu::content
