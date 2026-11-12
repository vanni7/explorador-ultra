// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/FileClipboard.h"

#include <QDir>
#include <QMimeData>
#include <QUrl>

namespace eu::ui {

namespace {

// GNOME (Nautilus y derivados): primera línea "cut" o "copy", luego las URL.
const QString gnomeFormat = QStringLiteral("x-special/gnome-copied-files");
// KDE (Dolphin): "1" si se cortó.
const QString kdeCutFormat = QStringLiteral("application/x-kde-cutselection");
// Windows: un entero de 4 bytes; 2 = mover, 5 = copiar o enlazar.
const QString windowsEffectFormat = QStringLiteral("application/x-qt-windows-mime;value=\"Preferred DropEffect\"");
// Marca propia, por si el escritorio no tiene ninguna de las anteriores (macOS).
const QString ownCutFormat = QStringLiteral("application/x-explorador-ultra-cut");

} // namespace

QMimeData *makeFileMimeData(const QStringList &paths, bool cut)
{
    QList<QUrl> urls;
    QStringList nativePaths;
    QByteArray gnome = cut ? QByteArrayLiteral("cut") : QByteArrayLiteral("copy");
    for (const QString &path : paths) {
        const QUrl url = QUrl::fromLocalFile(path);
        urls.append(url);
        nativePaths.append(QDir::toNativeSeparators(path));
        gnome += '\n' + url.toEncoded();
    }

    auto *data = new QMimeData;
    data->setUrls(urls);
    // Texto plano: al pegar en un editor o una terminal salen las rutas.
    data->setText(nativePaths.join(QLatin1Char('\n')));
    data->setData(gnomeFormat, gnome);
    data->setData(kdeCutFormat, cut ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));
    data->setData(windowsEffectFormat, QByteArray(cut ? "\x02\x00\x00\x00" : "\x05\x00\x00\x00", 4));
    data->setData(ownCutFormat, cut ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));
    return data;
}

FileSelection fileSelectionFrom(const QMimeData *data)
{
    FileSelection selection;
    if (data == nullptr || !data->hasUrls()) {
        return selection;
    }
    const QList<QUrl> urls = data->urls();
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            selection.paths.append(QDir::cleanPath(url.toLocalFile()));
        }
    }

    if (data->hasFormat(ownCutFormat)) {
        selection.cut = data->data(ownCutFormat) == "1";
    } else if (data->hasFormat(gnomeFormat)) {
        selection.cut = data->data(gnomeFormat).startsWith("cut");
    } else if (data->hasFormat(kdeCutFormat)) {
        selection.cut = data->data(kdeCutFormat) == "1";
    } else if (data->hasFormat(windowsEffectFormat)) {
        const QByteArray effect = data->data(windowsEffectFormat);
        selection.cut = !effect.isEmpty() && (effect.at(0) & 0x02) != 0;
    }
    return selection;
}

} // namespace eu::ui
