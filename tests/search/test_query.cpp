// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/search/Query.h"

#include <QDate>
#include <QDateTime>
#include <QElapsedTimer>
#include <QTest>

#include <vector>

using namespace eu;
using namespace eu::search;

namespace {

qint64 dateMs(int year, int month, int day, int hour = 12)
{
    return QDateTime(QDate(year, month, day), QTime(hour, 0)).toMSecsSinceEpoch();
}

/// Un archivo de prueba con solo el nombre.
Item file(const QString &name)
{
    Item item;
    item.name = name;
    return item;
}

Query parsed(const QString &text, bool caseSensitive = false)
{
    QueryOptions options;
    options.caseSensitive = caseSensitive;
    options.nowMs = dateMs(2026, 11, 12);
    const core::Result<Query> result = Query::parse(text, options);
    if (!result.ok()) {
        qWarning("La consulta «%s» no se pudo analizar: %s", qPrintable(text), qPrintable(result.error().message));
        return Query::parse(QStringLiteral("regex:/$^imposible/")).value();
    }
    return result.value();
}

bool match(const QString &query, const QString &name)
{
    const QString copy = name;   // el Item guarda una vista: la cadena debe seguir viva
    return parsed(query).matches(file(copy));
}

} // namespace

class TestQuery : public QObject
{
    Q_OBJECT

private slots:
    void emptyQueryMatchesEverything()
    {
        QVERIFY(parsed(QString()).isEmpty());
        QVERIFY(parsed(QStringLiteral("   ")).isEmpty());
        QVERIFY(match(QString(), QStringLiteral("cualquiera.txt")));
    }

    void plainWordsAreSubstrings()
    {
        QVERIFY(match(QStringLiteral("factura"), QStringLiteral("mi Factura de marzo.pdf")));
        QVERIFY(!match(QStringLiteral("factura"), QStringLiteral("recibo.pdf")));
        // Con distinción de mayúsculas activada.
        QVERIFY(!parsed(QStringLiteral("factura"), true).matches(file(QStringLiteral("Factura.pdf"))));
        QVERIFY(parsed(QStringLiteral("Factura"), true).matches(file(QStringLiteral("Factura.pdf"))));
    }

    void wildcards_data()
    {
        QTest::addColumn<QString>("query");
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("expected");
        // Los ejemplos de la especificación (§6.1).
        QTest::newRow("fot*.jpg sí") << "fot*.jpg" << "foto de playa.jpg" << true;
        QTest::newRow("fot*.jpg mayúsculas") << "fot*.jpg" << "FOTO1.JPG" << true;
        QTest::newRow("fot*.jpg otra extensión") << "fot*.jpg" << "foto.jpeg" << false;
        QTest::newRow("fot*.jpg no al principio") << "fot*.jpg" << "mi foto.jpg" << false;
        QTest::newRow("informe_202?.pdf sí") << "informe_202?.pdf" << "informe_2025.pdf" << true;
        QTest::newRow("? es exactamente uno") << "informe_202?.pdf" << "informe_202.pdf" << false;
        QTest::newRow("? no son dos") << "informe_202?.pdf" << "informe_20255.pdf" << false;
        QTest::newRow("*factura* en medio") << "*factura*" << "2025 factura luz.pdf" << true;
        QTest::newRow("documento?.docx") << "documento?.docx" << "documento7.docx" << true;
        QTest::newRow("* solo") << "*" << "lo que sea" << true;
        QTest::newRow("varios asteriscos") << "a*b*c" << "aXXbYYc" << true;
        QTest::newRow("vuelta atrás") << "*ab*ab" << "abXabab" << true;
        QTest::newRow("sin coincidencia final") << "a*b*c" << "aXXbYY" << false;
        QTest::newRow("? con letra acentuada") << "cami?n.txt" << "camión.txt" << true;
    }

    void wildcards()
    {
        QFETCH(QString, query);
        QFETCH(QString, name);
        QFETCH(bool, expected);
        QCOMPARE(match(query, name), expected);
    }

    void quotedTextIsLiteral()
    {
        QVERIFY(match(QStringLiteral("\"acta final\""), QStringLiteral("2025 acta final v2.docx")));
        QVERIFY(!match(QStringLiteral("\"acta final\""), QStringLiteral("acta casi final.docx")));
        // Entre comillas, * y ? son caracteres normales, y AND no es un operador.
        QVERIFY(match(QStringLiteral("\"que?\""), QStringLiteral("y que?.txt")));
        QVERIFY(!match(QStringLiteral("\"que?\""), QStringLiteral("y quex.txt")));
        QVERIFY(match(QStringLiteral("\"R AND D\""), QStringLiteral("informe R AND D.pdf")));
        QVERIFY(match(QStringLiteral("\"dijo \\\"hola\\\"\""), QStringLiteral("dijo \"hola\".txt")));
    }

    void operatorsAndPrecedence()
    {
        const QString a = QStringLiteral("alfa.txt");
        const QString ab = QStringLiteral("alfa beta.txt");
        const QString b = QStringLiteral("beta.txt");
        const QString c = QStringLiteral("gamma.txt");

        // Un espacio es AND.
        QVERIFY(match(QStringLiteral("alfa beta"), ab));
        QVERIFY(!match(QStringLiteral("alfa beta"), a));
        QVERIFY(match(QStringLiteral("alfa AND beta"), ab));
        QVERIFY(match(QStringLiteral("alfa OR beta"), b));
        QVERIFY(!match(QStringLiteral("alfa OR beta"), c));
        QVERIFY(match(QStringLiteral("NOT alfa"), b));
        QVERIFY(!match(QStringLiteral("NOT alfa"), a));
        QVERIFY(match(QStringLiteral("-alfa"), b));
        QVERIFY(!match(QStringLiteral("txt -alfa"), ab));

        // NOT > AND > OR:  alfa OR beta AND gamma  ==  alfa OR (beta AND gamma)
        QVERIFY(match(QStringLiteral("alfa OR beta AND gamma"), a));
        QVERIFY(!match(QStringLiteral("alfa OR beta AND gamma"), b));
        // Los paréntesis lo cambian: (alfa OR beta) AND gamma
        QVERIFY(!match(QStringLiteral("(alfa OR beta) AND gamma"), a));
        QVERIFY(match(QStringLiteral("(alfa OR beta) AND gamma"), QStringLiteral("beta gamma.txt")));
        // NOT alfa AND beta  ==  (NOT alfa) AND beta
        QVERIFY(match(QStringLiteral("NOT alfa AND beta"), b));
        QVERIFY(!match(QStringLiteral("NOT alfa AND beta"), ab));
        QVERIFY(match(QStringLiteral("NOT (alfa OR beta)"), c));
        QVERIFY(match(QStringLiteral("NOT NOT alfa"), a));
        QVERIFY(match(QStringLiteral("((alfa))"), a));

        // En minúsculas son palabras corrientes, no operadores.
        QVERIFY(match(QStringLiteral("and"), QStringLiteral("brand.txt")));
        QVERIFY(!match(QStringLiteral("alfa or beta"), a));
        // Un guion dentro de una palabra no es NOT.
        QVERIFY(match(QStringLiteral("informe-final"), QStringLiteral("informe-final.pdf")));
    }

    void extensionFilter()
    {
        QVERIFY(match(QStringLiteral("ext:pdf"), QStringLiteral("Informe.PDF")));
        QVERIFY(match(QStringLiteral("ext:.pdf"), QStringLiteral("informe.pdf")));
        QVERIFY(!match(QStringLiteral("ext:pdf"), QStringLiteral("informe.pdf.txt")));
        QVERIFY(!match(QStringLiteral("ext:pdf"), QStringLiteral("pdf")));
        QVERIFY(match(QStringLiteral("ext:jpg,png"), QStringLiteral("a.png")));
        QVERIFY(!match(QStringLiteral("ext:jpg,png"), QStringLiteral("a.gif")));
        QVERIFY(match(QStringLiteral("ext:tar.gz"), QStringLiteral("copia.tar.gz")));
        // Un archivo oculto sin extensión no "es" esa extensión.
        QVERIFY(!match(QStringLiteral("ext:bashrc"), QStringLiteral(".bashrc")));
        QVERIFY(match(QStringLiteral("informe ext:pdf"), QStringLiteral("informe 2025.pdf")));
        QVERIFY(match(QStringLiteral("EXT:pdf"), QStringLiteral("a.pdf")));
    }

    void sizeFilter()
    {
        const QString name = QStringLiteral("a.bin");
        const auto sized = [&](qint64 bytes) {
            Item item = file(name);
            item.size = bytes;
            return item;
        };
        QVERIFY(parsed(QStringLiteral("size:>10MB")).matches(sized(10'000'001)));
        QVERIFY(!parsed(QStringLiteral("size:>10MB")).matches(sized(10'000'000)));
        QVERIFY(parsed(QStringLiteral("size:>=10MB")).matches(sized(10'000'000)));
        QVERIFY(parsed(QStringLiteral("size:<1kB")).matches(sized(999)));
        QVERIFY(!parsed(QStringLiteral("size:<1kB")).matches(sized(1000)));
        QVERIFY(parsed(QStringLiteral("size:<=1KiB")).matches(sized(1024)));
        QVERIFY(parsed(QStringLiteral("size:=300")).matches(sized(300)));
        QVERIFY(!parsed(QStringLiteral("size:=300")).matches(sized(301)));
        QVERIFY(parsed(QStringLiteral("size:1MB..2MB")).matches(sized(1'500'000)));
        QVERIFY(parsed(QStringLiteral("size:1MB..2MB")).matches(sized(2'000'000)));
        QVERIFY(!parsed(QStringLiteral("size:1MB..2MB")).matches(sized(2'000'001)));
        QVERIFY(parsed(QStringLiteral("size:..1MB")).matches(sized(0)));
        QVERIFY(parsed(QStringLiteral("size:1GB..")).matches(sized(5'000'000'000LL)));
        // Sin comparador: lo que se vería como "10 MB".
        QVERIFY(parsed(QStringLiteral("size:10MB")).matches(sized(10'200'000)));
        QVERIFY(parsed(QStringLiteral("size:10MB")).matches(sized(9'500'000)));
        QVERIFY(!parsed(QStringLiteral("size:10MB")).matches(sized(10'500'000)));
        QVERIFY(parsed(QStringLiteral("size:>1,5GB")).matches(sized(1'600'000'000)));
        QVERIFY(parsed(QStringLiteral("size:>1.5gb")).matches(sized(1'600'000'000)));

        // Las carpetas nunca cumplen un filtro de tamaño.
        Item folder = sized(50'000'000);
        folder.isDirectory = true;
        QVERIFY(!parsed(QStringLiteral("size:>10MB")).matches(folder));

        QCOMPARE(parseSize(u"10MB"), qint64(10'000'000));
        QCOMPARE(parseSize(u"1 GiB"), qint64(1024) * 1024 * 1024);
        QCOMPARE(parseSize(u"42"), qint64(42));
        QCOMPARE(parseSize(u"diez"), qint64(-1));
        QCOMPARE(parseSize(u"10 patatas"), qint64(-1));
    }

    void dateFilters()
    {
        const QString name = QStringLiteral("a.txt");
        const auto modifiedOn = [&](int year, int month, int day, int hour = 12) {
            Item item = file(name);
            item.modifiedMs = dateMs(year, month, day, hour);
            return item;
        };
        // El ejemplo de la especificación: de enero a junio, ambos meses enteros.
        const Query range = parsed(QStringLiteral("modified:2025-01..2025-06"));
        QVERIFY(range.matches(modifiedOn(2025, 1, 1, 0)));
        QVERIFY(range.matches(modifiedOn(2025, 6, 30, 23)));
        QVERIFY(!range.matches(modifiedOn(2025, 7, 1, 0)));
        QVERIFY(!range.matches(modifiedOn(2024, 12, 31, 23)));

        QVERIFY(parsed(QStringLiteral("modified:2025")).matches(modifiedOn(2025, 8, 15)));
        QVERIFY(!parsed(QStringLiteral("modified:2025")).matches(modifiedOn(2026, 1, 1)));
        QVERIFY(parsed(QStringLiteral("modified:2025-03-14")).matches(modifiedOn(2025, 3, 14, 23)));
        QVERIFY(!parsed(QStringLiteral("modified:2025-03-14")).matches(modifiedOn(2025, 3, 15, 0)));
        QVERIFY(parsed(QStringLiteral("modified:>2025")).matches(modifiedOn(2026, 1, 1, 0)));
        QVERIFY(!parsed(QStringLiteral("modified:>2025")).matches(modifiedOn(2025, 12, 31)));
        QVERIFY(parsed(QStringLiteral("modified:>=2025-06")).matches(modifiedOn(2025, 6, 1, 0)));
        QVERIFY(parsed(QStringLiteral("modified:<2025-06")).matches(modifiedOn(2025, 5, 31, 23)));
        QVERIFY(!parsed(QStringLiteral("modified:<2025-06")).matches(modifiedOn(2025, 6, 1, 0)));
        QVERIFY(parsed(QStringLiteral("modified:<=2025-06")).matches(modifiedOn(2025, 6, 30)));
        QVERIFY(parsed(QStringLiteral("modified:2025-06..")).matches(modifiedOn(2030, 1, 1)));
        QVERIFY(parsed(QStringLiteral("modified:..2025-06")).matches(modifiedOn(1999, 1, 1)));

        // "hoy" y "ayer" respecto del momento de la consulta (12-nov-2026 en la prueba).
        QVERIFY(parsed(QStringLiteral("modified:hoy")).matches(modifiedOn(2026, 11, 12, 1)));
        QVERIFY(!parsed(QStringLiteral("modified:hoy")).matches(modifiedOn(2026, 11, 11, 23)));
        QVERIFY(parsed(QStringLiteral("modified:ayer")).matches(modifiedOn(2026, 11, 11, 23)));
        QVERIFY(parsed(QStringLiteral("modified:today")).matches(modifiedOn(2026, 11, 12, 20)));

        // Fecha desconocida: no cumple ningún filtro de fecha.
        QVERIFY(!parsed(QStringLiteral("modified:..2030")).matches(file(name)));

        // created: usa la fecha de creación, no la de modificación.
        Item created = file(name);
        created.createdMs = dateMs(2020, 5, 5);
        created.modifiedMs = dateMs(2025, 5, 5);
        QVERIFY(parsed(QStringLiteral("created:2020")).matches(created));
        QVERIFY(!parsed(QStringLiteral("created:2025")).matches(created));
    }

    void typeAndHiddenFilters()
    {
        QVERIFY(match(QStringLiteral("type:imagen"), QStringLiteral("playa.JPG")));
        QVERIFY(match(QStringLiteral("type:image"), QStringLiteral("playa.png")));
        QVERIFY(!match(QStringLiteral("type:imagen"), QStringLiteral("playa.mp4")));
        QVERIFY(match(QStringLiteral("type:vídeo"), QStringLiteral("playa.mp4")));
        QVERIFY(match(QStringLiteral("type:video"), QStringLiteral("playa.mkv")));
        QVERIFY(match(QStringLiteral("type:música"), QStringLiteral("tema.flac")));
        QVERIFY(match(QStringLiteral("type:documento"), QStringLiteral("acta.docx")));
        QVERIFY(match(QStringLiteral("type:comprimido"), QStringLiteral("copia.7z")));
        QVERIFY(match(QStringLiteral("type:código"), QStringLiteral("main.cpp")));
        QVERIFY(!match(QStringLiteral("type:imagen"), QStringLiteral("sin_extension")));

        const QString name = QStringLiteral("Fotos.jpg");   // una carpeta puede llamarse así
        Item folder = file(name);
        folder.isDirectory = true;
        QVERIFY(parsed(QStringLiteral("type:carpeta")).matches(folder));
        QVERIFY(!parsed(QStringLiteral("type:imagen")).matches(folder));
        QVERIFY(!parsed(QStringLiteral("type:archivo")).matches(folder));
        QVERIFY(parsed(QStringLiteral("type:archivo")).matches(file(name)));

        Item hidden = file(name);
        hidden.isHidden = true;
        QVERIFY(parsed(QStringLiteral("hidden:yes")).matches(hidden));
        QVERIFY(parsed(QStringLiteral("hidden:sí")).matches(hidden));
        QVERIFY(!parsed(QStringLiteral("hidden:no")).matches(hidden));
        QVERIFY(parsed(QStringLiteral("hidden:no")).matches(file(name)));
    }

    void regexFilter()
    {
        QVERIFY(match(QStringLiteral("regex:/^IMG_\\d{4}\\.jpe?g$/"), QStringLiteral("IMG_0042.jpg")));
        QVERIFY(match(QStringLiteral("regex:/^IMG_\\d{4}\\.jpe?g$/"), QStringLiteral("img_0042.JPEG")));
        QVERIFY(!match(QStringLiteral("regex:/^IMG_\\d{4}\\.jpe?g$/"), QStringLiteral("IMG_42.jpg")));
        // El patrón puede llevar espacios, paréntesis y barras escapadas.
        QVERIFY(match(QStringLiteral("regex:/(acta|informe) final/ ext:pdf"), QStringLiteral("Informe final.pdf")));
        QVERIFY(match(QStringLiteral("regex:/a\\/b/"), QStringLiteral("a/b")));
        // Con distinción de mayúsculas, la bandera i la desactiva solo para ese patrón.
        QVERIFY(!parsed(QStringLiteral("regex:/^img/"), true).matches(file(QStringLiteral("IMG_1.jpg"))));
        QVERIFY(parsed(QStringLiteral("regex:/^img/i"), true).matches(file(QStringLiteral("IMG_1.jpg"))));
    }

    void unicodeNormalization()
    {
        // "é" como una sola letra (NFC) y como "e" + acento (NFD, lo que guarda macOS).
        const QString composed = QString::fromUtf8("caf\xC3\xA9.txt");
        const QString decomposed = QString::fromUtf8("cafe\xCC\x81.txt");
        QVERIFY(composed != decomposed);
        for (const QString &queryText : {composed.left(4), decomposed.left(5)}) {
            QVERIFY(parsed(queryText).matches(file(composed)));
            QVERIFY(parsed(queryText).matches(file(decomposed)));
        }
        QVERIFY(parsed(QString::fromUtf8("caf?.txt")).matches(file(decomposed)));
        QVERIFY(parsed(QString::fromUtf8("CAF\xC3\x89")).matches(file(decomposed)));
        QVERIFY(parsed(QString::fromUtf8("regex:/^caf\xC3\xA9\\./")).matches(file(decomposed)));
        // Otras escrituras.
        QVERIFY(match(QString::fromUtf8("\xE6\x97\xA5\xE6\x9C\xAC*"), QString::fromUtf8("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.txt")));
        QVERIFY(match(QString::fromUtf8("\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"),
                      QString::fromUtf8("\xD0\x9F\xD0\xA0\xD0\x98\xD0\x92\xD0\x95\xD0\xA2.doc")));
    }

    void namesWithColonsAreNotFilters()
    {
        // "nota:importante" no es un filtro conocido: se busca tal cual.
        QVERIFY(match(QStringLiteral("nota:importante"), QStringLiteral("nota:importante.txt")));
        QVERIFY(match(QStringLiteral("name:fot*.jpg"), QStringLiteral("foto.jpg")));
    }

    void unavailableFiltersAreReported()
    {
        const Query query = parsed(QStringLiteral("informe tag:rojo"));
        QCOMPARE(query.unavailableFilters(), QStringList{QStringLiteral("tag")});
        // No aparentan funcionar: nada los cumple.
        QVERIFY(!query.matches(file(QStringLiteral("informe.txt"))));
        QVERIFY(parsed(QStringLiteral("informe")).unavailableFilters().isEmpty());
    }

    void severalWordsWithWildcardsAlsoMatchAsAPhrase()
    {
        // El caso que falló al dueño: la carpeta y sus libros.
        QVERIFY(match(QStringLiteral("walter riso*"), QStringLiteral("WALTER RISO")));
        QVERIFY(match(QStringLiteral("WALTER RISO*"), QStringLiteral("Walter Riso - Amar o depender.pdf")));
        QVERIFY(match(QStringLiteral("walter   riso*"), QStringLiteral("Walter Riso.pdf")));
        QVERIFY(!match(QStringLiteral("walter riso*"), QStringLiteral("Walter y Riso.pdf")));
        QVERIFY(!match(QStringLiteral("walter riso*"), QStringLiteral("walter.pdf")));
        // Sin comodines sigue siendo "están las dos palabras", en cualquier orden.
        QVERIFY(match(QStringLiteral("walter riso"), QStringLiteral("Riso, Walter.pdf")));
        // La lectura palabra por palabra sigue valiendo: contiene "factura" y es PDF.
        QVERIFY(match(QStringLiteral("factura *.pdf"), QStringLiteral("mi factura de mayo.pdf")));
        QVERIFY(match(QStringLiteral("*.pdf factura"), QStringLiteral("mi factura de mayo.pdf")));
        QVERIFY(!match(QStringLiteral("factura *.pdf"), QStringLiteral("factura.txt")));
        // Entre comillas es texto literal: el asterisco no es comodín.
        QVERIFY(!match(QStringLiteral("\"walter riso*\""), QStringLiteral("Walter Riso.pdf")));
        // NOT se aplica solo a la palabra que le sigue.
        QVERIFY(match(QStringLiteral("walter -riso*"), QStringLiteral("walter otro.pdf")));
        QVERIFY(match(QStringLiteral("walter -riso*"), QStringLiteral("Walter Riso.pdf")));
        QVERIFY(!match(QStringLiteral("walter -*riso*"), QStringLiteral("Walter Riso.pdf")));
    }

    void caseSensitivityIsAnOption()
    {
        const QString name = QStringLiteral("Walter Riso.pdf");
        QVERIFY(parsed(QStringLiteral("WALTER"), false).matches(file(name)));
        QVERIFY(!parsed(QStringLiteral("WALTER"), true).matches(file(name)));
        QVERIFY(parsed(QStringLiteral("Walter"), true).matches(file(name)));
        QVERIFY(!parsed(QStringLiteral("walter riso*"), true).matches(file(name)));
        QVERIFY(parsed(QStringLiteral("Walter Riso*"), true).matches(file(name)));
    }

    void contentIsAskedOnlyWhenTheRestMatches()
    {
        struct Text : ContentSource
        {
            QString value;
            int asked = 0;
            const QString *text() override
            {
                ++asked;
                return &value;
            }
        };
        Text text;
        text.value = QStringLiteral("Presupuesto anual de 2026\nTotal: 1500");

        const Query byFilter = parsed(QStringLiteral("content:\"presupuesto anual\" ext:txt"));
        QVERIFY(byFilter.usesContent());
        QVERIFY(byFilter.unavailableFilters().isEmpty());
        const QString pdf = QStringLiteral("cuentas.pdf");
        const QString txt = QStringLiteral("cuentas.txt");
        QVERIFY(!byFilter.matches(file(pdf), &text));
        QCOMPARE(text.asked, 0);   // la extensión ya lo descartó: no se lee
        QVERIFY(byFilter.matches(file(txt), &text));
        QCOMPARE(text.asked, 1);
        // Sin nadie que dé el texto, no se cumple.
        QVERIFY(!byFilter.matches(file(txt)));
        // Una carpeta no tiene contenido.
        Item folder = file(txt);
        folder.isDirectory = true;
        QVERIFY(!byFilter.matches(folder, &text));

        QVERIFY(!parsed(QStringLiteral("content:PRESUPUESTO"), true).matches(file(txt), &text));
        QVERIFY(parsed(QStringLiteral("content:PRESUPUESTO"), false).matches(file(txt), &text));
        QVERIFY(!parsed(QStringLiteral("cuentas")).usesContent());

        // Modo "buscar en el contenido": las palabras sueltas miran dentro.
        QueryOptions inside;
        inside.termsInContent = true;
        const Query words = Query::parse(QStringLiteral("total anual*"), inside).value();
        QVERIFY(words.usesContent());
        QVERIFY(words.matches(file(pdf), &text));
        QVERIFY(!Query::parse(QStringLiteral("total inexistente"), inside).value().matches(file(pdf), &text));
        // Los filtros siguen mirando el nombre.
        QVERIFY(!Query::parse(QStringLiteral("total name:informe"), inside).value().matches(file(pdf), &text));
    }

    void syntaxErrors_data()
    {
        QTest::addColumn<QString>("query");
        QTest::newRow("paréntesis sin cerrar") << "(alfa OR beta";
        QTest::newRow("paréntesis de más") << "alfa)";
        QTest::newRow("paréntesis vacíos") << "()";
        QTest::newRow("OR al final") << "alfa OR";
        QTest::newRow("OR al principio") << "OR alfa";
        QTest::newRow("AND al final") << "alfa AND";
        QTest::newRow("NOT solo") << "NOT";
        QTest::newRow("dos operadores") << "alfa AND OR beta";
        QTest::newRow("comilla sin cerrar") << "\"acta final";
        QTest::newRow("tamaño sin sentido") << "size:grande";
        QTest::newRow("tamaño al revés") << "size:2MB..1MB";
        QTest::newRow("fecha imposible") << "modified:2025-13";
        QTest::newRow("fecha con letras") << "modified:enero";
        QTest::newRow("tipo desconocido") << "type:cosa";
        QTest::newRow("hidden raro") << "hidden:quizá";
        QTest::newRow("regex sin barras") << "regex:abc";
        QTest::newRow("regex sin cerrar") << "regex:/abc";
        QTest::newRow("regex inválida") << "regex:/(/";
        QTest::newRow("filtro sin valor") << "ext:";
        // Consultas a medio escribir, como las ve el filtro tecla a tecla.
        QTest::newRow("solo abre paréntesis") << "(";
        QTest::newRow("palabra y abre paréntesis") << "informe (";
        QTest::newRow("paréntesis y OR") << "informe (marzo OR";
        QTest::newRow("NOT y paréntesis") << "NOT (";
        QTest::newRow("guion y paréntesis") << "-(";
        QTest::newRow("AND solo") << "AND";
    }

    void syntaxErrors()
    {
        QFETCH(QString, query);
        const core::Result<Query> result = Query::parse(query);
        QVERIFY2(!result.ok(), qPrintable(query));
        // El mensaje explica el problema: no es un texto vacío ni genérico.
        QVERIFY(result.error().message.size() > 10);
    }

    void everyPrefixOfAQueryIsHandled()
    {
        // Al escribir, el analizador recibe todos los prefijos de la consulta:
        // cada uno debe dar una consulta válida o un error explicado, nunca un fallo.
        const QString full = QStringLiteral(
            "informe (marzo OR \"acta final\") AND NOT -borrador ext:pdf,docx size:>1,5MB "
            "modified:2025-01..2025-06 type:documento hidden:no regex:/^a(b|c)\\/d/i tag:rojo");
        for (qsizetype length = 0; length <= full.size(); ++length) {
            const core::Result<Query> result = Query::parse(full.left(length));
            if (!result.ok()) {
                QVERIFY2(!result.error().message.isEmpty(), qPrintable(full.left(length)));
            }
        }
        QVERIFY(Query::parse(full).ok());
    }

    void filteringManyNamesIsFast()
    {
        // Lo que hará el filtrado local: evaluar la consulta contra todos los
        // elementos de una carpeta en cada tecla.
        std::vector<QString> names;
        names.reserve(100'000);
        for (int i = 0; i < 100'000; ++i) {
            names.push_back(QStringLiteral("Documento de trabajo número %1 (borrador).docx").arg(i));
        }
        const Query query = parsed(QStringLiteral("trabajo *9*.docx OR ext:pdf NOT final"));
        QElapsedTimer timer;
        timer.start();
        int matches = 0;
        for (const QString &name : names) {
            Item item;
            item.name = name;
            if (query.matches(item)) {
                ++matches;
            }
        }
        const qint64 elapsed = timer.elapsed();
        qInfo("100 000 nombres evaluados en %lld ms (%d coincidencias)", static_cast<long long>(elapsed), matches);
        QVERIFY(matches > 0);
#ifdef EU_CHECK_TIMINGS
        // Meta de la especificación (§4): < 100 ms por tecla. En el equipo de
        // desarrollo son ~15 ms; el margen es para los equipos de la CI.
        QVERIFY2(elapsed < 100, qPrintable(QStringLiteral("%1 ms").arg(elapsed)));
#endif
    }
};

QTEST_APPLESS_MAIN(TestQuery)
#include "test_query.moc"
