// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/core/SizeFormat.h"

#include <array>

namespace eu::core {

QString formatSize(qint64 bytes, SizeUnits units, const QLocale &locale)
{
    static const std::array<const char *, 5> decimalNames{"B", "kB", "MB", "GB", "TB"};
    static const std::array<const char *, 5> binaryNames{"B", "KiB", "MiB", "GiB", "TiB"};

    const double base = (units == SizeUnits::Decimal) ? 1000.0 : 1024.0;
    const auto &names = (units == SizeUnits::Decimal) ? decimalNames : binaryNames;

    if (bytes < 0) {
        bytes = 0;   // un tamaño negativo solo puede venir de un dato corrupto
    }

    double value = double(bytes);
    std::size_t unitIndex = 0;
    while (value >= base && unitIndex + 1 < names.size()) {
        value /= base;
        ++unitIndex;
    }

    // Si al redondear a un decimal se llega a la base ("1000,0 kB"), se sube
    // de unidad para mostrar "1,0 MB".
    if (unitIndex > 0 && unitIndex + 1 < names.size() && value >= base - 0.05) {
        value /= base;
        ++unitIndex;
    }

    const QString number = (unitIndex == 0)
        ? locale.toString(bytes)
        : locale.toString(value, 'f', value < 100.0 ? 1 : 0);
    return number + QLatin1Char(' ') + QLatin1String(names[unitIndex]);
}

} // namespace eu::core
