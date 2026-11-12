// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
//
// Programa auxiliar de la prueba de recuperación: empieza a mover una carpeta
// y se mata a sí mismo, sin limpiar nada, cuando ha copiado cierta cantidad de
// bytes. Simula un corte de luz o un cierre forzado a mitad de operación.
//
// Uso: eu-crash-helper <historial.db> <origen> <carpetaDestino> <bytesAntesDeMorir> <copiar|mover>

#include "eu/ops/Engine.h"

#include <QCoreApplication>

#include <cstdlib>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() != 6) {
        return 2;
    }
    auto journal = eu::journal::Journal::open(arguments[1]);
    if (!journal.ok()) {
        return 3;
    }
    eu::ops::Engine engine(journal.value());
    const qint64 dieAfterBytes = arguments[4].toLongLong();
    const auto dieWhenReached = [dieAfterBytes](const eu::ops::Progress &progress) {
        if (progress.bytesDone >= dieAfterBytes) {
            std::_Exit(42);   // muerte inmediata: sin destructores ni vaciado de búferes
        }
    };
    const eu::tasks::CancelToken cancel;
    if (arguments[5] == QStringLiteral("mover")) {
        engine.move({arguments[2]}, arguments[3], eu::ops::CollisionPolicy::Skip, cancel, dieWhenReached);
    } else {
        engine.copy({arguments[2]}, arguments[3], eu::ops::CollisionPolicy::Skip, cancel, dieWhenReached);
    }
    return 0;   // si llega aquí, no murió a tiempo: la prueba lo detecta
}
