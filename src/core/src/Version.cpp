// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/core/Version.h"

namespace eu::core {

QString versionString()
{
    // EU_VERSION_STRING la inyecta CMake para que exista una sola fuente de verdad.
    return QStringLiteral(EU_VERSION_STRING);
}

QString applicationName()
{
    return QStringLiteral("Explorador Ultra");
}

} // namespace eu::core
