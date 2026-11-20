// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
//
// Programa auxiliar de test_core_crash_report: instala el registro de fallos
// y se cae llamando a una función a través de un puntero nulo, como cuando se
// usa un objeto ya destruido.
// Uso: eu-crash-report-helper <carpeta de registros>

#include "eu/core/CrashReport.h"

#include <QCoreApplication>

#include <cstdio>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace {
// Volátil: el compilador no puede saber que es nulo ni quitar la llamada.
void (*volatile g_nothing)() = nullptr;
} // namespace

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void crashThroughNullPointer()
{
    g_nothing();
    std::puts("no se cayó");   // tras la llamada: así no es una llamada final convertida en salto
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    if (argc < 2) {
        return 2;
    }
#if defined(Q_OS_WIN)
    // Sin la ventana de «dejó de funcionar», que dejaría la prueba esperando.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    if (!eu::core::installCrashReport(QString::fromLocal8Bit(argv[1]))) {
        std::puts("sin-registro");
        return 3;
    }
    std::fflush(stdout);
    crashThroughNullPointer();
    return 0;
}
