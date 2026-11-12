// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/core/FileName.h"

#include <QTest>

using namespace eu::core;

class TestFileName : public QObject
{
    Q_OBJECT

private slots:
    void check_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<NameRules>("rules");
        QTest::addColumn<NameProblem>("expected");

        QTest::newRow("normal") << QStringLiteral("informe 2026.pdf") << NameRules::Portable << NameProblem::None;
        QTest::newRow("unicode y emoji") << QStringLiteral("ñandú 🦤 שלום.txt") << NameRules::Portable << NameProblem::None;
        QTest::newRow("vacío") << QString() << NameRules::Posix << NameProblem::Empty;
        QTest::newRow("punto") << QStringLiteral(".") << NameRules::Posix << NameProblem::DotOrDotDot;
        QTest::newRow("dos puntos") << QStringLiteral("..") << NameRules::Windows << NameProblem::DotOrDotDot;
        QTest::newRow("oculto es válido") << QStringLiteral(".config") << NameRules::Portable << NameProblem::None;
        QTest::newRow("barra") << QStringLiteral("a/b") << NameRules::Posix << NameProblem::InvalidCharacter;
        QTest::newRow("posix admite casi todo") << QStringLiteral("a:b*c?\"<>|\\") << NameRules::Posix << NameProblem::None;
        QTest::newRow("dos puntos en macOS") << QStringLiteral("12:30.txt") << NameRules::MacOS << NameProblem::InvalidCharacter;
        QTest::newRow("asterisco en macOS") << QStringLiteral("a*b") << NameRules::MacOS << NameProblem::None;
        QTest::newRow("interrogación en Windows") << QStringLiteral("qué?.txt") << NameRules::Windows << NameProblem::InvalidCharacter;
        QTest::newRow("control en Windows") << QStringLiteral("a\tb") << NameRules::Windows << NameProblem::InvalidCharacter;
        QTest::newRow("NUL") << QStringLiteral("NUL") << NameRules::Windows << NameProblem::ReservedName;
        QTest::newRow("nul.txt") << QStringLiteral("nul.txt") << NameRules::Windows << NameProblem::ReservedName;
        QTest::newRow("COM1 en posix") << QStringLiteral("COM1") << NameRules::Posix << NameProblem::None;
        QTest::newRow("NULO no es reservado") << QStringLiteral("NULO.txt") << NameRules::Windows << NameProblem::None;
        QTest::newRow("punto final") << QStringLiteral("nota.") << NameRules::Windows << NameProblem::TrailingDotOrSpace;
        QTest::newRow("espacio final") << QStringLiteral("nota ") << NameRules::Portable << NameProblem::TrailingDotOrSpace;
        QTest::newRow("punto final en posix") << QStringLiteral("nota.") << NameRules::Posix << NameProblem::None;
        QTest::newRow("255 ascii") << QString(255, QLatin1Char('a')) << NameRules::Portable << NameProblem::None;
        QTest::newRow("256 ascii") << QString(256, QLatin1Char('a')) << NameRules::Portable << NameProblem::TooLong;
        // 200 "ñ" son 200 unidades UTF-16 (cabe en NTFS) pero 400 bytes UTF-8 (no cabe en ext4).
        QTest::newRow("200 ñ en Windows") << QString(200, QChar(u'ñ')) << NameRules::Windows << NameProblem::None;
        QTest::newRow("200 ñ en posix") << QString(200, QChar(u'ñ')) << NameRules::Posix << NameProblem::TooLong;
    }

    void check()
    {
        QFETCH(QString, name);
        QFETCH(NameRules, rules);
        QFETCH(NameProblem, expected);
        const NameCheck result = checkFileName(name, rules);
        QCOMPARE(result.problem, expected);
        QCOMPARE(describeNameProblem(result).isEmpty(), expected == NameProblem::None);
    }

    void invalidCharacterIsReported()
    {
        const NameCheck result = checkFileName(QStringLiteral("a|b"), NameRules::Windows);
        QCOMPARE(result.character, QChar(u'|'));
        QVERIFY(describeNameProblem(result).contains(QLatin1Char('|')));
    }
};

QTEST_APPLESS_MAIN(TestFileName)
#include "test_file_name.moc"
