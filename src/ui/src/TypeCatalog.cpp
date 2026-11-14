// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/TypeCatalog.h"

#include "eu/ui/EntrySort.h"

#include <QApplication>
#include <QMimeDatabase>
#include <QStyle>

namespace eu::ui {

namespace {
const QString folderKey = QStringLiteral("/carpeta");
const QString linkBrokenKey = QStringLiteral("/enlace-roto");
} // namespace

const TypeCatalog::Info &TypeCatalog::infoFor(const fs::Entry &entry) const
{
    QString key;
    if (entry.isBrokenSymlink) {
        key = linkBrokenKey;
    } else if (entry.isDirectory) {
        key = folderKey;
    } else {
        key = suffixOf(entry.name);
    }

    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        return it.value();
    }

    // Se deduce el tipo solo por el nombre (sin abrir el archivo): no hay E/S.
    // Icono: el del tema del escritorio si lo tiene; si no, el genérico del
    // estilo de Qt, que existe siempre. Así ninguna fila queda sin icono.
    const auto iconFor = [](const QStringList &themeNames, QStyle::StandardPixmap fallback) {
        for (const QString &name : themeNames) {
            if (!name.isEmpty() && QIcon::hasThemeIcon(name)) {
                return QIcon::fromTheme(name);
            }
        }
        return QApplication::style()->standardIcon(fallback);
    };
    Info info;
    if (key == folderKey) {
        info.description = QCoreApplication::translate("eu::ui::DirectoryModel", "Carpeta");
        info.icon = iconFor({QStringLiteral("folder"), QStringLiteral("inode-directory")}, QStyle::SP_DirIcon);
    } else if (key == linkBrokenKey) {
        info.description = QCoreApplication::translate("eu::ui::DirectoryModel", "Enlace roto");
        info.icon = iconFor({QStringLiteral("emblem-unreadable"), QStringLiteral("dialog-warning")},
                            QStyle::SP_MessageBoxWarning);
    } else {
        static const QMimeDatabase mimeDatabase;
        const QMimeType mime = mimeDatabase.mimeTypeForFile(entry.name, QMimeDatabase::MatchExtension);
        info.description = mime.isDefault() ? (key.isEmpty() ? QCoreApplication::translate("eu::ui::DirectoryModel", "Archivo") : QCoreApplication::translate("eu::ui::DirectoryModel", "Archivo %1").arg(key.toUpper()))
                                            : mime.comment();
        info.icon = iconFor({mime.iconName(), mime.genericIconName(), QStringLiteral("text-x-generic")},
                            QStyle::SP_FileIcon);
    }
    return m_cache.insert(key, info).value();
}

} // namespace eu::ui
