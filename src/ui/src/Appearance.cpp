// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/Appearance.h"

#include <QApplication>
#include <QFont>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

#include <algorithm>
#include <cmath>

namespace eu::ui {

namespace {

/// Lo que había al arrancar, para volver con el tema «Sistema».
struct SystemLook
{
    QString styleName;
    QPalette palette;
    QFont font;
};

QColor mixed(const QColor &a, const QColor &b, qreal amountOfB)
{
    return QColor::fromRgbF(float(a.redF() * (1 - amountOfB) + b.redF() * amountOfB),
                            float(a.greenF() * (1 - amountOfB) + b.greenF() * amountOfB),
                            float(a.blueF() * (1 - amountOfB) + b.blueF() * amountOfB));
}

const SystemLook &systemLook()
{
    static const SystemLook look = [] {
        SystemLook system{QApplication::style()->name(), QApplication::palette(), QApplication::font()};
#ifdef Q_OS_WIN
        // El estilo «windows11» de Qt marca lo seleccionado con un fondo casi
        // blanco y una rayita: en el árbol y en las listas apenas se distingue.
        // Con Fusion (el que se ve en Linux) y los colores de Windows, la
        // selección es un bloque del color de acento con el texto en blanco.
        system.styleName = QStringLiteral("Fusion");
        for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive}) {
            QColor highlight = system.palette.color(QPalette::Active, QPalette::Accent);
            if (!highlight.isValid() || highlight.lightnessF() > 0.7f) {
                highlight = QColor(0x00, 0x67, 0xc0);   // azul de Windows, si el acento es muy claro
            }
            if (group == QPalette::Inactive) {
                highlight = mixed(highlight, system.palette.color(QPalette::Base), 0.35);
            }
            system.palette.setColor(group, QPalette::Highlight, highlight);
            system.palette.setColor(group, QPalette::HighlightedText, contrastingText(highlight));
        }
#endif
        return system;
    }();
    return look;
}

/// Una paleta completa a partir de unos pocos colores.
QPalette paletteFrom(const QColor &window, const QColor &base, const QColor &text, const QColor &highlight,
                     const QColor &accent)
{
    QPalette palette;
    const QColor disabledText = mixed(text, base, 0.55);
    const bool dark = base.lightnessF() < 0.5f;
    for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const bool disabled = group == QPalette::Disabled;
        palette.setColor(group, QPalette::Window, window);
        palette.setColor(group, QPalette::WindowText, disabled ? disabledText : text);
        palette.setColor(group, QPalette::Base, base);
        palette.setColor(group, QPalette::AlternateBase, mixed(base, text, dark ? 0.06 : 0.04));
        palette.setColor(group, QPalette::Text, disabled ? disabledText : text);
        palette.setColor(group, QPalette::Button, window);
        palette.setColor(group, QPalette::ButtonText, disabled ? disabledText : text);
        palette.setColor(group, QPalette::BrightText, Qt::red);
        palette.setColor(group, QPalette::ToolTipBase, base);
        palette.setColor(group, QPalette::ToolTipText, text);
        palette.setColor(group, QPalette::PlaceholderText, disabledText);
        // Sin el foco, la selección se ve más apagada (como en el sistema).
        const QColor selection = group == QPalette::Inactive ? mixed(highlight, base, 0.35) : highlight;
        palette.setColor(group, QPalette::Highlight, disabled ? mixed(highlight, base, 0.6) : selection);
        palette.setColor(group, QPalette::HighlightedText, contrastingText(selection));
        palette.setColor(group, QPalette::Link, accent);
        palette.setColor(group, QPalette::LinkVisited, mixed(accent, text, 0.3));
        palette.setColor(group, QPalette::Accent, accent);
        palette.setColor(group, QPalette::Light, mixed(window, Qt::white, dark ? 0.15 : 0.6));
        palette.setColor(group, QPalette::Midlight, mixed(window, Qt::white, dark ? 0.08 : 0.3));
        palette.setColor(group, QPalette::Mid, mixed(window, Qt::black, dark ? 0.3 : 0.2));
        palette.setColor(group, QPalette::Dark, mixed(window, Qt::black, dark ? 0.5 : 0.4));
        palette.setColor(group, QPalette::Shadow, Qt::black);
    }
    return palette;
}

ThemeKind themeFromName(const QString &name)
{
    if (name == QLatin1String("claro")) {
        return ThemeKind::Light;
    }
    if (name == QLatin1String("oscuro")) {
        return ThemeKind::Dark;
    }
    if (name == QLatin1String("personalizado")) {
        return ThemeKind::Custom;
    }
    return ThemeKind::System;
}

QString themeName(ThemeKind theme)
{
    switch (theme) {
    case ThemeKind::Light:
        return QStringLiteral("claro");
    case ThemeKind::Dark:
        return QStringLiteral("oscuro");
    case ThemeKind::Custom:
        return QStringLiteral("personalizado");
    case ThemeKind::System:
        break;
    }
    return QStringLiteral("sistema");
}

} // namespace

QColor contrastingText(const QColor &background)
{
    // Luminancia relativa (WCAG): por encima de ~0,45, texto negro.
    const auto channel = [](float value) {
        return value <= 0.03928f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
    };
    const float luminance = 0.2126f * channel(background.redF()) + 0.7152f * channel(background.greenF())
        + 0.0722f * channel(background.blueF());
    return luminance > 0.45f ? QColor(Qt::black) : QColor(Qt::white);
}

QPalette lightPalette()
{
    return paletteFrom(QColor(0xef, 0xef, 0xef), QColor(0xff, 0xff, 0xff), QColor(0x1e, 0x1e, 0x1e),
                       QColor(0x2f, 0x7d, 0xd1), QColor(0x1f, 0x6f, 0xc4));
}

QPalette darkPalette()
{
    return paletteFrom(QColor(0x2d, 0x2d, 0x30), QColor(0x1f, 0x1f, 0x22), QColor(0xe6, 0xe6, 0xe6),
                       QColor(0x3d, 0x84, 0xd6), QColor(0x5a, 0xa9, 0xf0));
}

QPalette customPalette(const Appearance &appearance)
{
    return paletteFrom(appearance.panels, appearance.background, appearance.text, appearance.selection,
                       appearance.accent);
}

Appearance loadAppearance(const QSettings &settings)
{
    Appearance appearance;
    appearance.theme = themeFromName(settings.value(QStringLiteral("apariencia/tema")).toString());
    const auto color = [&settings](const QString &key, const QColor &fallback) {
        const QColor stored(settings.value(QStringLiteral("apariencia/") + key).toString());
        return stored.isValid() ? stored : fallback;
    };
    appearance.background = color(QStringLiteral("fondo"), appearance.background);
    appearance.text = color(QStringLiteral("texto"), appearance.text);
    appearance.selection = color(QStringLiteral("seleccion"), appearance.selection);
    appearance.accent = color(QStringLiteral("acento"), appearance.accent);
    appearance.panels = color(QStringLiteral("paneles"), appearance.panels);
    appearance.fontPointSize = std::clamp(settings.value(QStringLiteral("apariencia/tamanoLetra"), 0).toInt(), 0, 40);
    appearance.listIconSize = std::clamp(settings.value(QStringLiteral("apariencia/tamanoIconos"), 16).toInt(), 12, 64);
    return appearance;
}

void saveAppearance(QSettings &settings, const Appearance &appearance)
{
    settings.setValue(QStringLiteral("apariencia/tema"), themeName(appearance.theme));
    settings.setValue(QStringLiteral("apariencia/fondo"), appearance.background.name());
    settings.setValue(QStringLiteral("apariencia/texto"), appearance.text.name());
    settings.setValue(QStringLiteral("apariencia/seleccion"), appearance.selection.name());
    settings.setValue(QStringLiteral("apariencia/acento"), appearance.accent.name());
    settings.setValue(QStringLiteral("apariencia/paneles"), appearance.panels.name());
    settings.setValue(QStringLiteral("apariencia/tamanoLetra"), appearance.fontPointSize);
    settings.setValue(QStringLiteral("apariencia/tamanoIconos"), appearance.listIconSize);
}

void clearAppearance(QSettings &settings)
{
    settings.remove(QStringLiteral("apariencia"));
}

void applyAppearance(const Appearance &appearance)
{
    const SystemLook &system = systemLook();
    if (appearance.theme == ThemeKind::System) {
        if (QApplication::style()->name() != system.styleName) {
            if (QStyle *style = QStyleFactory::create(system.styleName)) {
                QApplication::setStyle(style);
            }
        }
        QApplication::setPalette(system.palette);
        QGuiApplication::styleHints()->unsetColorScheme();
    } else {
        // Fusion: el mismo aspecto en las tres plataformas y respeta la paleta entera.
        if (QApplication::style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive) != 0) {
            if (QStyle *style = QStyleFactory::create(QStringLiteral("Fusion"))) {
                QApplication::setStyle(style);
            }
        }
        const QPalette palette = appearance.theme == ThemeKind::Light ? lightPalette()
            : appearance.theme == ThemeKind::Dark                      ? darkPalette()
                                                                       : customPalette(appearance);
        QGuiApplication::styleHints()->setColorScheme(palette.color(QPalette::Base).lightnessF() < 0.5f
                                                          ? Qt::ColorScheme::Dark
                                                          : Qt::ColorScheme::Light);
        QApplication::setPalette(palette);
    }
    QFont font = system.font;
    if (appearance.fontPointSize > 0) {
        font.setPointSize(appearance.fontPointSize);
    }
    QApplication::setFont(font);
}

} // namespace eu::ui
