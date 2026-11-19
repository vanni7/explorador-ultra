// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/tasks/Scheduler.h"
#include "eu/ui/FileClipboard.h"

#include <QHash>
#include <QListView>
#include <QPointer>
#include <QScrollArea>
#include <QTimer>

#include <functional>

class QHBoxLayout;
class QItemSelectionModel;

namespace eu::ui {

class DirectoryModel;

/// Una columna de la vista Miller: los nombres de una carpeta, con «›» en las
/// subcarpetas. Acepta archivos soltados (sobre una subcarpeta o sobre la
/// carpeta de la columna) y deja arrastrar lo seleccionado.
class MillerColumn : public QListView
{
    Q_OBJECT

public:
    MillerColumn(DirectoryModel *model, QWidget *parent = nullptr);

    DirectoryModel *directoryModel() const { return m_model; }

    MoveByDefaultRule moveByDefault;
    std::function<void(const QStringList &, const QString &, bool)> onDrop;
    /// Se llama al empezar a renombrar en esta columna.
    std::function<void()> onEditingStarted;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    DropPlan planFor(const QDropEvent *event, int *targetRow) const;
    void setDropTarget(bool active, int row);

    DirectoryModel *m_model;
    bool m_dropActive = false;
    int m_dropRow = -1;
};

/// Columnas Miller (SPEC §5.6): una columna por cada carpeta de la ruta, de la
/// raíz a la actual, y a la derecha el contenido de la subcarpeta que tiene
/// el cursor. Se desplaza en horizontal y cada columna se puede ensanchar
/// arrastrando su borde derecho.
///
/// La columna de la carpeta actual usa el modelo y la selección del panel, así
/// que todo lo que trabaja con la selección (copiar, renombrar, el menú, la
/// vista previa) funciona igual que en las otras vistas. Las demás columnas
/// tienen su propio modelo y sirven para ver y para saltar: elegir algo en
/// ellas pide navegar (navigateRequested).
class MillerView : public QScrollArea
{
    Q_OBJECT

public:
    MillerView(tasks::Scheduler &scheduler, DirectoryModel *currentModel, QItemSelectionModel *selection,
               QWidget *parent = nullptr);

    /// La columna de la carpeta actual (la que comparte modelo y selección).
    MillerColumn *currentColumn() const { return m_current; }

    /// La carpeta actual cambió: rehace las columnas de la izquierda. Las que
    /// siguen igual se conservan (con su desplazamiento).
    void setPath(const QString &path);
    /// La subcarpeta con el cursor en la columna actual; vacío si el cursor
    /// está en un archivo o en nada. Su contenido se ve a la derecha.
    void showChild(const QString &folder);
    void setShowHidden(bool show);
    void setMoveByDefaultRule(MoveByDefaultRule rule);
    void setIconSize(int size);
    std::function<void(const QStringList &, const QString &, bool)> onDrop;

    /// Ancho de las columnas que no se han ensanchado a mano.
    static constexpr int defaultColumnWidth = 230;

    /// Para las pruebas: la carpeta de cada columna, de izquierda a derecha
    /// (con la actual y, si se ve, la de la derecha).
    QStringList columnPaths() const;
    MillerColumn *columnFor(const QString &folder) const;

signals:
    /// Se eligió `name` en la columna de `folder` (una de la izquierda o la de
    /// la derecha): el panel debe ir a `folder` y dejar `name` seleccionado.
    void navigateRequested(const QString &folder, const QString &name);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Level
    {
        QString path;
        DirectoryModel *model = nullptr;   ///< Propio (nullptr en la actual).
        MillerColumn *view = nullptr;
        QWidget *grip = nullptr;
        QString selectedName;              ///< Lo que se marca en ella: el siguiente tramo de la ruta.
    };

    Level makeColumn(const QString &path, const QString &selectedName);
    QWidget *makeGrip(MillerColumn *column);
    void destroyColumn(Level &column);
    void ensureChildColumn();
    void relayout();
    void markSelected(Level &column);
    void revealEnd();
    int widthFor(const QString &path) const;

    tasks::Scheduler &m_scheduler;
    DirectoryModel *m_currentModel;
    MillerColumn *m_current = nullptr;
    QWidget *m_currentGrip = nullptr;
    QList<Level> m_ancestors;
    Level m_child;                        ///< La de la derecha (se reutiliza; oculta si no hay).
    QString m_childFolder;                 ///< Lo que debe verse a la derecha.
    QTimer m_childTimer;
    QWidget *m_strip = nullptr;
    QHBoxLayout *m_layout = nullptr;
    QString m_path;
    bool m_showHidden = false;
    int m_iconSize = 0;   ///< 0: el de fábrica del estilo.
    MoveByDefaultRule m_moveByDefault;
    QHash<QString, int> m_widths;          ///< Anchos puestos a mano, por carpeta.
    QHash<QObject *, MillerColumn *> m_gripColumns;   ///< Cada borde, con la columna que ensancha.
    QPointer<MillerColumn> m_dragColumn;   ///< La que se está ensanchando.
    int m_dragStartX = 0;
    int m_dragStartWidth = 0;
};

} // namespace eu::ui
