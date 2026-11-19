// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QPalette>

class QSettings;

namespace eu::ui {

/// Tema de la interfaz (SPEC §5.2).
enum class ThemeKind {
    System,   ///< El del sistema (estilo y colores de fábrica de la plataforma).
    Light,
    Dark,
    Custom,   ///< Colores elegidos por el usuario.
};

/// Apariencia elegida en Preferencias. Se guarda en la configuración
/// (grupo «apariencia») y se aplica al instante a toda la aplicación.
struct Appearance
{
    ThemeKind theme = ThemeKind::System;
    // Colores del tema personalizado.
    QColor background = QColor(0xfa, 0xfa, 0xfa);   ///< Fondo de las listas.
    QColor text = QColor(0x20, 0x20, 0x20);
    QColor selection = QColor(0x2f, 0x7d, 0xd1);
    QColor accent = QColor(0x2f, 0x7d, 0xd1);       ///< Enlaces, foco y la franja del panel activo.
    QColor panels = QColor(0xec, 0xec, 0xec);       ///< Ventana, barras y botones.
    int fontPointSize = 0;      ///< 0: la letra del sistema.
    int listIconSize = 16;      ///< Iconos de la vista detallada y de las columnas.

    bool operator==(const Appearance &other) const = default;
};

Appearance loadAppearance(const QSettings &settings);
void saveAppearance(QSettings &settings, const Appearance &appearance);
/// Quita lo guardado (vuelve a la de fábrica).
void clearAppearance(QSettings &settings);

/// Aplica tema y letra a toda la aplicación. La primera vez recuerda el
/// estilo, la paleta y la letra del sistema, para volver a ellos con «Sistema».
void applyAppearance(const Appearance &appearance);

/// Paletas de los temas (públicas para las pruebas).
QPalette lightPalette();
QPalette darkPalette();
QPalette customPalette(const Appearance &appearance);
/// Texto legible sobre `background` (negro o blanco).
QColor contrastingText(const QColor &background);

} // namespace eu::ui
