// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/core/NaturalCompare.h"
#include "eu/core/Version.h"

#include <QStringList>
#include <QTest>

#include <algorithm>

using eu::core::naturalCompare;

class TestNaturalCompare : public QObject
{
    Q_OBJECT

private slots:
    void versionIsNotEmpty()
    {
        QVERIFY(!eu::core::versionString().isEmpty());
    }

    void numbersCompareByValue()
    {
        QVERIFY(naturalCompare(u"archivo2", u"archivo10") < 0);
        QVERIFY(naturalCompare(u"archivo10", u"archivo2") > 0);
        QVERIFY(naturalCompare(u"a9b", u"a10a") < 0);
    }

    void caseIsIgnoredExceptForTies()
    {
        QVERIFY(naturalCompare(u"Beta", u"alfa") > 0);
        QVERIFY(naturalCompare(u"FOTO", u"foto") != 0);   // orden total: nunca "iguales"
        QCOMPARE(naturalCompare(u"foto", u"foto"), 0);
    }

    void leadingZerosAndHugeNumbers()
    {
        QVERIFY(naturalCompare(u"img007", u"img8") < 0);
        // 30 cifras: desbordaría cualquier entero; debe compararse igualmente bien.
        QVERIFY(naturalCompare(u"x123456789012345678901234567890",
                               u"x123456789012345678901234567891") < 0);
    }

    void prefixGoesFirst()
    {
        QVERIFY(naturalCompare(u"nota", u"nota final") < 0);
        QVERIFY(naturalCompare(u"", u"a") < 0);
    }

    void sortsAListAsAUserExpects()
    {
        QStringList names{QStringLiteral("archivo10.txt"), QStringLiteral("archivo2.txt"),
                          QStringLiteral("Archivo1.txt"), QStringLiteral("ñandú.txt"),
                          QStringLiteral("archivo20.txt")};
        std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
            return naturalCompare(a, b) < 0;
        });
        const QStringList expected{QStringLiteral("Archivo1.txt"), QStringLiteral("archivo2.txt"),
                                   QStringLiteral("archivo10.txt"), QStringLiteral("archivo20.txt"),
                                   QStringLiteral("ñandú.txt")};
        QCOMPARE(names, expected);
    }
};

QTEST_APPLESS_MAIN(TestNaturalCompare)
#include "test_natural_compare.moc"
