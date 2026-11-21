// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
//
// Cliente del servicio eu-indexsvc (ver UsnProtocol.h) y control del servicio
// para la ventana del índice.

#include "eu/index/ChangeJournal.h"
#include "eu/index/UsnProtocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>

#include <algorithm>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

namespace eu::index {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::index", text);
}

core::Error makeError(core::ErrorCode code, const QString &message, DWORD systemCode = 0)
{
    return core::Error{code, message, QString(), int(systemCode)};
}

/// Un manejador de Windows que se cierra solo.
class Handle
{
public:
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : m_handle(handle) {}
    ~Handle()
    {
        if (valid()) {
            CloseHandle(m_handle);
        }
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;

    bool valid() const { return m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr; }
    HANDLE get() const { return m_handle; }

private:
    HANDLE m_handle;
};

/// La tubería a usar; `overridden` si es otra que la del servicio (pruebas).
std::wstring pipeToUse(bool *overridden)
{
    const QByteArray other = qgetenv(usn::pipeVariable);
    *overridden = !other.isEmpty();
    return *overridden ? QString::fromLocal8Bit(other).toStdWString() : std::wstring(usn::pipeName);
}

/// Estado del servicio instalado y su proceso. false si no está instalado.
bool queryService(DWORD *state, DWORD *processId)
{
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return false;
    }
    bool found = false;
    if (SC_HANDLE service = OpenServiceW(manager, usn::serviceName, SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status),
                                 &needed)) {
            found = true;
            *state = status.dwCurrentState;
            *processId = status.dwProcessId;
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return found;
}

enum class Wait { Done, Failed, TimedOut };

/// Espera una lectura o escritura con OVERLAPPED. Si se agota el tiempo, la
/// cancela (y espera a que la cancelación termine: el búfer es de quien llama).
Wait waitFor(HANDLE pipe, OVERLAPPED &overlapped, DWORD timeoutMs, DWORD *bytes, DWORD *error)
{
    const DWORD waited = WaitForSingleObject(overlapped.hEvent, timeoutMs);
    if (waited != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped);
        GetOverlappedResult(pipe, &overlapped, bytes, TRUE);
        return Wait::TimedOut;
    }
    if (!GetOverlappedResult(pipe, &overlapped, bytes, FALSE)) {
        *error = GetLastError();
        return Wait::Failed;
    }
    return Wait::Done;
}

/// Envía una petición al servicio y devuelve las líneas de la respuesta (la
/// primera, con "ok"; si contestó "error", lo convierte en el Error).
core::Result<std::vector<std::string>> ask(const std::string &request, DWORD timeoutMs)
{
    bool overridden = false;
    const std::wstring name = pipeToUse(&overridden);
    HANDLE raw = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; ++attempt) {
        // Solo leer y escribir datos: con GENERIC_WRITE se pediría también
        // crear instancias de la tubería, que el servicio no concede.
        // SECURITY_IMPERSONATION: el servicio busca las carpetas con los
        // permisos de quien pregunta, no con los suyos.
        raw = CreateFileW(name.c_str(), GENERIC_READ | FILE_WRITE_DATA, 0, nullptr, OPEN_EXISTING,
                          FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION, nullptr);
        if (raw != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) {
            break;
        }
        WaitNamedPipeW(name.c_str(), 2000);   // todas las instancias ocupadas: se espera turno
    }
    if (raw == INVALID_HANDLE_VALUE) {
        return makeError(core::ErrorCode::NotFound,
                         tr("El servicio de índice de Windows no está instalado o no responde"), GetLastError());
    }
    Handle pipe(raw);
    if (!overridden) {
        // Que al otro lado esté el servicio instalado y no otro programa que
        // haya creado una tubería con el mismo nombre.
        ULONG server = 0;
        DWORD state = 0;
        DWORD servicePid = 0;
        if (!GetNamedPipeServerProcessId(pipe.get(), &server) || !queryService(&state, &servicePid)
            || state != SERVICE_RUNNING || server == 0 || server != servicePid) {
            return makeError(core::ErrorCode::PermissionDenied,
                             tr("La tubería del índice no es del servicio instalado: no se usa"));
        }
    }

    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        return makeError(core::ErrorCode::IoError, tr("No se pudo hablar con el servicio de índice"), GetLastError());
    }
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    const auto remaining = [deadline] {
        const ULONGLONG now = GetTickCount64();
        return now >= deadline ? DWORD(0) : DWORD(std::min<ULONGLONG>(deadline - now, 0x7fffffff));
    };

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD bytes = 0;
    DWORD error = 0;
    if (!WriteFile(pipe.get(), request.data(), DWORD(request.size()), nullptr, &overlapped)
        && GetLastError() != ERROR_IO_PENDING) {
        return makeError(core::ErrorCode::IoError, tr("No se pudo hablar con el servicio de índice"), GetLastError());
    }
    if (waitFor(pipe.get(), overlapped, remaining(), &bytes, &error) != Wait::Done || bytes != request.size()) {
        return makeError(core::ErrorCode::IoError, tr("No se pudo hablar con el servicio de índice"), error);
    }

    // La respuesta llega entera y después el servicio cierra la conexión.
    std::string answer;
    std::vector<char> buffer(64 * 1024);
    while (true) {
        ResetEvent(event.get());
        overlapped = OVERLAPPED{};
        overlapped.hEvent = event.get();
        if (!ReadFile(pipe.get(), buffer.data(), DWORD(buffer.size()), nullptr, &overlapped)) {
            error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
                break;
            }
            if (error != ERROR_IO_PENDING) {
                return makeError(core::ErrorCode::IoError, tr("Se cortó la conexión con el servicio de índice"),
                                 error);
            }
        }
        const Wait result = waitFor(pipe.get(), overlapped, remaining(), &bytes, &error);
        if (result == Wait::TimedOut) {
            return makeError(core::ErrorCode::Timeout, tr("El servicio de índice tardó demasiado en contestar"));
        }
        if (result == Wait::Failed) {
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
                break;
            }
            return makeError(core::ErrorCode::IoError, tr("Se cortó la conexión con el servicio de índice"), error);
        }
        answer.append(buffer.data(), bytes);
        if (answer.size() > 512u * 1024 * 1024) {
            return makeError(core::ErrorCode::IoError, tr("Respuesta del servicio de índice demasiado grande"));
        }
    }

    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < answer.size()) {
        const std::size_t end = answer.find('\n', start);
        if (end == std::string::npos) {
            break;   // una línea sin terminar: la respuesta se cortó
        }
        lines.push_back(answer.substr(start, end - start));
        start = end + 1;
    }
    if (lines.empty()) {
        return makeError(core::ErrorCode::IoError, tr("El servicio de índice no contestó"));
    }
    const std::vector<std::string> first = usn::splitFields(lines.front());
    if (first.front() == "ok") {
        return lines;
    }
    const std::string code = first.size() > 1 ? first[1] : std::string();
    const QString detail = first.size() > 2 ? QString::fromUtf8(first[2]) : QString();
    core::ErrorCode kind = core::ErrorCode::IoError;
    if (code == usn::errorNoJournal) {
        kind = core::ErrorCode::NotSupported;
    } else if (code == usn::errorJournalChanged || code == usn::errorPositionLost) {
        kind = core::ErrorCode::Corrupted;
    } else if (code == usn::errorTooMany) {
        kind = core::ErrorCode::Busy;
    } else if (code == usn::errorVolume) {
        kind = core::ErrorCode::NotFound;
    } else if (code == usn::errorImpersonation) {
        kind = core::ErrorCode::PermissionDenied;
    }
    return makeError(kind, tr("Servicio de índice: %1 (%2)").arg(detail, QString::fromStdString(code)));
}

core::Error badAnswer()
{
    return makeError(core::ErrorCode::IoError, tr("Respuesta del servicio de índice no válida"));
}

/// Habla con eu-indexsvc.
class ServiceJournal : public ChangeJournal
{
public:
    QString volumeOf(const QString &path) const override
    {
        const QStorageInfo storage(path);
        if (!storage.isValid() || !storage.isReady()) {
            return {};
        }
        const QString root = QDir::fromNativeSeparators(storage.rootPath());   // "C:/"
        if (root.size() != 3 || !root.at(0).isLetter() || root.at(1) != QLatin1Char(':')) {
            return {};   // una carpeta montada como unidad, o una ruta de red
        }
        const QByteArray type = storage.fileSystemType();
        if (type != "NTFS" && type != "ReFS") {
            return {};
        }
        // Una unidad de red asignada a una letra puede decir "NTFS": la del otro equipo.
        const std::wstring native = QDir::toNativeSeparators(root).toStdWString();
        const UINT kind = GetDriveTypeW(native.c_str());
        if (kind != DRIVE_FIXED && kind != DRIVE_REMOVABLE) {
            return {};
        }
        return root.left(2).toUpper();
    }

    core::Result<JournalPosition> position(const QString &volume) const override
    {
        const auto answer = ask(usn::makeLine({"posicion", volume.toStdString()}), 10000);
        if (!answer.ok()) {
            return answer.error();
        }
        const std::vector<std::string> fields = usn::splitFields(answer.value().front());
        const auto journal = fields.size() == 3 ? usn::toUnsigned(fields[1]) : std::nullopt;
        const auto usnValue = fields.size() == 3 ? usn::toSigned(fields[2]) : std::nullopt;
        if (!journal || !usnValue) {
            return badAnswer();
        }
        return JournalPosition{*journal, *usnValue};
    }

    core::Result<JournalChanges> changesSince(const QString &volume, const JournalPosition &since,
                                              int maxFolders) const override
    {
        const auto answer = ask(usn::makeLine({"cambios", volume.toStdString(), std::to_string(since.journal),
                                               std::to_string(since.usn), std::to_string(maxFolders)}),
                                5 * 60 * 1000);
        if (!answer.ok()) {
            return answer.error();
        }
        const std::vector<std::string> &lines = answer.value();
        const std::vector<std::string> fields = usn::splitFields(lines.front());
        const auto journal = fields.size() == 4 ? usn::toUnsigned(fields[1]) : std::nullopt;
        const auto usnValue = fields.size() == 4 ? usn::toSigned(fields[2]) : std::nullopt;
        const auto count = fields.size() == 4 ? usn::toUnsigned(fields[3]) : std::nullopt;
        if (!journal || !usnValue || !count || *count != lines.size() - 1) {
            return badAnswer();
        }
        JournalChanges changes;
        changes.next = JournalPosition{*journal, *usnValue};
        changes.folders.reserve(qsizetype(*count));
        for (std::size_t i = 1; i < lines.size(); ++i) {
            changes.folders.append(QDir::cleanPath(QDir::fromNativeSeparators(QString::fromUtf8(lines[i]))));
        }
        return changes;
    }

    core::Result<VolumeCounts> counts(const QString &volume) const override
    {
        const auto answer = ask(usn::makeLine({"recuento", volume.toStdString()}), 15 * 60 * 1000);
        if (!answer.ok()) {
            return answer.error();
        }
        const std::vector<std::string> fields = usn::splitFields(answer.value().front());
        const auto files = fields.size() == 3 ? usn::toUnsigned(fields[1]) : std::nullopt;
        const auto folders = fields.size() == 3 ? usn::toUnsigned(fields[2]) : std::nullopt;
        if (!files || !folders) {
            return badAnswer();
        }
        return VolumeCounts{qint64(*files), qint64(*folders)};
    }
};

QString serviceProgram()
{
    const QString program = QCoreApplication::applicationDirPath() + QStringLiteral("/eu-indexsvc.exe");
    return QFileInfo::exists(program) ? program : QString();
}

} // namespace

std::shared_ptr<ChangeJournal> systemChangeJournal()
{
    static const std::shared_ptr<ChangeJournal> journal = std::make_shared<ServiceJournal>();
    return journal;
}

namespace journalservice {

State state()
{
    DWORD current = 0;
    DWORD processId = 0;
    if (queryService(&current, &processId)) {
        return current == SERVICE_RUNNING ? State::Running : State::Stopped;
    }
    return serviceProgram().isEmpty() ? State::Unavailable : State::NotInstalled;
}

core::Result<Setup> Setup::start(bool install)
{
    const QString program = serviceProgram();
    if (program.isEmpty()) {
        return makeError(core::ErrorCode::NotFound, tr("Falta eu-indexsvc.exe junto a la aplicación"));
    }
    const std::wstring file = QDir::toNativeSeparators(program).toStdWString();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";   // con el aviso de Windows para pedir permiso de administrador
    info.lpFile = file.c_str();
    info.lpParameters = install ? L"--instalar" : L"--desinstalar";
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD error = GetLastError();
        if (error == ERROR_CANCELLED) {
            return makeError(core::ErrorCode::Cancelled, tr("No se dio permiso de administrador"), error);
        }
        return makeError(core::ErrorCode::IoError, tr("No se pudo lanzar eu-indexsvc.exe"), error);
    }
    if (info.hProcess == nullptr) {
        return makeError(core::ErrorCode::IoError, tr("No se pudo lanzar eu-indexsvc.exe"));
    }
    Setup setup;
    setup.m_process = std::shared_ptr<void>(info.hProcess, [](void *process) { CloseHandle(process); });
    return setup;
}

bool Setup::isFinished() const
{
    return !m_process || WaitForSingleObject(m_process.get(), 0) == WAIT_OBJECT_0;
}

int Setup::exitCode() const
{
    DWORD code = 0;
    return m_process && GetExitCodeProcess(m_process.get(), &code) ? int(code) : -1;
}

} // namespace journalservice

} // namespace eu::index
