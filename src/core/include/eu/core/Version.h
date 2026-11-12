// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace eu::core {

/// Versión de la aplicación tal como la define CMake (por ejemplo "0.0.1").
QString versionString();

/// Nombre visible de la aplicación.
QString applicationName();

} // namespace eu::core
