// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

#include <optional>
#include <utility>

namespace eu::core {

/// Clases de error que la aplicación sabe tratar. La interfaz decide qué mostrar
/// y qué ofrecer (reintentar, omitir…) según este código, no según el texto.
enum class ErrorCode {
    NotFound,          ///< El archivo o carpeta no existe.
    PermissionDenied,  ///< Faltan permisos.
    AlreadyExists,     ///< El destino ya existe.
    InvalidName,       ///< Nombre no válido en el sistema de archivos de destino.
    NotADirectory,     ///< Se esperaba una carpeta.
    DirectoryNotEmpty, ///< La carpeta no está vacía.
    NoSpace,           ///< No queda espacio en el volumen.
    ReadOnlyVolume,    ///< El volumen es de solo lectura.
    Busy,              ///< El archivo está en uso por otro proceso.
    Disconnected,      ///< La unidad o el recurso de red ya no está disponible.
    Timeout,           ///< Se agotó el tiempo de espera.
    Cancelled,         ///< El usuario canceló la tarea.
    NotSupported,      ///< La operación no existe en esta plataforma o ubicación.
    CrossDevice,       ///< Origen y destino están en volúmenes distintos.
    Corrupted,         ///< Datos dañados (base de datos, archivo comprimido…).
    IoError,           ///< Error de lectura o escritura sin clasificar.
    Unknown,
};

/// Un error con contexto suficiente para explicarlo al usuario y registrarlo.
struct Error
{
    ErrorCode code = ErrorCode::Unknown;
    QString message;   ///< Explicación legible, en el idioma de la interfaz.
    QString path;      ///< Ruta afectada, si aplica.
    int systemCode = 0; ///< errno (POSIX) o GetLastError (Windows); 0 si no aplica.
};

/// Nombre corto y estable del código, para el registro y las pruebas.
QString errorCodeName(ErrorCode code);

/// Convierte un `errno` de POSIX en un Error. `path` es la ruta afectada.
Error errorFromErrno(int errnoValue, const QString &path);

#ifdef Q_OS_WIN
/// Convierte un código de GetLastError() en un Error.
Error errorFromWindows(unsigned long lastError, const QString &path);
#endif

/// Resultado de una operación que puede fallar: contiene un valor o un Error.
///
/// Se usa en lugar de excepciones para que cada fallo sea visible en la firma
/// de la función y no pueda cruzar un hilo sin que nadie lo capture (SPEC §25.6).
///
///     Result<qint64> size = fileSize(path);
///     if (!size.ok()) { informar(size.error()); return; }
///     usar(size.value());
template <typename T>
class Result
{
public:
    Result(T value) : m_value(std::move(value)) {}   // NOLINT: conversión implícita deseada
    Result(Error error) : m_error(std::move(error)) {} // NOLINT

    bool ok() const { return m_value.has_value(); }
    explicit operator bool() const { return ok(); }

    /// Solo se puede llamar si ok() es true.
    const T &value() const { return *m_value; }
    T &value() { return *m_value; }

    /// Solo se puede llamar si ok() es false.
    const Error &error() const { return *m_error; }

private:
    std::optional<T> m_value;
    std::optional<Error> m_error;
};

/// Resultado de una operación que no devuelve nada: éxito o Error.
class Status
{
public:
    Status() = default;                                  ///< Éxito.
    Status(Error error) : m_error(std::move(error)) {}   // NOLINT

    bool ok() const { return !m_error.has_value(); }
    explicit operator bool() const { return ok(); }
    const Error &error() const { return *m_error; }

private:
    std::optional<Error> m_error;
};

} // namespace eu::core
