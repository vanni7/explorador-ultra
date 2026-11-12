// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/core/SizeFormat.h"
#include "eu/fs/Entry.h"
#include "eu/tasks/Scheduler.h"
#include "eu/ui/EntrySort.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QIcon>
#include <QTimer>

#include <vector>

namespace eu::ui {

/// Modelo de la vista detallada: el contenido de una carpeta.
///
/// Todo el acceso a disco ocurre en tareas del planificador; este objeto vive
/// en el hilo de interfaz y solo recibe los resultados. Es virtualizado: la
/// vista pide los datos de las filas visibles, no se crea nada por elemento.
///
/// Carga de una carpeta:
///  1. Si el listado termina pronto (lo normal), se muestra de golpe ya ordenado.
///  2. Si tarda más de `revealDelayMs`, se van mostrando los elementos según
///     llegan (sin ordenar) y al final se reordenan, para que el usuario vea
///     avance en carpetas enormes o lentas.
class DirectoryModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum class State {
        Empty,    ///< Sin carpeta asignada.
        Loading,
        Ready,
        Failed,   ///< No se pudo leer la carpeta; ver lastError().
    };

    explicit DirectoryModel(tasks::Scheduler &scheduler, QObject *parent = nullptr);
    ~DirectoryModel() override;

    /// Cambia de carpeta. Cancela la carga anterior si seguía en curso.
    void setPath(const QString &path);
    QString path() const { return m_path; }

    /// Vuelve a leer la carpeta actual conservando lo que hay a la vista hasta
    /// que llega el contenido nuevo.
    void refresh();

    State state() const { return m_state; }
    const core::Error &lastError() const { return m_lastError; }

    void setShowHidden(bool show);
    bool showHidden() const { return m_showHidden; }

    void setSizeUnits(core::SizeUnits units);
    core::SizeUnits sizeUnits() const { return m_sizeUnits; }

    /// Tiempo a partir del cual una carga lenta empieza a mostrarse por partes.
    void setRevealDelayMs(int milliseconds) { m_revealTimer.setInterval(milliseconds); }

    /// Elemento de una fila. `row` debe ser válido.
    const fs::Entry &entryAt(int row) const { return m_entries[size_t(row)]; }
    /// Fila de un nombre, o -1 si no está.
    int rowOfName(const QString &name) const;
    /// Ruta completa de una fila.
    QString filePath(int row) const;

    int totalCount() const { return int(m_entries.size()); }
    int hiddenCount() const { return m_hiddenCount; }

    // --- QAbstractTableModel ---
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    /// Editar el nombre en la vista no cambia el modelo: pide el renombrado
    /// con renameRequested() y devuelve false. El nombre nuevo aparece cuando
    /// el disco cambia de verdad.
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

signals:
    void stateChanged(eu::ui::DirectoryModel::State state);
    /// El contenido se sustituyó (carga terminada o refresco). La vista debe
    /// restaurar la selección a partir de los nombres.
    void contentReplaced();
    /// El usuario escribió un nombre nuevo para un elemento.
    void renameRequested(const QString &path, const QString &newName);

private:
    struct TypeInfo
    {
        QString description;
        QIcon icon;
    };

    void startLoading(bool keepCurrentContent);
    void cancelRunningTask();
    void retireCurrentTask();
    void onBatch(quint64 generation, std::vector<fs::Entry> batch);
    void onFinished(quint64 generation, std::vector<fs::Entry> sorted, core::Status status);
    void revealPending();
    void replaceEntries(std::vector<fs::Entry> &&entries);
    void setState(State state);
    const TypeInfo &typeInfoFor(const fs::Entry &entry) const;
    static std::vector<fs::Entry> withoutHidden(std::vector<fs::Entry> entries, bool showHidden, int *hiddenCount);

    tasks::Scheduler &m_scheduler;
    tasks::TaskHandle m_task;
    /// Tareas canceladas que quizá sigan terminando. Capturan `this`, así que
    /// el destructor debe esperarlas.
    std::vector<tasks::TaskHandle> m_retiredTasks;
    quint64 m_generation = 0;   ///< Identifica la carga vigente; lo que llegue de otra se descarta.

    QString m_path;
    std::vector<fs::Entry> m_entries;   ///< Lo que la vista está mostrando.
    std::vector<fs::Entry> m_pending;   ///< Lotes recibidos y aún no mostrados.
    bool m_progressive = false;         ///< La carga actual ya se está mostrando por partes.
    bool m_keepCurrentContent = false;  ///< Refresco: no vaciar la vista mientras carga.
    int m_hiddenCount = 0;

    State m_state = State::Empty;
    core::Error m_lastError;
    Column m_sortColumn = Column::Name;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    bool m_showHidden = false;
    core::SizeUnits m_sizeUnits = core::SizeUnits::Decimal;
    QTimer m_revealTimer;

    mutable QHash<QString, TypeInfo> m_typeCache;   ///< Por extensión; evita repetir consultas.
};

} // namespace eu::ui
