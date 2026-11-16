// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/content/TextExtractor.h"

#include "../support/TestMedia.h"

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

        // Un documento roto: error, sin colgarse.
        writeBytes(file(QStringLiteral("roto.docx")), QByteArray(500, 'x'));
        const auto broken = extractText(file(QStringLiteral("roto.docx")), {});
        QVERIFY(!broken.ok() || broken.value().isEmpty());
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
        QVERIFY(snippetAround(text, QStringLiteral("presupuesto anual"), Qt::CaseSensitive).text.isEmpty());
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
