// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
//
// Punto de entrada: prepara el registro, el planificador de tareas y la ventana.

#include "eu/core/Log.h"
#include "eu/core/Version.h"
#include "eu/journal/Journal.h"
#include "eu/tasks/Scheduler.h"
#include "eu/ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QTranslator>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("explorador-ultra"));
    QApplication::setApplicationDisplayName(eu::core::applicationName());
    QApplication::setApplicationVersion(eu::core::versionString());
    QApplication::setOrganizationName(QStringLiteral("ExploradorUltra"));
    QApplication::setDesktopFileName(QStringLiteral("explorador-ultra"));
    // Configuración en un archivo de texto legible en las tres plataformas
    // (en Windows, sin esto, iría al registro). SPEC §20.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icon/explorador-ultra.svg")));

    // Traducciones: primero las de Qt (botones estándar, diálogos) y luego las
    // propias. Hoy solo existe el español; añadir un idioma es añadir un .ts.
    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"),
                          QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QApplication::installTranslator(&qtTranslator);
    }
    QTranslator appTranslator;
    if (appTranslator.load(QStringLiteral(":/i18n/explorador-ultra_es.qm"))) {
        QApplication::installTranslator(&appTranslator);
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Administrador de archivos."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("carpeta"), QStringLiteral("Carpeta que abrir al iniciar."),
                                 QStringLiteral("[carpeta]"));
    // --smoke-test: abre la ventana y sale sola. Lo usa la CI para comprobar
    // que el ejecutable arranca de verdad y no solo que compila.
    const QCommandLineOption smokeTestOption(
        QStringLiteral("smoke-test"),
        QStringLiteral("Abre la ventana, espera un instante y termina (para la CI)."));
    parser.addOption(smokeTestOption);
    // --captura: guarda una imagen de la ventana y sale. Sirve para documentar
    // y para revisar la interfaz en máquinas sin pantalla.
    const QCommandLineOption screenshotOption(QStringLiteral("captura"),
                                              QStringLiteral("Guarda una captura de la ventana en <archivo> y termina."),
                                              QStringLiteral("archivo"));
    parser.addOption(screenshotOption);
    parser.process(application);
    const bool smokeTest = parser.isSet(smokeTestOption);

    eu::core::LogOptions logOptions;
    logOptions.directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/registros");
    if (!eu::core::startFileLogging(logOptions)) {
        qWarning("No se pudo abrir el archivo de registro en %s", qPrintable(logOptions.directory));
    }
    qInfo("Explorador Ultra %s (Qt %s)", qPrintable(eu::core::versionString()), qVersion());

    int exitCode = 0;
    {
        // El planificador se declara antes que la ventana para destruirse
        // después: así ninguna tarea sobrevive a los objetos que usa.
        eu::tasks::Scheduler scheduler;
        // Historial de operaciones. Si no se puede abrir, la aplicación sirve
        // para navegar pero no modifica archivos (SPEC §9: anotar antes de actuar).
        std::shared_ptr<eu::journal::Journal> journal;
        const QString journalPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/historial.db");
        QDir().mkpath(QFileInfo(journalPath).absolutePath());
        const auto opened = eu::journal::Journal::open(journalPath);
        if (opened.ok()) {
            journal = opened.value();
        } else {
            qWarning("No se pudo abrir el historial en %s: %s", qPrintable(journalPath),
                     qPrintable(opened.error().message));
        }
        eu::ui::MainWindow window(scheduler, journal);

        const QStringList arguments = parser.positionalArguments();
        if (!arguments.isEmpty()) {
            window.openTab(QDir(arguments.first()).absolutePath());
        } else if (smokeTest || parser.isSet(screenshotOption)) {
            window.openTab(QDir::homePath());   // la prueba no debe depender de una sesión guardada
        } else {
            window.restoreSession();
        }
        window.show();

        if (smokeTest) {
            QTimer::singleShot(800, &application, &QApplication::quit);
        }
        if (parser.isSet(screenshotOption)) {
            const QString file = parser.value(screenshotOption);
            QTimer::singleShot(1200, &window, [&window, file] {
                window.grab().save(file);
                QApplication::quit();
            });
        }
        exitCode = QApplication::exec();
    }

    eu::core::stopFileLogging();
    return exitCode;
}
