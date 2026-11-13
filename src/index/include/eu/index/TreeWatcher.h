// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>

#include <memory>

namespace eu::index {

/// Avisa de los cambios en una carpeta y en todo lo que cuelga de ella, para
/// mantener el índice al día (SPEC §6.4). Usa lo que ofrece cada sistema:
///
/// - Linux: inotify, que vigila carpeta a carpeta. Hay que pedirle cada una
///   con addFolder() y el sistema limita cuántas (`fs.inotify.max_user_watches`).
///   Si se alcanza el límite, isLimited() pasa a true y se emite limitReached():
///   las carpetas que no caben quedan sin vigilar.
/// - Windows: ReadDirectoryChangesW sobre la raíz, con todo el árbol.
/// - macOS: FSEvents sobre la raíz, con todo el árbol.
///
/// Solo dice *qué carpeta* cambió; quien escucha la vuelve a comparar con el
/// índice. Los avisos llegan al hilo del objeto.
class TreeWatcher : public QObject
{
    Q_OBJECT

public:
    /// Crea el vigilante de `root` para este sistema. Nunca devuelve nullptr:
    /// si no hay forma de vigilar, isActive() es false.
    static std::unique_ptr<TreeWatcher> create(const QString &root);

    ~TreeWatcher() override;

    const QString &root() const { return m_root; }

    /// true si el sistema aceptó vigilar.
    virtual bool isActive() const = 0;
    /// Pide vigilar una carpeta del árbol. Se puede llamar desde cualquier
    /// hilo. Donde el sistema vigila el árbol entero no hace nada.
    virtual void addFolder(const QString &folder) = 0;
    /// true si hay carpetas sin vigilar porque el sistema no admite más.
    virtual bool isLimited() const { return false; }
    /// Cuántas carpetas se vigilan una a una (0 donde se vigila el árbol entero).
    virtual qint64 watchedFolders() const { return 0; }

signals:
    /// Cambió el contenido de esta carpeta (algo se creó, borró, renombró o se
    /// terminó de escribir).
    void folderChanged(const QString &folder);
    /// El sistema descartó avisos: hay que volver a recorrer el árbol.
    void overflowed();
    /// Se alcanzó el límite de carpetas vigiladas (solo Linux).
    void limitReached();

protected:
    explicit TreeWatcher(QString root);

private:
    QString m_root;
};

} // namespace eu::index
