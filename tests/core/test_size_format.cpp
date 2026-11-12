// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/core/SizeFormat.h"

#include <QTest>

using namespace eu::core;

class TestSizeFormat : public QObject
{
    Q_OBJECT

private slots:
    void formats_data()
    {
        QTest::addColumn<qint64>("bytes");
        QTest::addColumn<SizeUnits>("units");
        QTest::addColumn<QString>("expected");

        QTest::newRow("cero") << qint64(0) << SizeUnits::Decimal << QStringLiteral("0 B");
        QTest::newRow("bytes exactos") << qint64(999) << SizeUnits::Decimal << QStringLiteral("999 B");
        QTest::newRow("1 kB") << qint64(1000) << SizeUnits::Decimal << QStringLiteral("1,0 kB");
        QTest::newRow("1023 B binario") << qint64(1023) << SizeUnits::Binary << QStringLiteral("1023 B");
        QTest::newRow("1 KiB") << qint64(1024) << SizeUnits::Binary << QStringLiteral("1,0 KiB");
        QTest::newRow("3,1 MB") << qint64(3'100'000) << SizeUnits::Decimal << QStringLiteral("3,1 MB");
        QTest::newRow("sin decimales desde 100") << qint64(212'400) << SizeUnits::Decimal << QStringLiteral("212 kB");
        QTest::newRow("redondeo sube de unidad") << qint64(999'980) << SizeUnits::Decimal << QStringLiteral("1,0 MB");
        QTest::newRow("4 GB frente a GiB dec") << qint64(4'000'000'000) << SizeUnits::Decimal << QStringLiteral("4,0 GB");
        QTest::newRow("4 GB frente a GiB bin") << qint64(4'000'000'000) << SizeUnits::Binary << QStringLiteral("3,7 GiB");
        QTest::newRow("tope en TB") << qint64(5'000'000'000'000'000) << SizeUnits::Decimal << QStringLiteral("5000 TB");
        QTest::newRow("negativo") << qint64(-5) << SizeUnits::Decimal << QStringLiteral("0 B");
    }

    void formats()
    {
        QFETCH(qint64, bytes);
        QFETCH(SizeUnits, units);
        QFETCH(QString, expected);
        // Español de España: coma decimal. Los números de cuatro cifras no llevan
        // separador de millares en español (regla de CLDR).
        QLocale locale(QLocale::Spanish, QLocale::Spain);
        locale.setNumberOptions(QLocale::DefaultNumberOptions);
        QCOMPARE(formatSize(bytes, units, locale), expected);
    }
};

QTEST_APPLESS_MAIN(TestSizeFormat)
#include "test_size_format.moc"
