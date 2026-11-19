// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/content/DocumentPreview.h"
#include "eu/content/TextExtractor.h"

#include "../support/TestMedia.h"

#include <QBuffer>
#include <QDir>
#include <QGuiApplication>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QTest>

using namespace eu;
using namespace eu::content;
using namespace eu::testsupport;

namespace {

void writeBytes(const QString &path, const QByteArray &data)
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(data);
}

QString textOf(const QString &path)
{
    const auto text = extractText(path, {});
    return text.ok() ? text.value() : QStringLiteral("ERROR: ") + text.error().message;
}

} // namespace

class TestContent : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString file(const QString &name) const { return m_dir.filePath(name); }

private slots:
    void plainTextInEveryEncoding()
    {
        writeBytes(file(QStringLiteral("notas.txt")), QStringLiteral("Presupuesto anual: ñandú\n").toUtf8());
        QCOMPARE(textOf(file(QStringLiteral("notas.txt"))), QStringLiteral("Presupuesto anual: ñandú\n"));
        writeBytes(file(QStringLiteral("viejo.txt")), QByteArray("Se\xf1or"));   // Latin-1
        QCOMPARE(textOf(file(QStringLiteral("viejo.txt"))), QStringLiteral("Señor"));
        writeBytes(file(QStringLiteral("falso.txt")), QByteArray("ab\0\0cd", 6));
        QVERIFY(!extractText(file(QStringLiteral("falso.txt")), {}).ok());
        QCOMPARE(textSourceFor(QStringLiteral("x.exe")), TextSource::None);
        QVERIFY(!extractText(file(QStringLiteral("no-existe.txt")), {}).ok());
    }

    void rtfWithoutItsCodes()
    {
        writeBytes(file(QStringLiteral("carta.rtf")),
                   "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Times New Roman;}}{\\colortbl;\\red0\\green0\\blue0;}"
                   "{\\*\\generator Prueba;}\\f0\\fs24 Estimado se\\'f1or:\\par Le escribo por el "
                   "{\\b presupuesto} anual.\\par Caf\\u233?\\tab listo\\par}");
        const QString text = textOf(file(QStringLiteral("carta.rtf")));
        QVERIFY2(text.contains(QStringLiteral("Estimado señor:")), qPrintable(text));
        QVERIFY(text.contains(QStringLiteral("Le escribo por el presupuesto anual.")));
        QVERIFY(text.contains(QStringLiteral("Café\tlisto")));
        QVERIFY(!text.contains(QStringLiteral("Times")));      // tabla de fuentes fuera
        QVERIFY(!text.contains(QStringLiteral("Prueba")));     // grupo {\* …} fuera
        QVERIFY(!text.contains(QLatin1Char('\\')));
    }

    void officeDocuments()
    {
#ifndef EU_HAVE_LIBARCHIVE
        QSKIP("Compilado sin libarchive.");
#else
        QVERIFY(writeZip(file(QStringLiteral("informe.docx")), {
            {QStringLiteral("word/document.xml"),
             "<?xml version=\"1.0\"?><w:document xmlns:w=\"w\"><w:body>"
             "<w:p><w:r><w:t>Primer párrafo con el </w:t></w:r><w:r><w:t>presupuesto anual</w:t></w:r></w:p>"
             "<w:p><w:r><w:t>Segundo</w:t></w:r><w:r><w:tab/><w:t>tabulado</w:t></w:r></w:p>"
             "</w:body></w:document>"}}));
        QString text = textOf(file(QStringLiteral("informe.docx")));
        QVERIFY2(text.contains(QStringLiteral("Primer párrafo con el presupuesto anual\n")), qPrintable(text));
        QVERIFY(text.contains(QStringLiteral("Segundo\ttabulado")));

        QVERIFY(writeZip(file(QStringLiteral("acta.odt")), {
            {QStringLiteral("mimetype"), "application/vnd.oasis.opendocument.text"},
            {QStringLiteral("content.xml"),
             "<?xml version=\"1.0\"?><office:document-content xmlns:office=\"o\" xmlns:text=\"t\"><office:body>"
             "<office:text><text:h>Acta de la reunión</text:h><text:p>Se aprobó el<text:s text:c=\"2\"/>"
             "<text:span>presupuesto</text:span> anual.</text:p></office:text></office:body>"
             "</office:document-content>"}}));
        text = textOf(file(QStringLiteral("acta.odt")));
        QVERIFY2(text.contains(QStringLiteral("Acta de la reunión\n")), qPrintable(text));
        QVERIFY(text.contains(QStringLiteral("Se aprobó el  presupuesto anual.")));

        QVERIFY(writeZip(file(QStringLiteral("cuentas.xlsx")), {
            {QStringLiteral("xl/sharedStrings.xml"),
             "<?xml version=\"1.0\"?><sst><si><t>Concepto</t></si><si><t>Presupuesto anual</t></si></sst>"},
            {QStringLiteral("xl/worksheets/sheet1.xml"),
             "<?xml version=\"1.0\"?><worksheet><sheetData><row><c t=\"inlineStr\"><is><t>Nota en celda</t></is></c>"
             "</row></sheetData></worksheet>"}}));
        text = textOf(file(QStringLiteral("cuentas.xlsx")));
        QVERIFY2(text.contains(QStringLiteral("Presupuesto anual")), qPrintable(text));
        QVERIFY(text.contains(QStringLiteral("Nota en celda")));

        QVERIFY(writeZip(file(QStringLiteral("charla.pptx")), {
            {QStringLiteral("ppt/slides/slide10.xml"),
             "<?xml version=\"1.0\"?><p:sld xmlns:a=\"a\" xmlns:p=\"p\"><a:p><a:r><a:t>Diapositiva diez</a:t></a:r></a:p></p:sld>"},
            {QStringLiteral("ppt/slides/slide2.xml"),
             "<?xml version=\"1.0\"?><p:sld xmlns:a=\"a\" xmlns:p=\"p\"><a:p><a:r><a:t>Diapositiva dos</a:t></a:r></a:p></p:sld>"}}));
        text = textOf(file(QStringLiteral("charla.pptx")));
        // En el orden de las diapositivas (2 antes que 10), no en el del nombre.
        QVERIFY2(text.indexOf(QStringLiteral("dos")) < text.indexOf(QStringLiteral("diez")), qPrintable(text));
        QCOMPARE(snippetAround(text, QStringLiteral("diez"), Qt::CaseInsensitive).page, 2);

        // Un documento roto: error, sin colgarse.
        writeBytes(file(QStringLiteral("roto.docx")), QByteArray(500, 'x'));
        const auto broken = extractText(file(QStringLiteral("roto.docx")), {});
        QVERIFY(!broken.ok() || broken.value().isEmpty());
#endif
    }

    void documentPreviews()
    {
#ifndef EU_HAVE_LIBARCHIVE
        QSKIP("Compilado sin libarchive.");
#else
        QVERIFY(hasDocumentPreview(QStringLiteral("a.DOCX")));
        QVERIFY(hasDocumentPreview(QStringLiteral("a.xlsx")));
        QVERIFY(!hasDocumentPreview(QStringLiteral("a.doc")));   // el formato viejo solo con LibreOffice
        QVERIFY(!hasDocumentPreview(QStringLiteral("a.pdf")));

        // Word: títulos (por el estilo de styles.xml), formato, listas, tablas
        // con celdas combinadas, salto de página; sin lo borrado ni la copia
        // de reserva de un dibujo; el texto, escapado.
        QVERIFY(writeZip(file(QStringLiteral("informe.docx")), {
            {QStringLiteral("word/styles.xml"),
             "<?xml version=\"1.0\"?><w:styles xmlns:w=\"w\">"
             "<w:style w:type=\"paragraph\" w:styleId=\"Ttulo1\"><w:name w:val=\"heading 1\"/></w:style>"
             "<w:style w:type=\"paragraph\" w:styleId=\"MiTitulo\"><w:name w:val=\"Mi título\"/>"
             "<w:basedOn w:val=\"Ttulo1\"/></w:style></w:styles>"},
            {QStringLiteral("word/document.xml"),
             "<?xml version=\"1.0\"?><w:document xmlns:w=\"w\" xmlns:mc=\"mc\"><w:body>"
             "<w:p><w:pPr><w:pStyle w:val=\"Ttulo1\"/></w:pPr><w:r><w:t>Informe anual</w:t></w:r></w:p>"
             "<w:p><w:pPr><w:pStyle w:val=\"MiTitulo\"/></w:pPr><w:r><w:t>Derivado</w:t></w:r></w:p>"
             "<w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:r><w:rPr><w:b/></w:rPr><w:t>negrita</w:t></w:r>"
             "<w:r><w:rPr><w:b w:val=\"0\"/><w:i/></w:rPr><w:t xml:space=\"preserve\"> y cursiva</w:t></w:r>"
             "<w:r><w:t>&lt;script&gt;</w:t></w:r></w:p>"
             "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"1\"/><w:numId w:val=\"3\"/></w:numPr></w:pPr>"
             "<w:r><w:t>elemento</w:t></w:r></w:p>"
             "<w:p><w:r><w:t>antes</w:t></w:r><w:del><w:r><w:delText>borrado</w:delText></w:r></w:del>"
             "<w:r><w:br w:type=\"page\"/><w:t>después</w:t></w:r></w:p>"
             "<w:tbl><w:tr><w:tc><w:tcPr><w:gridSpan w:val=\"2\"/></w:tcPr><w:p><w:r><w:t>ancha</w:t></w:r></w:p>"
             "</w:tc></w:tr><w:tr><w:tc><w:p><w:r><w:t>uno</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>dos</w:t>"
             "</w:r></w:p></w:tc></w:tr></w:tbl>"
             "<w:p><w:r><mc:AlternateContent><mc:Choice><w:txbxContent><w:p><w:r><w:t>cuadro</w:t></w:r></w:p>"
             "</w:txbxContent></mc:Choice><mc:Fallback><w:txbxContent><w:p><w:r><w:t>cuadro</w:t></w:r></w:p>"
             "</w:txbxContent></mc:Fallback></mc:AlternateContent></w:r></w:p>"
             "</w:body></w:document>"}}));
        auto preview = documentPreview(file(QStringLiteral("informe.docx")), {});
        QVERIFY2(preview.ok(), qPrintable(preview.ok() ? QString() : preview.error().message));
        QString html = preview.value().html;
        QVERIFY(!preview.value().isSpreadsheet());
        QVERIFY(!preview.value().truncated);
        QVERIFY2(html.contains(QStringLiteral("<h2>Informe anual</h2>")), qPrintable(html));
        QVERIFY(html.contains(QStringLiteral("<h2>Derivado</h2>")));   // basado en «heading 1»
        QVERIFY(html.contains(QStringLiteral("<p align=\"center\"><b>negrita</b><i> y cursiva</i>&lt;script&gt;</p>")));
        QVERIFY(html.contains(QStringLiteral("margin-left: 40px\">•&nbsp;elemento")));
        QVERIFY(!html.contains(QStringLiteral("borrado")));
        QVERIFY(html.contains(QStringLiteral("antes</p><hr/><p>después")));
        QVERIFY(html.contains(QStringLiteral("<td valign=\"top\" colspan=\"2\"><p>ancha</p></td>")));
        QCOMPARE(html.count(QStringLiteral("cuadro")), 1);
        QVERIFY(!html.contains(QStringLiteral("<script")));

        // Imágenes: la incrustada se ve (reducida al tamaño del dibujo); la
        // enlazada desde fuera nunca se carga; la que no se sabe leer, «[imagen]».
        QByteArray png;
        {
            QImage picture(1200, 600, QImage::Format_RGB32);
            picture.fill(Qt::darkGreen);
            QBuffer buffer(&png);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QVERIFY(picture.save(&buffer, "PNG"));
        }
        QVERIFY(writeZip(file(QStringLiteral("fotos.docx")), {
            {QStringLiteral("word/_rels/document.xml.rels"),
             "<?xml version=\"1.0\"?><Relationships>"
             "<Relationship Id=\"rId1\" Target=\"media/image1.png\"/>"
             "<Relationship Id=\"rId2\" Target=\"http://ejemplo.invalid/espia.png\" TargetMode=\"External\"/>"
             "<Relationship Id=\"rId3\" Target=\"media/image2.emf\"/></Relationships>"},
            {QStringLiteral("word/media/image1.png"), png},
            {QStringLiteral("word/media/image2.emf"), QByteArray(300, '\x01')},
            {QStringLiteral("word/document.xml"),
             "<?xml version=\"1.0\"?><w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:r=\"r\"><w:body>"
             "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"1905000\" cy=\"952500\"/><a:graphic><a:graphicData>"
             "<a:blip r:embed=\"rId1\"/></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>"
             "<w:p><w:r><w:drawing><a:blip r:embed=\"rId2\"/></w:drawing></w:r>"
             "<w:r><w:drawing><a:blip r:embed=\"rId3\"/></w:drawing></w:r></w:p>"
             "</w:body></w:document>"}}));
        preview = documentPreview(file(QStringLiteral("fotos.docx")), {});
        QVERIFY(preview.ok());
        QCOMPARE(preview.value().images.size(), 1);
        QVERIFY2(preview.value().html.contains(QStringLiteral("<img src=\"eu-imagen:0\" width=\"200\" height=\"100\"/>")),
                 qPrintable(preview.value().html));
        QCOMPARE(preview.value().html.count(QStringLiteral("[imagen]")), 2);
        const QImage reduced = QImage::fromData(preview.value().images.at(0));
        QCOMPARE(reduced.size(), QSize(400, 200));   // el doble del tamaño con que se ve, no 1200
        const DocumentPreview copied = documentPreviewFromJson(documentPreviewToJson(preview.value()));
        QCOMPARE(copied.images, preview.value().images);

        // Los topes: el texto se corta y se dice.
        PreviewLimits small;
        small.maxChars = 10;
        preview = documentPreview(file(QStringLiteral("informe.docx")), {}, small);
        QVERIFY(preview.ok());
        QVERIFY(preview.value().truncated);
        QVERIFY(preview.value().html.contains(QStringLiteral("solo se muestra el principio")));
        QVERIFY(!preview.value().html.contains(QStringLiteral("elemento")));

        // Excel: hojas visibles en su orden, textos compartidos (sin la guía
        // fonética), números, fechas, horas, porcentajes, verdadero/falso;
        // filas vacías fuera; columnas por su letra.
        QLocale::setDefault(QLocale(QLocale::Spanish, QLocale::Mexico));
        QVERIFY(writeZip(file(QStringLiteral("cuentas.xlsx")), {
            {QStringLiteral("xl/workbook.xml"),
             "<?xml version=\"1.0\"?><workbook xmlns:r=\"r\"><workbookPr/><sheets>"
             "<sheet name=\"Resumen\" sheetId=\"1\" r:id=\"rId2\"/>"
             "<sheet name=\"Secreta\" sheetId=\"2\" state=\"hidden\" r:id=\"rId1\"/>"
             "<sheet name=\"Datos &amp; más\" sheetId=\"3\" r:id=\"rId1\"/></sheets></workbook>"},
            {QStringLiteral("xl/_rels/workbook.xml.rels"),
             "<?xml version=\"1.0\"?><Relationships>"
             "<Relationship Id=\"rId1\" Target=\"worksheets/sheet1.xml\"/>"
             "<Relationship Id=\"rId2\" Target=\"/xl/worksheets/sheet2.xml\"/></Relationships>"},
            {QStringLiteral("xl/sharedStrings.xml"),
             "<?xml version=\"1.0\"?><sst><si><t>Concepto</t></si>"
             "<si><r><t>Pre</t></r><r><t>supuesto</t></r><rPh><t>fonética</t></rPh></si></sst>"},
            {QStringLiteral("xl/styles.xml"),
             "<?xml version=\"1.0\"?><styleSheet><numFmts><numFmt numFmtId=\"164\" formatCode=\"0.0%\"/>"
             "<numFmt numFmtId=\"165\" formatCode=\"[$-es-MX]dd/mm/yyyy\\ hh:mm\"/></numFmts>"
             "<cellStyleXfs><xf numFmtId=\"14\"/></cellStyleXfs>"
             "<cellXfs><xf numFmtId=\"0\"/><xf numFmtId=\"14\"/><xf numFmtId=\"164\"/><xf numFmtId=\"20\"/>"
             "<xf numFmtId=\"165\"/></cellXfs></styleSheet>"},
            {QStringLiteral("xl/worksheets/sheet1.xml"),
             "<?xml version=\"1.0\"?><worksheet><sheetData><row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>Hoja de datos</t>"
             "</is></c></row></sheetData></worksheet>"},
            {QStringLiteral("xl/worksheets/sheet2.xml"),
             "<?xml version=\"1.0\"?><worksheet><dimension ref=\"A1:F9\"/><sheetData>"
             "<row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c><c r=\"C1\" t=\"s\"><v>1</v></c></row>"
             "<row r=\"2\"><c r=\"A2\"><v>1234.5</v></c><c r=\"B2\" s=\"1\"><v>45000</v></c>"
             "<c r=\"C2\" s=\"2\"><v>0.25</v></c><c r=\"D2\" s=\"3\"><v>0.5</v></c>"
             "<c r=\"E2\" t=\"b\"><v>1</v></c><c r=\"F2\" t=\"str\"><f>A1</f><v>Concepto</v></c></row>"
             "<row r=\"3\" s=\"1\" customFormat=\"1\"/>"
             "<row r=\"9\"><c r=\"B9\" s=\"4\"><v>45000.75</v></c><c r=\"A9\" t=\"e\"><v>#DIV/0!</v></c></row>"
             "</sheetData></worksheet>"}}));
        preview = documentPreview(file(QStringLiteral("cuentas.xlsx")), {});
        QVERIFY2(preview.ok(), qPrintable(preview.ok() ? QString() : preview.error().message));
        QVERIFY(preview.value().isSpreadsheet());
        const QList<SheetPreview> &sheets = preview.value().sheets;
        QCOMPARE(sheets.size(), 2);
        QCOMPARE(sheets.at(0).name, QStringLiteral("Resumen"));
        QCOMPARE(sheets.at(1).name, QStringLiteral("Datos & más"));
        const SheetPreview &summary = sheets.at(0);
        QCOMPARE(summary.rowNumbers, (QList<int>{1, 2, 9}));   // la 3 solo tenía formato
        QCOMPARE(summary.columns, 6);
        QCOMPARE(summary.totalRows, 9);
        QCOMPARE(summary.cells.at(0), (QStringList{QStringLiteral("Concepto"), QString(), QStringLiteral("Presupuesto")}));
        QCOMPARE(summary.cells.at(1),
                 (QStringList{QStringLiteral("1234.5"), QStringLiteral("15/03/2023"), QStringLiteral("25%"),
                              QStringLiteral("12:00"), QStringLiteral("VERDADERO"), QStringLiteral("Concepto")}));
        QCOMPARE(summary.numeric.at(1), QByteArray("111100"));
        QCOMPARE(summary.cells.at(2), (QStringList{QStringLiteral("#DIV/0!"), QStringLiteral("15/03/2023 18:00")}));
        QCOMPARE(sheets.at(1).cells.at(0).at(0), QStringLiteral("Hoja de datos"));
        QCOMPARE(spreadsheetColumnName(1), QStringLiteral("A"));
        QCOMPARE(spreadsheetColumnName(28), QStringLiteral("AB"));

        // Topes de filas, columnas y hojas.
        small = PreviewLimits();
        small.maxRows = 2;
        small.maxColumns = 3;
        small.maxSheets = 1;
        preview = documentPreview(file(QStringLiteral("cuentas.xlsx")), {}, small);
        QVERIFY(preview.ok());
        QVERIFY(preview.value().truncated);
        QCOMPARE(preview.value().sheets.size(), 1);
        QCOMPARE(preview.value().moreSheets, 1);
        QCOMPARE(preview.value().sheets.at(0).rowNumbers, (QList<int>{1, 2}));
        QVERIFY(preview.value().sheets.at(0).moreRows);
        QVERIFY(preview.value().sheets.at(0).moreColumns);
        QCOMPARE(preview.value().sheets.at(0).columns, 3);

        // Ida y vuelta por JSON (lo que pasa por el proceso auxiliar).
        const DocumentPreview back = documentPreviewFromJson(documentPreviewToJson(preview.value()));
        QCOMPARE(back.sheets.at(0).cells, preview.value().sheets.at(0).cells);
        QCOMPARE(back.sheets.at(0).numeric, preview.value().sheets.at(0).numeric);
        QCOMPARE(back.sheets.at(0).rowNumbers, preview.value().sheets.at(0).rowNumbers);
        QCOMPARE(back.moreSheets, 1);

        // OpenDocument: texto con estilos automáticos y hoja con celdas repetidas.
        QVERIFY(writeZip(file(QStringLiteral("acta.odt")), {
            {QStringLiteral("content.xml"),
             "<?xml version=\"1.0\"?><office:document-content xmlns:office=\"o\" xmlns:text=\"t\" xmlns:style=\"s\" "
             "xmlns:fo=\"f\" xmlns:table=\"tb\"><office:automatic-styles>"
             "<style:style style:name=\"T1\" style:family=\"text\"><style:text-properties fo:font-weight=\"bold\"/>"
             "</style:style><style:style style:name=\"P1\" style:family=\"paragraph\">"
             "<style:paragraph-properties fo:text-align=\"center\"/></style:style></office:automatic-styles>"
             "<office:body><office:text><text:h text:outline-level=\"1\">Acta</text:h>"
             "<text:p text:style-name=\"P1\">Se aprobó el <text:span text:style-name=\"T1\">presupuesto</text:span>"
             "<office:annotation><text:p>nota oculta</text:p></office:annotation></text:p>"
             "<text:list><text:list-item><text:p>punto uno</text:p></text:list-item></text:list>"
             "</office:text></office:body></office:document-content>"}}));
        preview = documentPreview(file(QStringLiteral("acta.odt")), {});
        QVERIFY(preview.ok());
        html = preview.value().html;
        QVERIFY2(html.contains(QStringLiteral("<h2>Acta</h2>")), qPrintable(html));
        QVERIFY(html.contains(QStringLiteral("<p align=\"center\">Se aprobó el <b>presupuesto</b></p>")));
        QVERIFY(html.contains(QStringLiteral("•&nbsp;punto uno")));
        QVERIFY(!html.contains(QStringLiteral("nota oculta")));

        QVERIFY(writeZip(file(QStringLiteral("cuentas.ods")), {
            {QStringLiteral("content.xml"),
             "<?xml version=\"1.0\"?><office:document-content xmlns:office=\"o\" xmlns:text=\"t\" xmlns:table=\"tb\">"
             "<office:body><office:spreadsheet><table:table table:name=\"Gastos\">"
             "<table:table-row><table:table-cell office:value-type=\"string\"><text:p>Luz</text:p></table:table-cell>"
             "<table:table-cell table:number-columns-repeated=\"2\"/>"
             "<table:table-cell office:value-type=\"float\" office:value=\"30\"><text:p>30,00 $</text:p>"
             "</table:table-cell><table:table-cell table:number-columns-repeated=\"16380\"/></table:table-row>"
             "<table:table-row table:number-rows-repeated=\"1048575\"><table:table-cell "
             "table:number-columns-repeated=\"16384\"/></table:table-row>"
             "</table:table></office:spreadsheet></office:body></office:document-content>"}}));
        preview = documentPreview(file(QStringLiteral("cuentas.ods")), {});
        QVERIFY(preview.ok());
        QCOMPARE(preview.value().sheets.size(), 1);
        QCOMPARE(preview.value().sheets.at(0).name, QStringLiteral("Gastos"));
        QCOMPARE(preview.value().sheets.at(0).rowNumbers, (QList<int>{1}));
        QCOMPARE(preview.value().sheets.at(0).cells.at(0),
                 (QStringList{QStringLiteral("Luz"), QString(), QString(), QStringLiteral("30,00 $")}));
        QCOMPARE(preview.value().sheets.at(0).numeric.at(0), QByteArray("0001"));
        QVERIFY(!preview.value().truncated);

        // Un documento roto o que no es un ZIP: error, sin colgarse.
        writeBytes(file(QStringLiteral("roto.xlsx")), QByteArray(500, 'x'));
        QVERIFY(!documentPreview(file(QStringLiteral("roto.xlsx")), {}).ok());
        QVERIFY(writeZip(file(QStringLiteral("vacio.docx")), {{QStringLiteral("otra.xml"), "<a/>"}}));
        QVERIFY(!documentPreview(file(QStringLiteral("vacio.docx")), {}).ok());
#endif
    }

    void pdfText()
    {
        if (textSourceFor(QStringLiteral("x.pdf")) != TextSource::Pdf) {
            QSKIP("Compilado sin Qt PDF.");
        }
        const QString path = file(QStringLiteral("libro.pdf"));
        {
            QPdfWriter writer(path);
            QPainter painter(&writer);
            QFont font = painter.font();
            font.setPointSize(14);
            painter.setFont(font);
            painter.drawText(QPointF(200, 400), QStringLiteral("El presupuesto anual"));
            writer.newPage();
            painter.drawText(QPointF(200, 400), QStringLiteral("Segunda página"));
        }
        const QString text = textOf(path);
        QVERIFY2(text.contains(QStringLiteral("presupuesto anual")), qPrintable(text));
        QVERIFY(text.contains(QStringLiteral("Segunda")));
        // Las páginas van separadas: se sabe en cuál está cada cosa.
        QCOMPARE(snippetAround(text, QStringLiteral("presupuesto"), Qt::CaseInsensitive).page, 1);
        QCOMPARE(snippetAround(text, QStringLiteral("Segunda"), Qt::CaseInsensitive).page, 2);
    }

    void limitsAreRespected()
    {
        writeBytes(file(QStringLiteral("largo.log")), QByteArray(5000, 'a'));
        ExtractLimits limits;
        limits.maxTextChars = 1000;
        const auto text = extractText(file(QStringLiteral("largo.log")), {}, limits);
        QVERIFY(text.ok());
        QCOMPARE(text.value().size(), 1000);
        writeBytes(file(QStringLiteral("grande.rtf")), QByteArray(5000, 'r'));
        limits.maxFileBytes = 1000;
        QVERIFY(!extractText(file(QStringLiteral("grande.rtf")), {}, limits).ok());
        tasks::CancelSource source;
        source.cancel();
        QVERIFY(!extractText(file(QStringLiteral("grande.rtf")), source.token()).ok());
    }

    void snippetsShowTheMatch()
    {
        const QString text = QStringLiteral("Primera línea.\nEn esta reunión se aprobó el Presupuesto Anual de 2026 "
                                            "con algunos cambios menores que se verán después.");
        const Snippet snippet = snippetAround(text, QStringLiteral("presupuesto anual"), Qt::CaseInsensitive, 20);
        QVERIFY(!snippet.text.isEmpty());
        QCOMPARE(snippet.text.mid(snippet.start, snippet.length), QStringLiteral("Presupuesto Anual"));
        QVERIFY(snippet.text.startsWith(QStringLiteral("…")));
        QVERIFY(!snippet.text.contains(QLatin1Char('\n')));
        QCOMPARE(snippet.line, 2);
        QCOMPARE(snippet.page, 0);   // texto sin páginas
        QVERIFY(snippetAround(text, QStringLiteral("presupuesto anual"), Qt::CaseSensitive).text.isEmpty());
        // Los espacios seguidos y los saltos se juntan en uno; la posición
        // de la coincidencia sigue siendo la buena.
        const Snippet spaced = snippetAround(QStringLiteral("una   experiencia\n\n extática,  de felicidad  inmensa"),
                                             QStringLiteral("felicidad"), Qt::CaseInsensitive, 60);
        QCOMPARE(spaced.text, QStringLiteral("una experiencia extática, de felicidad inmensa"));
        QCOMPARE(spaced.text.mid(spaced.start, spaced.length), QStringLiteral("felicidad"));
        const Snippet cut = snippetAround(QStringLiteral("una   experiencia\n\n extática,  de felicidad"),
                                          QStringLiteral("felicidad"), Qt::CaseInsensitive, 30);
        QCOMPARE(cut.text, QStringLiteral("…experiencia extática, de felicidad"));
        QVERIFY(snippetAround(text, QStringLiteral("no está"), Qt::CaseInsensitive).text.isEmpty());
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);
    TestContent test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "test_content.moc"
