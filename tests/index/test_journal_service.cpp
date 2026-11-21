// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
//
// El servicio eu-indexsvc de verdad (solo Windows). Leer el diario de NTFS
// exige ser administrador: sin serlo, las pruebas se saltan (la CI lo es).

#include "eu/index/ChangeJournal.h"
#include "eu/index/UsnProtocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <windows.h>

using namespace eu;
using namespace eu::index;

namespace {

bool isAdministrator()
{
    BOOL member = FALSE;
    PSID administrators = nullptr;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0,
                                 0, &administrators)) {
        if (!CheckTokenMembership(nullptr, administrators, &member)) {
            member = FALSE;
        }
        FreeSid(administrators);
    }
    return member != FALSE;
}

QString serviceProgram()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/eu-indexsvc.exe");
}

void writeFile(const QString &path)
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write("x");
}

/// true si alguna de las carpetas acaba en `name` (la ruta que da el servicio
/// es la larga y normalizada; la temporal puede venir en forma corta, 8.3).
bool containsFolderNamed(const QStringList &folders, const QString &name)
{
    for (const QString &folder : folders) {
        if (folder.endsWith(QLatin1Char('/') + name, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

/// Lo que se espera del servicio, se lance como se lance.
void checkJournal(const QTemporaryDir &directory)
{
    const std::shared_ptr<ChangeJournal> journal = systemChangeJournal();
    QVERIFY(journal != nullptr);
    const QString volume = journal->volumeOf(directory.path());
    QVERIFY2(volume.size() == 2 && volume.endsWith(QLatin1Char(':')), qPrintable(volume));

    const core::Result<JournalPosition> start = journal->position(volume);
    QVERIFY2(start.ok(), qPrintable(start.ok() ? QString() : start.error().message));
    QVERIFY(start.value().isValid());

    // Una carpeta nueva con un archivo dentro: cambian la temporal y la nueva.
    const QString name = QFileInfo(directory.path()).fileName();
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("nueva")));
    writeFile(directory.filePath(QStringLiteral("nueva/dato.txt")));
    const core::Result<JournalChanges> changes = journal->changesSince(volume, start.value(), 100000);
    QVERIFY2(changes.ok(), qPrintable(changes.ok() ? QString() : changes.error().message));
    QVERIFY2(containsFolderNamed(changes.value().folders, name), qPrintable(changes.value().folders.join(QLatin1Char('\n'))));
    QVERIFY2(containsFolderNamed(changes.value().folders, QStringLiteral("nueva")),
             qPrintable(changes.value().folders.join(QLatin1Char('\n'))));
    QCOMPARE(changes.value().next.journal, start.value().journal);
    QVERIFY(changes.value().next.usn > start.value().usn);

    // Con más carpetas cambiadas que las pedidas, mejor recorrer.
    const core::Result<JournalChanges> few = journal->changesSince(volume, start.value(), 1);
    QVERIFY(!few.ok());
    QCOMPARE(few.error().code, core::ErrorCode::Busy);
    // Otro diario (se borró y se volvió a crear): no sirve.
    const core::Result<JournalChanges> other =
        journal->changesSince(volume, JournalPosition{start.value().journal + 1, start.value().usn}, 100);
    QVERIFY(!other.ok());
    QCOMPARE(other.error().code, core::ErrorCode::Corrupted);

    // La MFT: solo números.
    const core::Result<VolumeCounts> counted = journal->counts(volume);
    QVERIFY2(counted.ok(), qPrintable(counted.ok() ? QString() : counted.error().message));
    QVERIFY2(counted.value().files > 1000 && counted.value().folders > 100,
             qPrintable(QStringLiteral("%1 archivos, %2 carpetas").arg(counted.value().files).arg(counted.value().folders)));
}

} // namespace

class TestJournalService : public QObject
{
    Q_OBJECT

private slots:
    void answersInConsoleMode()
    {
        if (!isAdministrator()) {
            QSKIP("Leer el diario de NTFS exige ser administrador.");
        }
        QVERIFY(QFileInfo::exists(serviceProgram()));
        const QString pipe = QStringLiteral("\\\\.\\pipe\\eu-prueba-%1").arg(QCoreApplication::applicationPid());
        QProcess server;
        server.start(serviceProgram(), {QStringLiteral("--consola"), QStringLiteral("--tuberia"), pipe});
        QVERIFY(server.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(server.readAllStandardOutput().contains("listo")
                                     || server.state() == QProcess::NotRunning, 10000);
        QVERIFY2(server.state() == QProcess::Running, server.readAllStandardError().constData());
        qputenv(usn::pipeVariable, pipe.toLocal8Bit());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        checkJournal(directory);

        // Una tubería que no es la del servicio no se usa si no se pide así.
        qunsetenv(usn::pipeVariable);
        server.kill();
        server.waitForFinished();
    }

    void installsRunsAndUninstalls()
    {
        // Instala un servicio de verdad en el equipo: solo en la CI.
        if (qgetenv("EU_PRUEBA_INSTALAR_SERVICIO") != "1") {
            QSKIP("Solo en la CI (EU_PRUEBA_INSTALAR_SERVICIO=1): instala un servicio de Windows.");
        }
        if (!isAdministrator()) {
            QSKIP("Instalar un servicio exige ser administrador.");
        }
        qunsetenv(usn::pipeVariable);
        QProcess install;
        install.start(serviceProgram(), {QStringLiteral("--instalar")});
        QVERIFY(install.waitForFinished(120000));
        QCOMPARE(install.exitCode(), 0);
        QCOMPARE(journalservice::state(), journalservice::State::Running);
        const QString installed = QDir::fromNativeSeparators(qEnvironmentVariable("ProgramFiles"))
            + QStringLiteral("/Explorador Ultra/servicio/eu-indexsvc.exe");
        QVERIFY2(QFileInfo::exists(installed), qPrintable(installed));

        // Instalar otra vez lo actualiza sin fallar.
        QProcess again;
        again.start(serviceProgram(), {QStringLiteral("--instalar")});
        QVERIFY(again.waitForFinished(120000));
        QCOMPARE(again.exitCode(), 0);

        // Por la tubería del servicio instalado (la aplicación comprueba que es él).
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        checkJournal(directory);

        QProcess uninstall;
        uninstall.start(serviceProgram(), {QStringLiteral("--desinstalar")});
        QVERIFY(uninstall.waitForFinished(120000));
        QCOMPARE(uninstall.exitCode(), 0);
        // (Windows lo borra del todo cuando nadie lo tiene abierto.)
        QTRY_COMPARE_WITH_TIMEOUT(journalservice::state(), journalservice::State::NotInstalled, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(installed), 10000);
        // Sin servicio, cada consulta falla (y el índice recorre el disco como siempre).
        QVERIFY(!systemChangeJournal()->position(QStringLiteral("C:")).ok());
    }
};

QTEST_GUILESS_MAIN(TestJournalService)
#include "test_journal_service.moc"
