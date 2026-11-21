// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/ChangeJournal.h"

#include <QCoreApplication>

namespace eu::index {

QString JournalPosition::toString() const
{
    return isValid() ? QString::number(journal) + QLatin1Char(':') + QString::number(usn) : QString();
}

JournalPosition JournalPosition::fromString(const QString &text)
{
    const qsizetype colon = text.indexOf(QLatin1Char(':'));
    if (colon <= 0) {
        return {};
    }
    bool journalOk = false;
    bool usnOk = false;
    JournalPosition position;
    position.journal = text.left(colon).toULongLong(&journalOk);
    position.usn = text.mid(colon + 1).toLongLong(&usnOk);
    return journalOk && usnOk && position.usn >= 0 ? position : JournalPosition{};
}

#ifndef Q_OS_WIN

// Fuera de Windows no hay servicio: el índice sigue los cambios con
// inotify o FSEvents y, al arrancar, recorre la carpeta.

std::shared_ptr<ChangeJournal> systemChangeJournal()
{
    return nullptr;
}

namespace journalservice {

State state()
{
    return State::Unavailable;
}

core::Result<Setup> Setup::start(bool)
{
    return core::Error{core::ErrorCode::NotSupported,
                       QCoreApplication::translate("eu::index", "Solo existe en Windows"), QString(), 0};
}

bool Setup::isFinished() const
{
    return true;
}

int Setup::exitCode() const
{
    return -1;
}

} // namespace journalservice

#endif

} // namespace eu::index
