// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/core/Error.h"

#include <QTest>

#include <cerrno>

using namespace eu::core;

namespace {

Result<int> half(int number)
{
    if (number % 2 != 0) {
        return Error{ErrorCode::InvalidName, QStringLiteral("impar"), {}, 0};
    }
    return number / 2;
}

} // namespace

class TestError : public QObject
{
    Q_OBJECT

private slots:
    void resultHoldsValueOrError()
    {
        const Result<int> good = half(10);
        QVERIFY(good.ok());
        QCOMPARE(good.value(), 5);

        const Result<int> bad = half(3);
        QVERIFY(!bad.ok());
        QCOMPARE(bad.error().code, ErrorCode::InvalidName);
    }

    void statusDefaultsToSuccess()
    {
        QVERIFY(Status().ok());
        const Status failed = Error{ErrorCode::NoSpace, {}, {}, 0};
        QVERIFY(!failed.ok());
        QCOMPARE(failed.error().code, ErrorCode::NoSpace);
    }

    void errnoIsClassified_data()
    {
        QTest::addColumn<int>("errnoValue");
        QTest::addColumn<ErrorCode>("expected");
        QTest::newRow("ENOENT") << ENOENT << ErrorCode::NotFound;
        QTest::newRow("EACCES") << EACCES << ErrorCode::PermissionDenied;
        QTest::newRow("EEXIST") << EEXIST << ErrorCode::AlreadyExists;
        QTest::newRow("ENOSPC") << ENOSPC << ErrorCode::NoSpace;
        QTest::newRow("EXDEV") << EXDEV << ErrorCode::CrossDevice;
        QTest::newRow("ENOTEMPTY") << ENOTEMPTY << ErrorCode::DirectoryNotEmpty;
        QTest::newRow("EROFS") << EROFS << ErrorCode::ReadOnlyVolume;
    }

    void errnoIsClassified()
    {
        QFETCH(int, errnoValue);
        QFETCH(ErrorCode, expected);
        const Error error = errorFromErrno(errnoValue, QStringLiteral("/tmp/x"));
        QCOMPARE(error.code, expected);
        QCOMPARE(error.systemCode, errnoValue);
        QCOMPARE(error.path, QStringLiteral("/tmp/x"));
        QVERIFY(!error.message.isEmpty());
    }

    void unknownErrnoKeepsSystemText()
    {
        const Error error = errorFromErrno(EDOM, {});
        QCOMPARE(error.code, ErrorCode::Unknown);
        QVERIFY(!error.message.isEmpty());
    }

    void everyCodeHasAName()
    {
        QCOMPARE(errorCodeName(ErrorCode::Cancelled), QStringLiteral("Cancelled"));
        QCOMPARE(errorCodeName(ErrorCode::Unknown), QStringLiteral("Unknown"));
    }
};

QTEST_APPLESS_MAIN(TestError)
#include "test_error.moc"
