// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/SizeFormat.h"
#include "eu/search/Searcher.h"
#include "eu/tasks/Scheduler.h"
#include "eu/ui/TypeCatalog.h"

#include <QAbstractTableModel>
#include <QStringList>

#include <vector>

namespace eu::ui {

/// Modelo de la lista de resultados de una búsqueda.
///
/// Los resultados se añaden según llegan, en el orden en que se encuentran
/// (primero lo más cercano a la carpeta de partida). Al pulsar un encabezado
/// se ordenan en segundo plano: con cientos de miles de resultados, ordenar
/// en el hilo de interfaz lo congelaría.
class SearchResultsModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        NameColumn,
        FolderColumn,
        SizeColumn,
        TypeColumn,
        ModifiedColumn,
        OriginColumn,   ///< "Índice" o "Disco": de dónde sale el resultado.
        ColumnCount,
    };

    explicit SearchResultsModel(tasks::Scheduler &scheduler, QObject *parent = nullptr);

    /// Carpeta de partida: la columna "Carpeta" se muestra relativa a ella.
    void setRoot(const QString &root);
    /// Los resultados vienen del índice (no se han comprobado en el disco) o
    /// de recorrer el disco. Vale para todos los de la búsqueda.
    void setFromIndex(bool fromIndex);
    bool isFromIndex() const { return m_fromIndex; }
    void clear();
    void append(std::vector<search::Hit> &&hits);
    /// Vuelve a aplicar el orden elegido (tras añadir resultados).
    void resort();
    /// Comprueba en segundo plano qué resultados ya no existen (se borraron o
    /// movieron) y los quita de la lista.
    void pruneMissing();

    void setSizeUnits(core::SizeUnits units);

    int count() const { return int(m_hits.size()); }
    const search::Hit &hitAt(int row) const { return m_hits[size_t(row)]; }
    QString filePath(int row) const { return m_hits[size_t(row)].path(); }
    /// Fila de una ruta completa, o -1.
    int rowOfPath(const QString &path) const;

    // --- QAbstractTableModel ---
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    /// `column` < 0: sin ordenar (el orden en que se encontraron).
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    Qt::DropActions supportedDragActions() const override;

signals:
    /// La lista se sustituyó entera (orden nuevo o resultados retirados).
    void contentReplaced();

private:
    QString folderText(const search::Hit &hit) const;

    tasks::Scheduler &m_scheduler;
    std::vector<search::Hit> m_hits;
    QString m_root;
    bool m_fromIndex = false;
    core::SizeUnits m_units = core::SizeUnits::Decimal;
    TypeCatalog m_types;
    int m_sortColumn = -1;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    /// Identifica el contenido vigente: lo que una tarea calculó sobre otro
    /// contenido (orden, comprobación de existencia) se descarta.
    quint64 m_generation = 0;
};

} // namespace eu::ui
