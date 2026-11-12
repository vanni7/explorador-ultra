// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace eu::core {

struct LogOptions
{
    QString directory;                 ///< Carpeta donde se guardan los registros.
    QString baseName = QStringLiteral("explorador-ultra");
    qint64 maxFileBytes = 2 * 1024 * 1024; ///< Al superarlo, el archivo rota.
    int maxFiles = 5;                  ///< Archivos que se conservan (actual + antiguos).
    bool alsoToStderr = true;
};

/// Activa el registro rotativo en archivo (SPEC §21). A partir de aquí, todo lo
/// que se escriba con qDebug/qInfo/qWarning/qCritical va a
/// `<directory>/<baseName>.log`; al llenarse pasa a `.1.log`, `.2.log`…
///
/// Es seguro llamar a las funciones de registro desde cualquier hilo.
/// Devuelve false si no se pudo crear o abrir el archivo (la aplicación sigue
/// funcionando y registra solo en stderr).
bool startFileLogging(const LogOptions &options);

/// Cierra el archivo y restaura el comportamiento normal de Qt.
void stopFileLogging();

/// Ruta del archivo de registro activo, o vacía si no hay.
QString currentLogFile();

} // namespace eu::core
