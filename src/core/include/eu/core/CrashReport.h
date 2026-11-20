// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace eu::core {

/// Registro de fallos (SPEC §21): si el programa se cae (acceso a memoria no
/// válida, instrucción ilegal, abort…), anota en `<directory>/fallos.log` la
/// hora, la versión, qué pasó y la pila de llamadas, para poder encontrar la
/// causa después aunque no se pueda repetir. En Windows deja además un volcado
/// `fallo-<fecha>.dmp` que se abre con el .pdb de la compilación.
///
/// Se llama una vez, al empezar. Devuelve false si no se instaló: no se pudo
/// abrir el archivo, o la compilación lleva AddressSanitizer, que ya informa
/// de los fallos por su cuenta.
bool installCrashReport(const QString &directory);

/// El archivo donde se anotan los fallos.
QString crashLogFile(const QString &directory);

} // namespace eu::core
