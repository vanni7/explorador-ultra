// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QLocale>
#include <QString>

namespace eu::core {

/// Convención para mostrar tamaños (SPEC §5.4).
enum class SizeUnits {
    Decimal, ///< SI: 1 kB = 1000 B (kB, MB, GB, TB). Es la predeterminada.
    Binary,  ///< IEC: 1 KiB = 1024 B (KiB, MiB, GiB, TiB).
};

/// Devuelve un tamaño legible, por ejemplo "3,1 MB" o "2,9 MiB".
/// Por debajo de la primera unidad se muestran bytes exactos ("512 B").
/// El separador decimal sale de `locale`.
QString formatSize(qint64 bytes, SizeUnits units = SizeUnits::Decimal,
                   const QLocale &locale = QLocale());

} // namespace eu::core
