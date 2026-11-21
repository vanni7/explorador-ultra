// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/ChangeJournal.h"
#include "eu/index/UsnProtocol.h"

#include <QTest>

#include <limits>

using namespace eu::index;

class TestUsnProtocol : public QObject
{
    Q_OBJECT

private slots:
    void linesAndFields()
    {
        const auto fields = usn::splitFields("cambios\tC:\t12\t345\t5000\n");
        QCOMPARE(fields.size(), std::size_t(5));
        QCOMPARE(QString::fromStdString(fields[0]), QStringLiteral("cambios"));
        QCOMPARE(QString::fromStdString(fields[4]), QStringLiteral("5000"));
        QCOMPARE(usn::splitFields("hola\r\n").size(), std::size_t(1));
        QCOMPARE(usn::splitFields("").size(), std::size_t(1));   // una línea vacía: un campo vacío
        QCOMPARE(usn::splitFields("a\t\tb").size(), std::size_t(3));

        // Un tabulador o un salto de línea dentro de un campo no lo parte.
        QCOMPARE(QString::fromStdString(usn::makeLine({"error", "sistema", "dos\tpartes\ny más"})),
                 QStringLiteral("error\tsistema\tdos partes y más\n"));
        QCOMPARE(QString::fromStdString(usn::makeLine({"hola"})), QStringLiteral("hola\n"));
    }

    void onlyDriveLettersAreVolumes()
    {
        QVERIFY(usn::isDrive("C:"));
        QVERIFY(usn::isDrive("z:"));
        QVERIFY(!usn::isDrive("C:\\"));
        QVERIFY(!usn::isDrive("\\\\.\\C:"));
        QVERIFY(!usn::isDrive("CC"));
        QVERIFY(!usn::isDrive("1:"));
        QVERIFY(!usn::isDrive(""));
        QVERIFY(!usn::isDrive("..\\"));
    }

    void numbersAreStrict()
    {
        QCOMPARE(usn::toUnsigned("123"), std::optional<std::uint64_t>(123));
        QCOMPARE(usn::toUnsigned("18446744073709551615"),
                 std::optional<std::uint64_t>(std::numeric_limits<std::uint64_t>::max()));
        QVERIFY(!usn::toUnsigned("18446744073709551616"));   // no cabe
        QVERIFY(!usn::toUnsigned("-1"));
        QVERIFY(!usn::toUnsigned("12a"));
        QVERIFY(!usn::toUnsigned(" 12"));
        QVERIFY(!usn::toUnsigned(""));
        QCOMPARE(usn::toSigned("-5"), std::optional<std::int64_t>(-5));
        QVERIFY(!usn::toSigned("+5"));
        QVERIFY(!usn::toSigned("9223372036854775808"));
    }

    void positionsAreSavedAsText()
    {
        const JournalPosition position{133000000000000001ULL, 98765};
        QCOMPARE(position.toString(), QStringLiteral("133000000000000001:98765"));
        const JournalPosition back = JournalPosition::fromString(position.toString());
        QCOMPARE(back.journal, position.journal);
        QCOMPARE(back.usn, position.usn);
        QVERIFY(!JournalPosition::fromString(QString()).isValid());
        QVERIFY(!JournalPosition::fromString(QStringLiteral("basura")).isValid());
        QVERIFY(!JournalPosition::fromString(QStringLiteral("5:-1")).isValid());
        QVERIFY(!JournalPosition::fromString(QStringLiteral("0:5")).isValid());   // diario 0: ninguno
        QVERIFY(JournalPosition{}.toString().isEmpty());
    }

    void otherSystemsHaveNoJournal()
    {
#ifndef Q_OS_WIN
        QVERIFY(systemChangeJournal() == nullptr);
        QCOMPARE(journalservice::state(), journalservice::State::Unavailable);
        QVERIFY(!journalservice::Setup::start(true).ok());
#else
        QSKIP("En Windows lo prueba test_index_journal_service.");
#endif
    }
};

QTEST_GUILESS_MAIN(TestUsnProtocol)
#include "test_usn_protocol.moc"
