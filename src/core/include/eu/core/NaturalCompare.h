// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QStringView>

namespace eu::core {

/// Comparación "natural" de nombres: los tramos de dígitos se comparan por su
/// valor numérico, de modo que "archivo2" va antes que "archivo10" (SPEC §5.4).
///
/// Devuelve un valor negativo si a < b, cero si son equivalentes y positivo si a > b.
/// No distingue mayúsculas de minúsculas, salvo para desempatar, de modo que el
/// orden sea siempre total y estable.
int naturalCompare(QStringView a, QStringView b);

} // namespace eu::core
