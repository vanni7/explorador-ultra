// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/tasks/Cancel.h"

#include <QHash>
#include <QList>
#include <QString>

#include <functional>

/// Comparación y sincronización de carpetas (SPEC §16). Este módulo solo lee
/// y planea: comparar dos árboles, decidir qué copiar o quitar y recordar el
/// estado de la última sincronización de cada par. Aplicar el plan lo hace el
/// motor de operaciones (ops::Engine::synchronize), con historial y papelera.
namespace eu::sync {

/// Cómo se decide que dos archivos con el mismo nombre son iguales.
enum class Criterion {
    Name,          ///< Basta con que estén los dos.
    Size,          ///< Mismo tamaño.
    SizeAndDate,   ///< Mismo tamaño y misma fecha de modificación (±2 s: FAT guarda de 2 en 2).
    Hash,          ///< Mismo tamaño y mismo SHA-256.
    Content,       ///< Mismo tamaño y mismos bytes.
};

/// Estado de un elemento al comparar (SPEC §16: idéntico, diferente, ausente,
/// más reciente y conflicto).
enum class State {
    Identical,
    Different,     ///< Distintos y sin un lado claramente más reciente.
    LeftNewer,
    RightNewer,
    LeftOnly,      ///< Ausente a la derecha.
    RightOnly,     ///< Ausente a la izquierda.
    TypeConflict,  ///< Un archivo en un lado y una carpeta en el otro.
};

struct Side
{
    bool exists = false;
    bool isDirectory = false;
    qint64 size = 0;           ///< Archivos: bytes. Carpetas que solo están en un lado: lo que suman.
    qint64 modifiedMs = 0;
};

struct Item
{
    QString relativePath;      ///< Con «/», sin la carpeta raíz.
    Side left;
    Side right;
    State state = State::Identical;
    /// Carpetas que están en los dos lados: true si algo de dentro difiere.
    bool differsInside = false;
};

struct CompareOptions
{
    Criterion criterion = Criterion::SizeAndDate;
    bool includeHidden = false;
    qint64 timeToleranceMs = 2000;
};

struct CompareProgress
{
    int foldersRead = 0;
    int filesCompared = 0;
    QString currentPath;
};

struct Comparison
{
    QString left;
    QString right;
    /// Por orden de ruta. Las carpetas que solo están en un lado aparecen una
    /// vez (sin su contenido); las que están en los dos, con lo de dentro.
    QList<Item> items;
    QList<core::Error> errors;   ///< Lo que no se pudo leer o comparar.
    int skippedLinks = 0;        ///< Los enlaces simbólicos no se comparan ni se siguen.
    bool cancelled = false;

    int count(State state) const;
    bool sameTrees() const;      ///< Ninguna diferencia (ni errores).
};

/// Compara los dos árboles. Bloqueante; se llama desde una tarea.
Comparison compare(const QString &left, const QString &right, const CompareOptions &options,
                   const tasks::CancelToken &cancel,
                   const std::function<void(const CompareProgress &)> &progress = {});

// ------------------------------------------------------------ sincronizar

enum class Direction {
    LeftToRight,   ///< Lo de la izquierda manda.
    RightToLeft,
    Both,          ///< Bidireccional, con el estado de la vez anterior.
};

/// Qué hacer cuando los dos lados cambiaron (bidireccional) o, en un sentido,
/// cuando el destino es más reciente.
enum class ConflictPolicy {
    Skip,          ///< No tocar ninguno (de fábrica).
    NewerWins,     ///< Gana el más reciente.
    LeftWins,
    RightWins,
};

/// Qué hacer con algo que se borró en un lado desde la última sincronización.
enum class DeletionChoice {
    Propagate,     ///< Quitarlo también del otro lado (a la papelera propia).
    Restore,       ///< Recuperarlo copiándolo desde el lado que aún lo tiene.
    Keep,          ///< Dejar los dos lados como están (y recordar el borrado).
};

struct SyncOptions
{
    Direction direction = Direction::LeftToRight;
    ConflictPolicy conflicts = ConflictPolicy::Skip;
    /// Quitar (a la papelera propia) lo que ya no está en el otro lado. Sin
    /// esto nunca se propaga un borrado (SPEC §16).
    bool propagateDeletions = false;
    /// Decisión por ruta para los borrados detectados (Action::deletion);
    /// manda sobre `propagateDeletions`. Sin decisión: en los dos sentidos se
    /// propaga o se deja según `propagateDeletions`; en un sentido, lo borrado
    /// en el origen igual, y lo borrado en el destino se recupera (es lo que
    /// pide un espejo).
    QHash<QString, DeletionChoice> deletionChoices;
};

enum class ActionKind {
    CopyToRight,   ///< Crear o reemplazar a la derecha (lo reemplazado va a la papelera).
    CopyToLeft,
    TrashLeft,     ///< A la papelera propia.
    TrashRight,
    Skip,          ///< Se deja como está (con su motivo).
};

struct Action
{
    QString relativePath;
    ActionKind kind = ActionKind::Skip;
    bool isDirectory = false;
    qint64 bytes = 0;          ///< Lo que se copiará.
    bool replaces = false;     ///< Hay algo en el destino que se reemplaza.
    bool conflict = false;     ///< Cambió en los dos lados (o el destino era más reciente).
    /// Bidireccional: se borró en un lado y no se propaga. Su entrada del
    /// estado anterior se conserva (si no, la próxima vez parecería nuevo).
    bool deletionKept = false;
    /// Estaba en los dos lados en la última sincronización, sigue igual en uno
    /// y en el otro ya no está: se borró ahí (se puede decidir con deletionChoices).
    bool deletion = false;
    bool deletedOnLeft = false;   ///< Con `deletion`: en qué lado se borró.
    QString reason;            ///< Por qué se hace o se omite, para enseñarlo.
};

struct Plan
{
    QList<Action> actions;     ///< Solo lo que cambia algo o se omite por una razón que hay que ver.
    qint64 bytesToCopy = 0;
    int conflicts = 0;         ///< Sin resolver (omitidos).
    int deletionsNotPropagated = 0;

    int count(ActionKind kind) const;
    bool hasChanges() const;
};

/// Lo que había en los dos lados al terminar la última sincronización
/// bidireccional: con eso se distingue «borrado en un lado» de «nuevo en el
/// otro» y «cambiado en un lado» de «cambiado en los dos».
struct SyncState
{
    struct Entry
    {
        bool isDirectory = false;
        qint64 size = 0;
        qint64 leftModifiedMs = 0;
        qint64 rightModifiedMs = 0;
    };
    QHash<QString, Entry> entries;   ///< Por ruta relativa.
    qint64 savedMs = 0;
    bool isEmpty() const { return entries.isEmpty(); }
};

/// Decide qué hacer. `previous` es el estado de la última sincronización del
/// par (puede ser null: primera vez). En bidireccional distingue lo cambiado
/// en un lado o en los dos; en cualquier sentido, lo borrado de lo nuevo.
Plan plan(const Comparison &comparison, const SyncOptions &options, const SyncState *previous,
          qint64 timeToleranceMs = 2000);

/// El estado a guardar tras sincronizar: lo idéntico en los dos lados según
/// `after` (la comparación hecha al terminar) y, de `previous`, lo que el plan
/// aplicado dejó como borrado sin propagar.
SyncState stateFrom(const Comparison &after, const Plan &applied, const SyncState *previous);

/// Dónde se guarda el estado de cada par de carpetas (una base por par).
/// `directory` es la carpeta de datos de la aplicación.
QString stateFile(const QString &directory, const QString &left, const QString &right);
core::Result<SyncState> loadState(const QString &file);
core::Status saveState(const QString &file, const SyncState &state);

} // namespace eu::sync
