// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/tasks/Cancel.h"

#include <QCryptographicHash>
#include <QString>

#include <functional>
#include <memory>

namespace eu::ops {

/// Operaciones elementales sobre el disco. Son bloqueantes: se llaman desde
/// tareas del planificador. Ninguna sigue enlaces simbólicos ni sobrescribe
/// nada que ya exista (SPEC §8.3): si el destino existe, fallan con AlreadyExists.

/// Huella de un elemento, para comprobar más tarde si sigue igual.
struct FileStamp
{
    bool exists = false;
    bool isDirectory = false;   ///< Carpeta real (un enlace a carpeta no cuenta).
    bool isSymlink = false;
    qint64 size = -1;           ///< Solo para archivos normales.
    qint64 modifiedMs = 0;      ///< Solo para archivos normales.
};

FileStamp stampOf(const QString &path);

core::Status makeDirectory(const QString &path);
core::Status createEmptyFile(const QString &path);

/// Renombra o mueve dentro del mismo volumen, de forma atómica y sin
/// sobrescribir. Entre volúmenes falla con CrossDevice.
core::Status renameNoReplace(const QString &source, const QString &target);

/// Se llama con los bytes escritos desde el aviso anterior.
using CopyProgress = std::function<void(qint64 bytesJustWritten)>;

/// Algoritmos para comprobar que una copia es igual al original (SPEC §13).
enum class HashAlgorithm {
    Sha256,   ///< El de fábrica.
    Sha1,
    Md5,
    Crc32,
};
/// "SHA-256", "MD5"…
QString hashAlgorithmName(HashAlgorithm algorithm);
/// Qué no garantiza (vacío para SHA-256), para decirlo junto a la opción.
QString hashAlgorithmLimitation(HashAlgorithm algorithm);

/// Calcula un hash por partes.
class Hasher
{
public:
    explicit Hasher(HashAlgorithm algorithm);
    ~Hasher();
    Hasher(const Hasher &) = delete;
    Hasher &operator=(const Hasher &) = delete;

    void addData(const char *data, qint64 length);
    QByteArray result();

private:
    HashAlgorithm m_algorithm;
    std::unique_ptr<QCryptographicHash> m_hash;
    quint32 m_crc = 0xFFFFFFFFu;
};

/// El hash de un archivo, leído del disco (no de la caché del sistema cuando
/// se puede pedir: Linux y macOS), para comprobar lo que de verdad quedó escrito.
core::Result<QByteArray> hashFile(const QString &path, HashAlgorithm algorithm, const tasks::CancelToken &cancel);

/// Solo para pruebas: las próximas `times` copias escriben un byte cambiado,
/// como haría un disco o un cable defectuoso.
void corruptNextCopiesForTesting(int times);

struct CopyOptions
{
    /// Comprobar la copia: el hash del original se calcula al leerlo y el de
    /// la copia, releyéndola del disco antes de darle su nombre. Si no
    /// coinciden se repite la copia una vez; si vuelve a fallar, error.
    bool verify = false;
    HashAlgorithm algorithm = HashAlgorithm::Sha256;
    /// Se llama entre bloque y bloque: si la transferencia está en pausa,
    /// espera ahí (y vuelve si se cancela). Vacío: sin pausa.
    std::function<void(const tasks::CancelToken &)> pausePoint;
};

/// Sufijo del archivo temporal que usa copyFile mientras escribe.
QString partialSuffix();

/// Copia un archivo normal. Escribe en `<target>.eu-parcial`, sincroniza a
/// disco, copia permisos y fecha, y solo entonces lo renombra al nombre final:
/// así nunca queda un destino a medias con el nombre bueno (SPEC §8.3).
/// Si se cancela o falla, borra el temporal.
core::Status copyFile(const QString &source, const QString &target, const tasks::CancelToken &cancel,
                      const CopyProgress &progress = {}, const CopyOptions &options = {});

/// Copia un enlace simbólico como enlace (no copia aquello a lo que apunta).
core::Status copySymlink(const QString &source, const QString &target);

/// Borra un archivo o un enlace (el enlace, no su destino). Definitivo.
core::Status removeFile(const QString &path);
/// Borra una carpeta vacía. Si tiene contenido falla con DirectoryNotEmpty.
core::Status removeEmptyDirectory(const QString &path);

/// Cómo saber si dos archivos son iguales. Nunca basta con el tamaño (SPEC §13).
enum class Comparison {
    Hash,    ///< SHA-256 de los dos.
    Bytes,   ///< Byte a byte (lee los dos a la vez; para en la primera diferencia).
};

/// true si `a` y `b` tienen el mismo contenido: mismo tamaño y, además, mismo
/// SHA-256 o mismos bytes. Dos enlaces simbólicos son iguales si apuntan al
/// mismo sitio. Una carpeta nunca es igual a nada.
core::Result<bool> sameContent(const QString &a, const QString &b, Comparison comparison,
                               const tasks::CancelToken &cancel);

/// "foto.jpg" con la fecha `ms` -> "foto 2026-11-16 14.05.33.jpg" (hora local).
QString nameWithDateTime(const QString &name, qint64 ms);

/// Devuelve un nombre libre en `directory` a partir de `name`:
/// "foto.jpg" -> "foto (2).jpg", "foto (3).jpg"… Si `name` está libre, lo devuelve tal cual.
QString uniqueName(const QString &directory, const QString &name);

} // namespace eu::ops
