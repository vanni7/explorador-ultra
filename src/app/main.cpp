// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
//
// Fase 0: ventana mínima ("hola mundo"). Su único propósito es comprobar que la
// cadena de compilación y Qt funcionan en Linux, Windows y macOS.
// La interfaz real empieza en la Fase 1 (módulo ui).

#include "eu/core/Version.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(eu::core::applicationName());
    QApplication::setApplicationVersion(eu::core::versionString());
    QApplication::setOrganizationName(QStringLiteral("ExploradorUltra"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icon/explorador-ultra.svg")));

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    // --smoke-test: abre la ventana y sale sola. Lo usa la CI para comprobar
    // que el ejecutable arranca de verdad y no solo que compila.
    const QCommandLineOption smokeTestOption(
        QStringLiteral("smoke-test"),
        QStringLiteral("Abre la ventana, espera un instante y termina (para la CI)."));
    parser.addOption(smokeTestOption);
    parser.process(application);

    QMainWindow window;
    window.setWindowTitle(eu::core::applicationName());
    window.resize(720, 420);

    auto *label = new QLabel(
        QStringLiteral("Explorador Ultra %1\nFase 0: entorno de compilación verificado (Qt %2).")
            .arg(eu::core::versionString(), QString::fromLatin1(qVersion())),
        &window);
    label->setAlignment(Qt::AlignCenter);
    window.setCentralWidget(label);
    window.show();

    if (parser.isSet(smokeTestOption)) {
        QTimer::singleShot(500, &application, &QApplication::quit);
    }

    return QApplication::exec();
}
