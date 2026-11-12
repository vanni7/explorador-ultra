// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/core/FileName.h"

#include <QCoreApplication>
#include <QStringList>

namespace eu::core {

namespace {

/// Límite de un componente de ruta: 255 bytes UTF-8 en ext4/APFS y
/// 255 unidades UTF-16 en NTFS/exFAT.
constexpr int maxComponentLength = 255;

bool isWindowsReservedName(const QString &name)
{
    // Windows reserva estos nombres con o sin extensión: "NUL" y "nul.txt".
    static const QStringList reserved{
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),  QStringLiteral("NUL"),
        QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
        QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
        QStringLiteral("LPT8"), QStringLiteral("LPT9"),
    };
    const qsizetype dot = name.indexOf(QLatin1Char('.'));
    const QString stem = (dot < 0 ? name : name.left(dot)).trimmed().toUpper();
    return reserved.contains(stem);
}

} // namespace

NameRules nativeNameRules()
{
#if defined(Q_OS_WIN)
    return NameRules::Windows;
#elif defined(Q_OS_MACOS)
    return NameRules::MacOS;
#else
    return NameRules::Posix;
#endif
}

NameCheck checkFileName(const QString &name, NameRules rules)
{
    if (name.isEmpty()) {
        return {NameProblem::Empty, {}};
    }
    if (name == QLatin1String(".") || name == QLatin1String("..")) {
        return {NameProblem::DotOrDotDot, {}};
    }

    const bool windowsRules = (rules == NameRules::Windows || rules == NameRules::Portable);
    const bool macRules = (rules == NameRules::MacOS || rules == NameRules::Portable);

    for (const QChar ch : name) {
        const char16_t code = ch.unicode();
        bool invalid = (code == u'/' || code == u'\0');
        if (windowsRules) {
            invalid = invalid || code < 0x20 || code == u'\\' || code == u':' || code == u'*'
                || code == u'?' || code == u'"' || code == u'<' || code == u'>' || code == u'|';
        }
        if (macRules) {
            invalid = invalid || code == u':';
        }
        if (invalid) {
            return {NameProblem::InvalidCharacter, ch};
        }
    }

    if (windowsRules) {
        if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
            return {NameProblem::TrailingDotOrSpace, {}};
        }
        if (isWindowsReservedName(name)) {
            return {NameProblem::ReservedName, {}};
        }
    }

    // Se comprueban las dos medidas (UTF-16 y UTF-8) según las reglas pedidas.
    const bool tooLongUtf16 = name.size() > maxComponentLength;
    const bool tooLongUtf8 = name.toUtf8().size() > maxComponentLength;
    if ((windowsRules && tooLongUtf16) || (rules != NameRules::Windows && tooLongUtf8)) {
        return {NameProblem::TooLong, {}};
    }

    return {};
}

QString describeNameProblem(const NameCheck &check)
{
    const char *context = "eu::core::FileName";
    switch (check.problem) {
    case NameProblem::None:
        return {};
    case NameProblem::Empty:
        return QCoreApplication::translate(context, "El nombre no puede estar vacío.");
    case NameProblem::DotOrDotDot:
        return QCoreApplication::translate(context, "\".\" y \"..\" no se pueden usar como nombre.");
    case NameProblem::InvalidCharacter:
        return QCoreApplication::translate(context, "El nombre no puede contener el carácter \"%1\".")
            .arg(check.character.isPrint() ? QString(check.character) : QStringLiteral("?"));
    case NameProblem::ReservedName:
        return QCoreApplication::translate(context, "Ese nombre está reservado por Windows.");
    case NameProblem::TrailingDotOrSpace:
        return QCoreApplication::translate(context, "El nombre no puede terminar en punto ni en espacio.");
    case NameProblem::TooLong:
        return QCoreApplication::translate(context, "El nombre es demasiado largo.");
    }
    return {};
}

} // namespace eu::core
