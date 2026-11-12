// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/core/NaturalCompare.h"

#include <QChar>

namespace eu::core {

namespace {

/// Avanza `pos` hasta el final del tramo de dígitos que empieza en `pos`.
qsizetype endOfDigitRun(QStringView text, qsizetype pos)
{
    while (pos < text.size() && text[pos].isDigit()) {
        ++pos;
    }
    return pos;
}

/// Compara dos tramos formados solo por dígitos según su valor numérico.
/// No se convierten a entero para no desbordar con números muy largos
/// (por ejemplo, nombres con marcas de tiempo de 20 cifras).
int compareDigitRuns(QStringView a, QStringView b)
{
    // Los ceros a la izquierda no cambian el valor.
    while (a.size() > 1 && a.front().digitValue() == 0) {
        a = a.mid(1);
    }
    while (b.size() > 1 && b.front().digitValue() == 0) {
        b = b.mid(1);
    }
    if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;   // más cifras significa número mayor
    }
    for (qsizetype i = 0; i < a.size(); ++i) {
        const int da = a[i].digitValue();
        const int db = b[i].digitValue();
        if (da != db) {
            return da < db ? -1 : 1;
        }
    }
    return 0;
}

} // namespace

int naturalCompare(QStringView a, QStringView b)
{
    qsizetype ia = 0;
    qsizetype ib = 0;

    while (ia < a.size() && ib < b.size()) {
        if (a[ia].isDigit() && b[ib].isDigit()) {
            const qsizetype endA = endOfDigitRun(a, ia);
            const qsizetype endB = endOfDigitRun(b, ib);
            const int result = compareDigitRuns(a.mid(ia, endA - ia), b.mid(ib, endB - ib));
            if (result != 0) {
                return result;
            }
            ia = endA;
            ib = endB;
            continue;
        }

        const QChar ca = a[ia].toCaseFolded();
        const QChar cb = b[ib].toCaseFolded();
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        ++ia;
        ++ib;
    }

    if (ia < a.size()) {
        return 1;    // a tiene más texto: va después
    }
    if (ib < b.size()) {
        return -1;
    }

    // Equivalentes sin distinguir mayúsculas ni ceros: se desempata con la
    // comparación exacta para que el orden sea determinista.
    return a.compare(b, Qt::CaseSensitive);
}

} // namespace eu::core
