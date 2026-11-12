// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/SizeFormat.h"
#include "eu/fs/Volumes.h"

#include <QIcon>
#include <QTreeWidget>

namespace eu::ui {

/// Icono de una unidad según su tipo (disco, extraíble, red, óptico).
QIcon iconForVolume(const fs::Volume &volume);

/// Lista de unidades con su espacio: tamaño total, ocupado, libre y una barra
/// de uso. Es lo que muestra una pestaña en la ubicación "Unidades".
///
/// No consulta el disco: recibe la lista ya leída con setVolumes().
class VolumesView : public QTreeWidget
{
    Q_OBJECT

public:
    enum Column {
        NameColumn,
        LocationColumn,
        TypeColumn,
        TotalColumn,
        UsedColumn,
        FreeColumn,
        UsageColumn,
        ColumnCount,
    };

    explicit VolumesView(QWidget *parent = nullptr);

    /// Sustituye la lista. Conserva la unidad seleccionada si sigue estando.
    void setVolumes(const QList<fs::Volume> &volumes);
    QList<fs::Volume> volumes() const { return m_volumes; }

    void setSizeUnits(core::SizeUnits units);

    /// Deja seleccionada la unidad con este punto de montaje, si está.
    void selectRoot(const QString &rootPath);
    /// Punto de montaje de la unidad seleccionada, o vacío.
    QString selectedRoot() const;

signals:
    /// El usuario abrió una unidad (doble clic o Intro).
    void volumeActivated(const QString &rootPath);

private:
    void rebuild();

    QList<fs::Volume> m_volumes;
    core::SizeUnits m_units = core::SizeUnits::Decimal;
};

} // namespace eu::ui
