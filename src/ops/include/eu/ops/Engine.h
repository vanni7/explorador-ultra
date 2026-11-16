// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/core/FileName.h"
#include "eu/journal/Journal.h"
#include "eu/ops/FileOps.h"
#include "eu/tasks/Cancel.h"
#include "eu/trash/Trash.h"

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace eu::ops {

/// Qué hacer cuando en el destino ya hay algo con el mismo nombre (SPEC §13).
/// Si los dos son carpetas, las que reemplazan o comparan juntan el contenido
/// y deciden archivo por archivo.
enum class CollisionPolicy {
    Skip,             ///< Dejar el destino como está y no copiar ese elemento.
    AutoRename,       ///< Copiar con otro nombre: "foto (2).jpg".
    Overwrite,        ///< Reemplazar. Lo reemplazado va a la papelera propia:
                      ///< se puede deshacer. Si su unidad no tiene papelera, no se reemplaza.
    OverwriteIfNewer, ///< Reemplazar solo si el origen es más reciente; si no, omitir.
    AppendDateTime,   ///< Copiar con la fecha del original: "foto 2026-11-16 14.05.33.jpg".
    SkipIfIdentical,  ///< Omitir si el contenido es idéntico; si no, copiar con otro nombre.
    Ask,              ///< Preguntar en cada caso (TransferOptions::ask).
};

/// Un choque de nombres, para preguntar qué hacer.
struct Conflict
{
    QString source;
    QString target;
    FileStamp sourceStamp;
    FileStamp targetStamp;
};

/// La respuesta a un choque.
struct CollisionDecision
{
    CollisionPolicy policy = CollisionPolicy::Skip;   ///< Cualquiera menos Ask.
    bool applyToAll = false;   ///< Usarla también en los choques que queden.
    bool cancel = false;       ///< Parar aquí la operación.
};
/// Se llama desde el hilo de la operación y espera la respuesta (o a que se
/// cancele la operación: entonces debe volver enseguida).
using CollisionAsker = std::function<CollisionDecision(const Conflict &, const tasks::CancelToken &)>;

/// Cómo copiar o mover: qué hacer con los choques y si se comprueba cada copia.
struct TransferOptions
{
    TransferOptions(CollisionPolicy chosen = CollisionPolicy::AutoRename) : policy(chosen) {}   // NOLINT
    CollisionPolicy policy;
    Comparison comparison = Comparison::Hash;   ///< Para SkipIfIdentical.
    CollisionAsker ask;                         ///< Para Ask. Sin él, Ask renombra.
    /// Comprobar cada archivo copiado releyéndolo (también al mover entre
    /// unidades, antes de borrar el original). Ver CopyOptions.
    CopyOptions copy;
    /// Si devuelve true cuando la operación se cancela, no se da por cerrada
    /// en el historial: queda «interrumpida» y al volver a abrir se ofrece
    /// reanudarla. La interfaz lo usa al cerrarse con copias en marcha.
    std::function<bool()> leaveOpenIfCancelled;
};

/// Avance de una operación larga.
struct Progress
{
    qint64 bytesDone = 0;
    qint64 bytesTotal = 0;
    int itemsDone = 0;
    int itemsTotal = 0;
    QString currentPath;
};
using ProgressCallback = std::function<void(const Progress &)>;

/// Resultado de una operación.
struct Outcome
{
    journal::OperationId operationId = 0;   ///< 0 si no llegó a anotarse en el historial.
    QStringList resultPaths;   ///< Lo creado o movido: rutas finales (para seleccionarlas).
    QStringList skipped;       ///< Elementos omitidos por colisión de nombre (incluye los idénticos).
    QStringList identical;     ///< De ellos, los omitidos por ser idénticos al destino.
    QStringList replaced;      ///< Destinos reemplazados (lo que había está en la papelera).
    int verified = 0;          ///< Archivos copiados y comprobados con su hash.
    /// Copiar y mover: lo que no se terminó (error o cancelación), con el
    /// destino exacto que se eligió, para reintentarlo con Engine::retry.
    QList<QPair<QString, QString>> unfinished;
    QList<core::Error> errors; ///< Lo que no se pudo hacer, con su causa.
    bool cancelled = false;

    bool ok() const { return errors.isEmpty() && !cancelled; }
};

/// Reglas de nombres del volumen donde está `directory` (un disco NTFS o
/// exFAT montado en Linux exige las de Windows).
core::NameRules nameRulesFor(const QString &directory);

/// Motor de operaciones de archivos. Cada método hace una operación completa:
/// la anota en el historial paso a paso **antes** de tocar el disco (SPEC §9)
/// y devuelve qué pasó. Son bloqueantes: se llaman desde una tarea.
///
/// Garantías: nunca se sobrescribe un elemento existente, nunca se siguen
/// enlaces simbólicos, y al mover entre unidades el origen solo se borra
/// después de copiar y sincronizar a disco (SPEC §8.3).
class Engine
{
public:
    explicit Engine(std::shared_ptr<journal::Journal> journal);

    Outcome createFolder(const QString &parentDirectory, const QString &name);
    Outcome createFile(const QString &parentDirectory, const QString &name);
    Outcome rename(const QString &path, const QString &newName);
    /// Renombra muchos a la vez (renombrado masivo, SPEC §11) como una sola
    /// operación del historial: deshacer los devuelve todos. Cadenas y ciclos
    /// (a→b, b→a) y cambios solo de mayúsculas pasan por nombres temporales.
    /// Nunca sobrescribe: si un nombre final ya está ocupado, ese elemento
    /// vuelve a su nombre y se informa.
    Outcome renameMany(const QList<QPair<QString, QString>> &items, const tasks::CancelToken &cancel);

    Outcome copy(const QStringList &sources, const QString &destinationDirectory, const TransferOptions &collisions,
                 const tasks::CancelToken &cancel, const ProgressCallback &progress = {});
    Outcome move(const QStringList &sources, const QString &destinationDirectory, const TransferOptions &collisions,
                 const tasks::CancelToken &cancel, const ProgressCallback &progress = {});

    /// Reintenta lo que quedó sin terminar (`Outcome::unfinished`): cada
    /// origen va a su destino exacto. Lo que ya llegó entero no se vuelve a
    /// copiar (se compara su contenido) y lo que quedó a medias se completa.
    Outcome retry(const QList<QPair<QString, QString>> &items, bool moving, const TransferOptions &options,
                  const tasks::CancelToken &cancel, const ProgressCallback &progress = {});

    /// Reanuda una copia o un movimiento que quedó a medias por un cierre
    /// inesperado (SPEC §9): con el plan guardado en el historial, hace lo que
    /// falta (lo ya llegado no se repite; el archivo cortado sigue desde su
    /// punto de control). La operación interrumpida queda cerrada.
    Outcome resumeInterrupted(journal::OperationId operationId, const TransferOptions &options,
                              const tasks::CancelToken &cancel, const ProgressCallback &progress = {});

    /// Envía a la papelera propia. Si un volumen no admite papelera, ese
    /// elemento queda en `errors` con código NotSupported y no se toca.
    Outcome trash(const QStringList &paths, const tasks::CancelToken &cancel);
    /// Eliminación definitiva. No se puede deshacer.
    Outcome deletePermanently(const QStringList &paths, const tasks::CancelToken &cancel);

    /// Devuelve elementos de la papelera a su ubicación original. No
    /// sobrescribe: si ahí ya hay algo con ese nombre, ese elemento se queda
    /// en la papelera y se informa en `errors`. No se deshace con Deshacer
    /// (basta con volver a enviarlo a la papelera).
    Outcome restoreFromTrash(const QList<trash::TrashItem> &items, const tasks::CancelToken &cancel);
    /// Elimina para siempre elementos que están en la papelera.
    Outcome purgeFromTrash(const QList<trash::TrashItem> &items, const tasks::CancelToken &cancel);

    /// true si la operación se puede deshacer (terminó, no era una eliminación
    /// permanente ni una restauración de la papelera, y no se deshizo ya).
    static bool canUndo(const journal::Operation &operation);

    /// Deshace una operación. Antes de cada paso comprueba que el disco está
    /// como lo dejó la operación; lo que cambió desde fuera no se toca y se
    /// informa en `errors` (SPEC §9).
    Outcome undo(journal::OperationId operationId, const tasks::CancelToken &cancel);

    /// Recuperación tras un cierre inesperado (SPEC §9): revierte lo que una
    /// operación interrumpida alcanzó a hacer y limpia sus restos.
    Outcome revertInterrupted(journal::OperationId operationId);
    /// Da por perdida una operación interrumpida: limpia los archivos a medio
    /// escribir y deja hecho lo que ya estaba hecho.
    void discardInterrupted(journal::OperationId operationId);

private:
    class Run;
    Outcome transferAll(const QStringList &sources, const QString &destinationDirectory,
                        const TransferOptions &collisions, const tasks::CancelToken &cancel,
                        const ProgressCallback &progress, bool moving);
    std::shared_ptr<journal::Journal> m_journal;
};

} // namespace eu::ops
