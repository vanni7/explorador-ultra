// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
//
// eu-indexsvc: servicio opcional de Windows que lee, sin modificar nada, el
// diario de cambios (USN Journal) y la tabla de archivos (MFT) de los
// volúmenes NTFS, y contesta a la aplicación por una tubería con nombre
// (SPEC §6.4, ADR-0013; el protocolo está en eu/index/UsnProtocol.h).
//
//   eu-indexsvc --instalar     se copia a Archivos de programa, se registra y arranca (administrador)
//   eu-indexsvc --desinstalar  se para y se quita (administrador)
//   eu-indexsvc --consola [--tuberia \\.\pipe\otra]   en primer plano (pruebas)
//   (sin argumentos)           como servicio, lanzado por Windows
//
// Corre como LocalSystem: por eso no usa Qt ni otras bibliotecas (un único
// .exe con el entorno de ejecución dentro), solo lee, contesta únicamente a
// procesos de este equipo y resuelve las carpetas con los permisos de quien
// pregunta, para no revelar nombres que esa persona no podría ver.

#include "eu/index/UsnProtocol.h"

#include <windows.h>
#include <winioctl.h>
#include <sddl.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace usn = eu::index::usn;

namespace {

// Códigos de salida de --instalar y --desinstalar (los traduce la aplicación).
constexpr int exitOk = 0;
constexpr int exitUsage = 1;
constexpr int exitNotAdmin = 2;
constexpr int exitCopyFailed = 3;
constexpr int exitServiceManager = 4;
constexpr int exitStartFailed = 5;
constexpr int exitRemoveFailed = 6;

constexpr wchar_t displayName[] = L"Explorador Ultra: índice de NTFS";
constexpr wchar_t description[] =
    L"Lee el diario de cambios de NTFS (solo lectura) para que Explorador Ultra ponga al día sus índices "
    L"de búsqueda sin recorrer los discos enteros. Opcional: la aplicación funciona igual sin él.";

/// Hasta cuántas conexiones a la vez.
constexpr DWORD maxInstances = 8;

std::wstring g_pipe = usn::pipeName;
bool g_console = false;
HANDLE g_stop = nullptr;   ///< Evento: hay que parar.
std::atomic<int> g_clients{0};
SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};

// ---------------------------------------------------------------- utilidades

std::wstring widen(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring wide(std::size_t(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), length);
    return wide;
}

std::string narrow(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(std::size_t(std::max(length, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), bytes.data(), length, nullptr, nullptr);
    return bytes;
}

/// Un manejador de Windows que se cierra solo.
class Handle
{
public:
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : m_handle(handle) {}
    ~Handle() { reset(); }
    Handle(Handle &&other) noexcept : m_handle(other.m_handle) { other.m_handle = INVALID_HANDLE_VALUE; }
    Handle &operator=(Handle &&other) noexcept
    {
        if (this != &other) {
            reset();
            m_handle = other.m_handle;
            other.m_handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;

    bool valid() const { return m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr; }
    HANDLE get() const { return m_handle; }
    void reset()
    {
        if (valid()) {
            CloseHandle(m_handle);
        }
        m_handle = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE m_handle;
};

/// Avisa de un problema: en la consola o en el Visor de eventos de Windows.
/// (Nada de archivos de registro: un servicio que escribe como SYSTEM en una
/// carpeta donde un usuario puede crear enlaces es un riesgo conocido.)
void report(WORD type, const std::wstring &text)
{
    if (g_console) {
        std::fprintf(stderr, "%s\n", narrow(text).c_str());
        return;
    }
    if (HANDLE source = RegisterEventSourceW(nullptr, usn::serviceName)) {
        const wchar_t *strings[] = {text.c_str()};
        ReportEventW(source, type, 0, 0, nullptr, 1, 0, strings, nullptr);
        DeregisterEventSource(source);
    }
}

std::wstring knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) {
        result = path;
    }
    CoTaskMemFree(path);
    return result;
}

std::string errorLine(const char *code, const std::string &message)
{
    return usn::makeLine({"error", code, message});
}

// ------------------------------------------------------- diario y tabla MFT

/// Identificador de un archivo: 64 bits en NTFS (registros V2), 128 en ReFS (V3).
struct FileKey
{
    bool extended = false;
    FILE_ID_128 id{};

    bool operator<(const FileKey &other) const
    {
        if (extended != other.extended) {
            return extended < other.extended;
        }
        return std::memcmp(id.Identifier, other.id.Identifier, sizeof(id.Identifier)) < 0;
    }
};

/// El volumen "C:" para leer su diario (lo abre quien tiene permiso: SYSTEM).
Handle openVolume(const std::string &drive)
{
    const std::wstring path = L"\\\\.\\" + widen(drive);
    return Handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                              nullptr));
}

struct JournalData
{
    DWORDLONG id = 0;
    USN first = 0;
    USN next = 0;
};

DWORD queryJournal(HANDLE volume, JournalData &journal)
{
    USN_JOURNAL_DATA_V2 data{};
    DWORD bytes = 0;
    if (!DeviceIoControl(volume, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &data, sizeof(data), &bytes, nullptr)) {
        return GetLastError();
    }
    journal.id = data.UsnJournalID;
    journal.first = data.FirstUsn;
    journal.next = data.NextUsn;
    return ERROR_SUCCESS;
}

std::string journalError(DWORD error)
{
    if (error == ERROR_JOURNAL_NOT_ACTIVE || error == ERROR_JOURNAL_DELETE_IN_PROGRESS) {
        return errorLine(usn::errorNoJournal, "la unidad no tiene diario de cambios activo");
    }
    return errorLine(usn::errorSystem, "no se pudo consultar el diario (error " + std::to_string(error) + ")");
}

/// Recorre los registros de un búfer de FSCTL_READ_USN_JOURNAL o de
/// FSCTL_ENUM_USN_DATA (sin los 8 bytes del principio).
template <typename Visit>
void forEachRecord(const BYTE *begin, const BYTE *end, Visit &&visit)
{
    const BYTE *record = begin;
    while (record + sizeof(USN_RECORD_COMMON_HEADER) <= end) {
        const auto *header = reinterpret_cast<const USN_RECORD_COMMON_HEADER *>(record);
        if (header->RecordLength == 0 || record + header->RecordLength > end) {
            break;
        }
        if (header->MajorVersion == 2 || header->MajorVersion == 3) {
            visit(header->MajorVersion, record);
        }
        record += header->RecordLength;
    }
}

/// Cambios que alteran lo que anota el índice: nombres, tamaños, fechas y tipo.
constexpr DWORD relevantReasons = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE | USN_REASON_RENAME_OLD_NAME
    | USN_REASON_RENAME_NEW_NAME | USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND | USN_REASON_DATA_TRUNCATION
    | USN_REASON_BASIC_INFO_CHANGE | USN_REASON_HARD_LINK_CHANGE;

std::string handlePosition(const std::vector<std::string> &fields)
{
    if (fields.size() != 2 || !usn::isDrive(fields[1])) {
        return errorLine(usn::errorRequest, "petición mal formada");
    }
    const Handle volume = openVolume(fields[1]);
    if (!volume.valid()) {
        return errorLine(usn::errorVolume, "no se pudo abrir la unidad " + fields[1]);
    }
    JournalData journal;
    if (const DWORD error = queryJournal(volume.get(), journal)) {
        return journalError(error);
    }
    return usn::makeLine({"ok", std::to_string(journal.id), std::to_string(journal.next)});
}

std::string handleChanges(const std::vector<std::string> &fields, HANDLE pipe)
{
    if (fields.size() != 5 || !usn::isDrive(fields[1])) {
        return errorLine(usn::errorRequest, "petición mal formada");
    }
    const auto journalId = usn::toUnsigned(fields[2]);
    const auto since = usn::toSigned(fields[3]);
    const auto maximum = usn::toUnsigned(fields[4]);
    if (!journalId || !since || !maximum || *maximum == 0 || *maximum > usn::maxFoldersLimit) {
        return errorLine(usn::errorRequest, "petición mal formada");
    }
    const Handle volume = openVolume(fields[1]);
    if (!volume.valid()) {
        return errorLine(usn::errorVolume, "no se pudo abrir la unidad " + fields[1]);
    }
    JournalData journal;
    if (const DWORD error = queryJournal(volume.get(), journal)) {
        return journalError(error);
    }
    if (journal.id != *journalId) {
        return errorLine(usn::errorJournalChanged, "el diario de la unidad se volvió a crear");
    }
    if (*since < journal.first || *since > journal.next) {
        return errorLine(usn::errorPositionLost, "lo pedido ya no está en el diario");
    }

    // Las carpetas de lo que cambió, hasta la posición de ahora.
    const USN stopAt = journal.next;
    std::set<FileKey> parents;
    READ_USN_JOURNAL_DATA_V1 request{};
    request.StartUsn = *since;
    request.ReasonMask = 0xFFFFFFFF;
    request.ReturnOnlyOnClose = FALSE;
    request.Timeout = 0;
    request.BytesToWaitFor = 0;
    request.UsnJournalID = journal.id;
    request.MinMajorVersion = 2;
    request.MaxMajorVersion = 3;
    std::vector<DWORDLONG> buffer(64 * 1024 / sizeof(DWORDLONG));   // alineado a 8 bytes, como los registros
    bool tooMany = false;
    while (request.StartUsn < stopAt && !tooMany) {
        DWORD bytes = 0;
        if (!DeviceIoControl(volume.get(), FSCTL_READ_USN_JOURNAL, &request, sizeof(request), buffer.data(),
                             DWORD(buffer.size() * sizeof(DWORDLONG)), &bytes, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_JOURNAL_ENTRY_DELETED) {
                return errorLine(usn::errorPositionLost, "lo pedido ya no está en el diario");
            }
            if (error == ERROR_JOURNAL_NOT_ACTIVE || error == ERROR_JOURNAL_DELETE_IN_PROGRESS) {
                return errorLine(usn::errorJournalChanged, "el diario de la unidad se volvió a crear");
            }
            return errorLine(usn::errorSystem, "no se pudo leer el diario (error " + std::to_string(error) + ")");
        }
        if (bytes <= sizeof(USN)) {
            break;
        }
        const auto *data = reinterpret_cast<const BYTE *>(buffer.data());
        USN next = 0;
        std::memcpy(&next, data, sizeof(next));
        bool reachedEnd = false;
        forEachRecord(data + sizeof(USN), data + bytes, [&](WORD version, const BYTE *record) {
            FileKey parent;
            USN recordUsn = 0;
            DWORD reason = 0;
            if (version == 2) {
                const auto *v2 = reinterpret_cast<const USN_RECORD_V2 *>(record);
                recordUsn = v2->Usn;
                reason = v2->Reason;
                std::memcpy(parent.id.Identifier, &v2->ParentFileReferenceNumber, sizeof(v2->ParentFileReferenceNumber));
            } else {
                const auto *v3 = reinterpret_cast<const USN_RECORD_V3 *>(record);
                recordUsn = v3->Usn;
                reason = v3->Reason;
                parent.extended = true;
                parent.id = v3->ParentFileReferenceNumber;
            }
            if (recordUsn >= stopAt) {
                reachedEnd = true;
                return;
            }
            if ((reason & relevantReasons) != 0) {
                parents.insert(parent);
                tooMany = tooMany || parents.size() > *maximum;
            }
        });
        if (reachedEnd || next <= request.StartUsn) {
            break;
        }
        request.StartUsn = next;
    }
    if (tooMany) {
        return errorLine(usn::errorTooMany, "cambiaron más de " + fields[4] + " carpetas");
    }

    // De identificador a ruta, con los permisos de quien pregunta: solo las
    // carpetas que esa persona puede listar.
    const std::wstring rootPath = widen(fields[1]) + L"\\";
    const Handle hint(CreateFileW(rootPath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!hint.valid()) {
        return errorLine(usn::errorVolume, "no se pudo abrir la unidad " + fields[1]);
    }
    if (!ImpersonateNamedPipeClient(pipe)) {
        return errorLine(usn::errorImpersonation, "no se pudo actuar con los permisos de quien pregunta");
    }
    std::vector<std::string> folders;
    std::vector<wchar_t> path(1024);
    for (const FileKey &key : parents) {
        FILE_ID_DESCRIPTOR descriptor{};
        descriptor.dwSize = sizeof(descriptor);
        if (key.extended) {
            descriptor.Type = ExtendedFileIdType;
            descriptor.ExtendedFileId = key.id;
        } else {
            descriptor.Type = FileIdType;
            std::memcpy(&descriptor.FileId, key.id.Identifier, sizeof(descriptor.FileId));
        }
        const Handle folder(OpenFileById(hint.get(), &descriptor, FILE_LIST_DIRECTORY | SYNCHRONIZE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                         FILE_FLAG_BACKUP_SEMANTICS));
        if (!folder.valid()) {
            continue;   // ya no existe, o quien pregunta no puede verla
        }
        DWORD length = GetFinalPathNameByHandleW(folder.get(), path.data(), DWORD(path.size()),
                                                 FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length >= path.size()) {
            path.resize(std::size_t(length) + 1);
            length = GetFinalPathNameByHandleW(folder.get(), path.data(), DWORD(path.size()),
                                               FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        }
        if (length == 0 || length >= path.size()) {
            continue;
        }
        std::wstring_view text(path.data(), length);
        if (text.rfind(L"\\\\?\\", 0) == 0) {
            text.remove_prefix(4);
        }
        if (text.rfind(L"UNC\\", 0) == 0) {
            continue;
        }
        folders.push_back(narrow(text));
    }
    if (!RevertToSelf()) {
        // Nunca seguir con la identidad de otro.
        report(EVENTLOG_ERROR_TYPE, L"No se pudo dejar de actuar como el cliente; el servicio se cierra.");
        ExitProcess(1);
    }

    std::string answer = usn::makeLine(
        {"ok", std::to_string(journal.id), std::to_string(stopAt), std::to_string(folders.size())});
    for (const std::string &folder : folders) {
        answer += usn::makeLine({folder});
    }
    return answer;
}

/// Cuenta archivos y carpetas en la MFT. No devuelve nombres: solo números.
std::string handleCount(const std::vector<std::string> &fields)
{
    if (fields.size() != 2 || !usn::isDrive(fields[1])) {
        return errorLine(usn::errorRequest, "petición mal formada");
    }
    const Handle volume = openVolume(fields[1]);
    if (!volume.valid()) {
        return errorLine(usn::errorVolume, "no se pudo abrir la unidad " + fields[1]);
    }
    MFT_ENUM_DATA_V1 request{};
    request.StartFileReferenceNumber = 0;
    request.LowUsn = 0;
    request.HighUsn = MAXLONGLONG;
    request.MinMajorVersion = 2;
    request.MaxMajorVersion = 3;
    std::uint64_t files = 0;
    std::uint64_t folders = 0;
    std::vector<DWORDLONG> buffer(256 * 1024 / sizeof(DWORDLONG));
    while (WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0) {
        DWORD bytes = 0;
        if (!DeviceIoControl(volume.get(), FSCTL_ENUM_USN_DATA, &request, sizeof(request), buffer.data(),
                             DWORD(buffer.size() * sizeof(DWORDLONG)), &bytes, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_HANDLE_EOF) {
                break;   // fin de la tabla
            }
            return journalError(error);
        }
        if (bytes <= sizeof(DWORDLONG)) {
            break;
        }
        const auto *data = reinterpret_cast<const BYTE *>(buffer.data());
        DWORDLONG next = 0;
        std::memcpy(&next, data, sizeof(next));
        forEachRecord(data + sizeof(DWORDLONG), data + bytes, [&](WORD version, const BYTE *record) {
            const DWORD attributes = version == 2 ? reinterpret_cast<const USN_RECORD_V2 *>(record)->FileAttributes
                                                  : reinterpret_cast<const USN_RECORD_V3 *>(record)->FileAttributes;
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                ++folders;
            } else {
                ++files;
            }
        });
        if (next == request.StartFileReferenceNumber) {
            break;
        }
        request.StartFileReferenceNumber = next;
    }
    return usn::makeLine({"ok", std::to_string(files), std::to_string(folders)});
}

std::string answerTo(const std::string &request, HANDLE pipe)
{
    try {
        const std::vector<std::string> fields = usn::splitFields(request);
        const std::string &operation = fields.front();
        if (operation == "hola" && fields.size() == 1) {
            return usn::makeLine({"ok", std::to_string(usn::protocolVersion)});
        }
        if (operation == "posicion") {
            return handlePosition(fields);
        }
        if (operation == "cambios") {
            return handleChanges(fields, pipe);
        }
        if (operation == "recuento") {
            return handleCount(fields);
        }
        return errorLine(usn::errorRequest, "petición desconocida");
    } catch (...) {
        return errorLine(usn::errorSystem, "fallo interno del servicio");
    }
}

// --------------------------------------------------------------- la tubería

/// Espera una lectura o escritura; false si falla, se agota el tiempo o hay que parar.
bool finish(HANDLE pipe, OVERLAPPED &overlapped, DWORD timeoutMs, DWORD *bytes)
{
    HANDLE waits[2] = {overlapped.hEvent, g_stop};
    if (WaitForMultipleObjects(2, waits, FALSE, timeoutMs) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped);
        GetOverlappedResult(pipe, &overlapped, bytes, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &overlapped, bytes, FALSE) != 0;
}

bool readRequest(HANDLE pipe, HANDLE event, std::string &request)
{
    char buffer[512];
    while (request.find('\n') == std::string::npos) {
        if (request.size() > usn::maxRequestBytes) {
            return false;
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        ResetEvent(event);
        DWORD bytes = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), nullptr, &overlapped) && GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        if (!finish(pipe, overlapped, 10000, &bytes) || bytes == 0) {
            return false;
        }
        request.append(buffer, bytes);
    }
    request.resize(request.find('\n') + 1);
    return true;
}

bool writeAll(HANDLE pipe, HANDLE event, const std::string &answer)
{
    std::size_t done = 0;
    while (done < answer.size()) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        ResetEvent(event);
        const DWORD chunk = DWORD(std::min<std::size_t>(answer.size() - done, 64 * 1024));
        DWORD bytes = 0;
        if (!WriteFile(pipe, answer.data() + done, chunk, nullptr, &overlapped) && GetLastError() != ERROR_IO_PENDING) {
            return false;
        }
        if (!finish(pipe, overlapped, 60000, &bytes) || bytes == 0) {
            return false;
        }
        done += bytes;
    }
    return true;
}

void serveClient(HANDLE pipe)
{
    const Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    std::string request;
    if (event.valid() && readRequest(pipe, event.get(), request)) {
        writeAll(pipe, event.get(), answerTo(request, pipe));
    }
    // Sin DisconnectNamedPipe: descartaría lo que el cliente aún no ha leído.
    // Al cerrar, el cliente lee lo que queda y luego ve la tubería rota.
    CloseHandle(pipe);
}

/// Atiende la tubería hasta que haya que parar. `onReady` se llama cuando
/// ya se puede conectar.
int runServer(void (*onReady)())
{
    // SYSTEM y administradores, todo; los demás usuarios del equipo, leer y
    // escribir datos (0x12008b = FILE_GENERIC_READ | FILE_WRITE_DATA), pero
    // no crear otras instancias de la tubería para hacerse pasar por el servicio.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x12008b;;;AU)",
                                                              SDDL_REVISION_1, &descriptor, nullptr)) {
        report(EVENTLOG_ERROR_TYPE, L"No se pudo preparar la seguridad de la tubería.");
        return 1;
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    const Handle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    bool first = true;
    int result = 0;
    while (WaitForSingleObject(g_stop, 0) != WAIT_OBJECT_0) {
        // La primera instancia la crea quien llega antes: si otro programa ya
        // tiene la tubería, no se le atiende y se avisa.
        const DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        HANDLE pipe = CreateNamedPipeW(g_pipe.c_str(), openMode,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       maxInstances, 64 * 1024, 64 * 1024, 0, &attributes);
        if (pipe == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (first) {
                report(EVENTLOG_ERROR_TYPE,
                       L"No se pudo crear la tubería " + g_pipe + L" (¿ya está en uso?), error " + std::to_wstring(error));
                result = 1;
                break;
            }
            WaitForSingleObject(g_stop, error == ERROR_PIPE_BUSY ? 200 : 1000);   // todas ocupadas: se espera
            continue;
        }
        if (first) {
            first = false;
            if (onReady != nullptr) {
                onReady();
            }
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = connected.get();
        ResetEvent(connected.get());
        bool ready = false;
        if (!ConnectNamedPipe(pipe, &overlapped)) {
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                ready = true;
            } else if (error == ERROR_IO_PENDING) {
                DWORD ignored = 0;
                ready = finish(pipe, overlapped, INFINITE, &ignored);
            }
        } else {
            ready = true;
        }
        if (!ready) {
            CloseHandle(pipe);
            continue;
        }
        ++g_clients;
        std::thread([pipe] {
            serveClient(pipe);
            --g_clients;
        }).detach();
    }
    // Se da un momento a lo que esté en curso.
    for (int i = 0; i < 100 && g_clients.load() > 0; ++i) {
        Sleep(100);
    }
    LocalFree(descriptor);
    return result;
}

// ---------------------------------------------------------------- servicio

void setState(DWORD state, DWORD exitCode = NO_ERROR)
{
    static DWORD checkPoint = 1;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwServiceSpecificExitCode = 0;
    const bool settled = state == SERVICE_RUNNING || state == SERVICE_STOPPED;
    g_status.dwCheckPoint = settled ? 0 : checkPoint++;
    g_status.dwWaitHint = settled ? 0 : 15000;
    SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI controlHandler(DWORD control, DWORD, LPVOID, LPVOID)
{
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        setState(SERVICE_STOP_PENDING);
        SetEvent(g_stop);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI serviceMain(DWORD, LPWSTR *)
{
    g_statusHandle = RegisterServiceCtrlHandlerExW(usn::serviceName, controlHandler, nullptr);
    if (g_statusHandle == nullptr) {
        return;
    }
    setState(SERVICE_START_PENDING);
    const int result = runServer([] { setState(SERVICE_RUNNING); });
    setState(SERVICE_STOPPED, result == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
}

BOOL WINAPI consoleHandler(DWORD)
{
    SetEvent(g_stop);
    return TRUE;
}

// --------------------------------------------------- instalar y desinstalar

bool isAdministrator()
{
    BOOL member = FALSE;
    PSID administrators = nullptr;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0,
                                 0, &administrators)) {
        if (!CheckTokenMembership(nullptr, administrators, &member)) {
            member = FALSE;
        }
        FreeSid(administrators);
    }
    return member != FALSE;
}

/// Dónde se instala: una carpeta propia en Archivos de programa, que solo
/// los administradores pueden cambiar (lo que corre como SYSTEM no puede
/// estar donde un usuario pueda reemplazarlo).
std::wstring installFolder()
{
    return knownFolder(FOLDERID_ProgramFiles) + L"\\Explorador Ultra\\servicio";
}

bool waitForState(SC_HANDLE service, DWORD wanted, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (true) {
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status),
                                  &needed)) {
            return false;
        }
        if (status.dwCurrentState == wanted) {
            return true;
        }
        if (GetTickCount64() >= deadline || (wanted == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED)) {
            return false;
        }
        Sleep(200);
    }
}

/// Lo para y espera a que su proceso haya salido del todo (hasta entonces
/// el .exe sigue en uso y la tubería, abierta).
void stopService(SC_HANDLE service)
{
    SERVICE_STATUS_PROCESS before{};
    DWORD needed = 0;
    QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&before), sizeof(before), &needed);
    const Handle process(before.dwProcessId != 0 ? OpenProcess(SYNCHRONIZE, FALSE, before.dwProcessId) : nullptr);
    SERVICE_STATUS status{};
    if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        waitForState(service, SERVICE_STOPPED, 60000);
    }
    if (process.valid()) {
        WaitForSingleObject(process.get(), 30000);
    }
}

/// Avisa en la consola de qué paso falló (al lanzarlo la aplicación no se
/// ve, pero sí en las pruebas y a mano) y devuelve el código de salida.
int failure(int code, const char *step)
{
    std::fprintf(stderr, "eu-indexsvc: %s (error %lu)\n", step, GetLastError());
    return code;
}

/// Copia el .exe; reintenta un rato si está en uso (el proceso anterior aún
/// se está cerrando, o el antivirus lo está revisando).
bool copyProgram(const wchar_t *from, const std::wstring &to)
{
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (CopyFileW(from, to.c_str(), FALSE)) {
            return true;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED && error != ERROR_LOCK_VIOLATION) {
            return false;
        }
        Sleep(1000);
    }
    return false;
}

int install()
{
    if (!isAdministrator()) {
        return exitNotAdmin;
    }
    std::vector<wchar_t> self(32768);
    const DWORD selfLength = GetModuleFileNameW(nullptr, self.data(), DWORD(self.size()));
    if (selfLength == 0 || selfLength >= self.size()) {
        return failure(exitCopyFailed, "no se sabe dónde está este programa");
    }
    const std::wstring folder = installFolder();
    const std::wstring target = folder + L"\\eu-indexsvc.exe";

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr) {
        return failure(exitServiceManager, "no se pudo abrir el administrador de servicios");
    }
    SC_HANDLE service = OpenServiceW(manager, usn::serviceName, SERVICE_ALL_ACCESS);
    if (service != nullptr) {
        stopService(service);   // se actualiza: un .exe en marcha no se puede reemplazar
    }
    const int created = SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS && created != ERROR_FILE_EXISTS) {
        if (service != nullptr) {
            CloseServiceHandle(service);
        }
        CloseServiceHandle(manager);
        return failure(exitCopyFailed, "no se pudo crear la carpeta en Archivos de programa");
    }
    if (_wcsicmp(self.data(), target.c_str()) != 0 && !copyProgram(self.data(), target)) {
        const int code = failure(exitCopyFailed, "no se pudo copiar a Archivos de programa");
        if (service != nullptr) {
            CloseServiceHandle(service);
        }
        CloseServiceHandle(manager);
        return code;
    }
    const std::wstring command = L"\"" + target + L"\"";
    if (service != nullptr) {
        ChangeServiceConfigW(service, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                             command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, displayName);
    } else {
        // Sin cuenta: LocalSystem, la única que puede abrir el volumen para leer el diario.
        service = CreateServiceW(manager, usn::serviceName, displayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                 SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
                                 nullptr, nullptr);
    }
    if (service == nullptr) {
        const int code = failure(exitServiceManager, "no se pudo registrar el servicio");
        CloseServiceHandle(manager);
        return code;
    }
    std::wstring text = description;
    SERVICE_DESCRIPTIONW serviceDescription{text.data()};
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &serviceDescription);
    SERVICE_DELAYED_AUTO_START_INFO delayed{TRUE};   // arranca cuando Windows ya está libre
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
    // Solo los privilegios que usa: recorrer carpetas, actuar como el cliente y consultar el volumen.
    wchar_t privileges[] = L"SeChangeNotifyPrivilege\0SeImpersonatePrivilege\0SeManageVolumePrivilege\0";
    SERVICE_REQUIRED_PRIVILEGES_INFOW required{privileges};
    ChangeServiceConfig2W(service, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &required);
    // Si se cae, se vuelve a arrancar al minuto (dos veces al día como mucho).
    SC_ACTION actions[2] = {{SC_ACTION_RESTART, 60000}, {SC_ACTION_RESTART, 60000}};
    SERVICE_FAILURE_ACTIONSW failure{};
    failure.dwResetPeriod = 24 * 60 * 60;
    failure.cActions = 2;
    failure.lpsaActions = actions;
    ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

    int result = exitOk;
    if (!StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        result = failure(exitStartFailed, "no se pudo arrancar");
    } else if (!waitForState(service, SERVICE_RUNNING, 60000)) {
        result = failure(exitStartFailed, "arrancó pero no llegó a estar en marcha (ver el Visor de eventos)");
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return result;
}

int uninstall()
{
    if (!isAdministrator()) {
        return exitNotAdmin;
    }
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return exitServiceManager;
    }
    int result = exitOk;
    if (SC_HANDLE service = OpenServiceW(manager, usn::serviceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE)) {
        stopService(service);
        if (!DeleteService(service) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) {
            result = exitRemoveFailed;
        }
        CloseServiceHandle(service);
    } else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
        result = exitRemoveFailed;
    }
    CloseServiceHandle(manager);

    // Su copia en Archivos de programa, y nada más (nunca la de la aplicación).
    const std::wstring folder = installFolder();
    const std::wstring target = folder + L"\\eu-indexsvc.exe";
    if (!DeleteFileW(target.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        MoveFileExW(target.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);   // en uso: al reiniciar
    }
    RemoveDirectoryW(folder.c_str());   // solo si quedó vacía
    RemoveDirectoryW(folder.substr(0, folder.find_last_of(L'\\')).c_str());
    return result;
}

void printUsage()
{
    std::fprintf(stderr, "Uso: eu-indexsvc --instalar | --desinstalar | --consola [--tuberia \\\\.\\pipe\\nombre]\n");
}

} // namespace

int wmain(int argc, wchar_t *argv[])
{
    const std::vector<std::wstring> arguments(argv + 1, argv + argc);
    if (arguments.size() == 1 && arguments[0] == L"--instalar") {
        return install();
    }
    if (arguments.size() == 1 && arguments[0] == L"--desinstalar") {
        return uninstall();
    }
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop == nullptr) {
        return exitUsage;
    }
    if (!arguments.empty() && arguments[0] == L"--consola") {
        g_console = true;
        if (arguments.size() == 3 && arguments[1] == L"--tuberia" && arguments[2].rfind(L"\\\\.\\pipe\\", 0) == 0) {
            g_pipe = arguments[2];
        } else if (arguments.size() != 1) {
            printUsage();
            return exitUsage;
        }
        SetConsoleCtrlHandler(consoleHandler, TRUE);
        return runServer([] {
            std::printf("listo\n");   // las pruebas esperan esta línea para conectar
            std::fflush(stdout);
        });
    }
    if (!arguments.empty()) {
        printUsage();
        return exitUsage;
    }
    wchar_t name[] = L"ExploradorUltraIndice";
    SERVICE_TABLE_ENTRYW table[] = {{name, serviceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        printUsage();   // lanzado a mano, no por Windows
        return exitUsage;
    }
    return exitOk;
}
