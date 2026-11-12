// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/core/Error.h"

#include <QCoreApplication>

#include <cerrno>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace eu::core {

namespace {

/// Texto para el usuario de cada clase de error. Se traduce con Qt Linguist.
QString describe(ErrorCode code)
{
    const char *context = "eu::core::Error";
    switch (code) {
    case ErrorCode::NotFound:          return QCoreApplication::translate(context, "El archivo o la carpeta no existe.");
    case ErrorCode::PermissionDenied:  return QCoreApplication::translate(context, "No tienes permiso para hacer esto.");
    case ErrorCode::AlreadyExists:     return QCoreApplication::translate(context, "Ya existe un elemento con ese nombre.");
    case ErrorCode::InvalidName:       return QCoreApplication::translate(context, "El nombre no es válido.");
    case ErrorCode::NotADirectory:     return QCoreApplication::translate(context, "No es una carpeta.");
    case ErrorCode::DirectoryNotEmpty: return QCoreApplication::translate(context, "La carpeta no está vacía.");
    case ErrorCode::NoSpace:           return QCoreApplication::translate(context, "No queda espacio en la unidad.");
    case ErrorCode::ReadOnlyVolume:    return QCoreApplication::translate(context, "La unidad es de solo lectura.");
    case ErrorCode::Busy:              return QCoreApplication::translate(context, "El archivo está en uso por otro programa.");
    case ErrorCode::Disconnected:      return QCoreApplication::translate(context, "La unidad ya no está disponible.");
    case ErrorCode::Timeout:           return QCoreApplication::translate(context, "Se agotó el tiempo de espera.");
    case ErrorCode::Cancelled:         return QCoreApplication::translate(context, "Operación cancelada.");
    case ErrorCode::NotSupported:      return QCoreApplication::translate(context, "Esta operación no está disponible aquí.");
    case ErrorCode::CrossDevice:       return QCoreApplication::translate(context, "El origen y el destino están en unidades distintas.");
    case ErrorCode::Corrupted:         return QCoreApplication::translate(context, "Los datos están dañados.");
    case ErrorCode::IoError:           return QCoreApplication::translate(context, "Error de lectura o escritura.");
    case ErrorCode::Unknown:           break;
    }
    return QCoreApplication::translate(context, "Error desconocido.");
}

} // namespace

QString errorCodeName(ErrorCode code)
{
    switch (code) {
    case ErrorCode::NotFound:          return QStringLiteral("NotFound");
    case ErrorCode::PermissionDenied:  return QStringLiteral("PermissionDenied");
    case ErrorCode::AlreadyExists:     return QStringLiteral("AlreadyExists");
    case ErrorCode::InvalidName:       return QStringLiteral("InvalidName");
    case ErrorCode::NotADirectory:     return QStringLiteral("NotADirectory");
    case ErrorCode::DirectoryNotEmpty: return QStringLiteral("DirectoryNotEmpty");
    case ErrorCode::NoSpace:           return QStringLiteral("NoSpace");
    case ErrorCode::ReadOnlyVolume:    return QStringLiteral("ReadOnlyVolume");
    case ErrorCode::Busy:              return QStringLiteral("Busy");
    case ErrorCode::Disconnected:      return QStringLiteral("Disconnected");
    case ErrorCode::Timeout:           return QStringLiteral("Timeout");
    case ErrorCode::Cancelled:         return QStringLiteral("Cancelled");
    case ErrorCode::NotSupported:      return QStringLiteral("NotSupported");
    case ErrorCode::CrossDevice:       return QStringLiteral("CrossDevice");
    case ErrorCode::Corrupted:         return QStringLiteral("Corrupted");
    case ErrorCode::IoError:           return QStringLiteral("IoError");
    case ErrorCode::Unknown:           break;
    }
    return QStringLiteral("Unknown");
}

Error errorFromErrno(int errnoValue, const QString &path)
{
    ErrorCode code = ErrorCode::Unknown;
    switch (errnoValue) {
    case ENOENT:       code = ErrorCode::NotFound; break;
    case EACCES:
    case EPERM:        code = ErrorCode::PermissionDenied; break;
    case EEXIST:       code = ErrorCode::AlreadyExists; break;
    case ENOTDIR:      code = ErrorCode::NotADirectory; break;
    case ENOTEMPTY:    code = ErrorCode::DirectoryNotEmpty; break;
    case ENOSPC:       code = ErrorCode::NoSpace; break;
    case EROFS:        code = ErrorCode::ReadOnlyVolume; break;
    case EBUSY:        code = ErrorCode::Busy; break;
    case ENODEV:
    case ENXIO:        code = ErrorCode::Disconnected; break;
    case ETIMEDOUT:    code = ErrorCode::Timeout; break;
    case ECANCELED:    code = ErrorCode::Cancelled; break;
    case ENOSYS:
    case ENOTSUP:      code = ErrorCode::NotSupported; break;
    case EXDEV:        code = ErrorCode::CrossDevice; break;
    case EINVAL:
    case ENAMETOOLONG:
    case EILSEQ:       code = ErrorCode::InvalidName; break;
    case EIO:          code = ErrorCode::IoError; break;
    default:           break;
    }

    Error error;
    error.code = code;
    error.path = path;
    error.systemCode = errnoValue;
    // Para errores sin clasificar se conserva el texto del sistema: es mejor
    // un mensaje técnico que "Error desconocido" a secas.
    error.message = (code == ErrorCode::Unknown)
        ? qt_error_string(errnoValue)   // texto del sistema, seguro entre hilos
        : describe(code);
    return error;
}

#ifdef Q_OS_WIN
Error errorFromWindows(unsigned long lastError, const QString &path)
{
    ErrorCode code = ErrorCode::Unknown;
    switch (lastError) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:      code = ErrorCode::NotFound; break;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:  code = ErrorCode::PermissionDenied; break;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:      code = ErrorCode::AlreadyExists; break;
    case ERROR_DIRECTORY:           code = ErrorCode::NotADirectory; break;
    case ERROR_DIR_NOT_EMPTY:       code = ErrorCode::DirectoryNotEmpty; break;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:    code = ErrorCode::NoSpace; break;
    case ERROR_WRITE_PROTECT:       code = ErrorCode::ReadOnlyVolume; break;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:      code = ErrorCode::Busy; break;
    case ERROR_NOT_READY:
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_BAD_NETPATH:
    case ERROR_NETNAME_DELETED:     code = ErrorCode::Disconnected; break;
    case ERROR_SEM_TIMEOUT:
    case ERROR_TIMEOUT:             code = ErrorCode::Timeout; break;
    case ERROR_OPERATION_ABORTED:
    case ERROR_CANCELLED:           code = ErrorCode::Cancelled; break;
    case ERROR_NOT_SUPPORTED:       code = ErrorCode::NotSupported; break;
    case ERROR_NOT_SAME_DEVICE:     code = ErrorCode::CrossDevice; break;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_FILENAME_EXCED_RANGE: code = ErrorCode::InvalidName; break;
    case ERROR_CRC:
    case ERROR_FILE_CORRUPT:        code = ErrorCode::Corrupted; break;
    default:                        break;
    }

    Error error;
    error.code = code;
    error.path = path;
    error.systemCode = int(lastError);
    if (code == ErrorCode::Unknown) {
        wchar_t *buffer = nullptr;
        const DWORD length = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, lastError, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
        error.message = QString::fromWCharArray(buffer, int(length)).trimmed();
        LocalFree(buffer);
    } else {
        error.message = describe(code);
    }
    return error;
}
#endif

} // namespace eu::core
