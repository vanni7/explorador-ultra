// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/core/CrashReport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace eu::core;

class TestCrashReport : public QObject
{
    Q_OBJECT

private slots:
    void aCrashLeavesWhereItHappened()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QProcess helper;
        helper.start(QCoreApplication::applicationDirPath() + QStringLiteral("/eu-crash-report-helper"),
                     {directory.path()});
        QVERIFY(helper.waitForFinished(60000));
        if (helper.readAllStandardOutput().contains("sin-registro")) {
            QSKIP("Compilación con sanitizer: los fallos los informa él.");
        }
        QVERIFY(helper.exitStatus() == QProcess::CrashExit || helper.exitCode() != 0);

        QFile log(crashLogFile(directory.path()));
        QVERIFY2(log.open(QIODevice::ReadOnly), qPrintable(log.fileName()));
        const QByteArray text = log.readAll();
        QVERIFY2(text.contains("=== FALLO"), text.constData());
        QVERIFY2(text.contains("=== fin del informe"), text.constData());
        // Saltó a la dirección 0: se dice, y se nombra a quien hizo la llamada.
        QVERIFY2(text.contains("puntero no válido"), text.constData());
        QVERIFY2(text.contains("crashThroughNullPointer"), text.constData());
#ifdef Q_OS_WIN
        // Y el volcado para abrirlo con el .pdb.
        QVERIFY2(!QDir(directory.path()).entryList({QStringLiteral("fallo-*.dmp")}, QDir::Files).isEmpty(),
                 text.constData());
#endif
    }
};

QTEST_GUILESS_MAIN(TestCrashReport)
#include "test_crash_report.moc"
