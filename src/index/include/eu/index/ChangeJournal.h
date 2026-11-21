// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"

#include <QString>
#include <QStringList>

#include <memory>

namespace eu::index {

/// Hasta dónde se leyó el diario de cambios de un volumen.
struct JournalPosition
{
    quint64 journal = 0;   ///< Identificador del diario: cambia si se borra y se vuelve a crear.
    qint64 usn = 0;        ///< Posición dentro de él.

    bool isValid() const { return journal != 0; }
    /// "diario:usn", para guardarlo; vacío si no es válida.
    QString toString() const;
    static JournalPosition fromString(const QString &text);
};

/// Las carpetas cuyo contenido cambió desde una posición.
struct JournalChanges
{
    QStringList folders;     ///< Rutas absolutas, con '/'.
    JournalPosition next;    ///< Desde dónde seguir la próxima vez.
};

/// Cuántos archivos y carpetas tiene un volumen, según su tabla de archivos.
struct VolumeCounts
{
    qint64 files = 0;
    qint64 folders = 0;
};

/// El diario de cambios del sistema de archivos: qué carpetas cambiaron
/// desde la última vez, sin recorrer el disco. Con él, el índice de una
/// unidad se pone al día al arrancar en segundos en vez de recorrerla entera.
///
/// En Windows lo da el servicio opcional eu-indexsvc, que lee el USN Journal
/// y la MFT de NTFS (SPEC §6.4). Si no está instalado, cada consulta falla y
/// el índice hace lo de siempre. Las consultas bloquean: se hacen desde una
/// tarea. Las implementaciones se pueden usar desde cualquier hilo.
class ChangeJournal
{
public:
    virtual ~ChangeJournal() = default;

    /// La unidad con diario donde está `path` ("C:"), o vacío si allí no hay
    /// (otro sistema de archivos, una unidad de red…).
    virtual QString volumeOf(const QString &path) const = 0;
    /// La posición actual del diario de `volume`.
    virtual core::Result<JournalPosition> position(const QString &volume) const = 0;
    /// Las carpetas que cambiaron desde `since`. Falla si el servicio no está,
    /// si el diario se volvió a crear, si lo pedido ya no está en él o si
    /// cambiaron más de `maxFolders` carpetas: en todos esos casos hay que
    /// recorrer la unidad.
    virtual core::Result<JournalChanges> changesSince(const QString &volume, const JournalPosition &since,
                                                      int maxFolders) const = 0;
    /// Archivos y carpetas del volumen, contados en su tabla (la MFT).
    virtual core::Result<VolumeCounts> counts(const QString &volume) const = 0;
};

/// El de este sistema: en Windows, el que habla con el servicio eu-indexsvc;
/// en los demás, ninguno (nullptr).
std::shared_ptr<ChangeJournal> systemChangeJournal();

/// El servicio de Windows, para mostrar su estado e instalarlo o quitarlo.
namespace journalservice {

enum class State {
    Unavailable,    ///< No es Windows, o falta eu-indexsvc.exe junto a la aplicación.
    NotInstalled,
    Stopped,        ///< Instalado pero parado.
    Running,
};

State state();

/// Un instalador o desinstalador lanzado con permiso de administrador.
class Setup
{
public:
    /// Pide permiso de administrador (el aviso de Windows) y lanza
    /// eu-indexsvc para instalarse (o desinstalarse). Error Cancelled si el
    /// usuario no dio permiso.
    static core::Result<Setup> start(bool install);

    bool isFinished() const;
    /// 0 si fue bien (solo tiene sentido cuando terminó).
    int exitCode() const;

private:
    std::shared_ptr<void> m_process;
};

} // namespace journalservice

} // namespace eu::index
