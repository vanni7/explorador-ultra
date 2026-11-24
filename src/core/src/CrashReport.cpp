// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/core/CrashReport.h"

#include "eu/core/Version.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(Q_OS_WIN)
#include <windows.h>
// dbghelp.h necesita windows.h antes.
#include <dbghelp.h>
#else
#include <csignal>
#include <ctime>
#include <dlfcn.h>
#include <sys/mman.h>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(Q_OS_LINUX)
#include <ucontext.h>
#elif defined(Q_OS_MACOS)
#include <sys/ucontext.h>
#endif
#endif

// Con AddressSanitizer o ThreadSanitizer no se instala nada: ellos ya
// capturan los fallos y dan un informe mejor.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define EU_CRASH_REPORT_DISABLED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define EU_CRASH_REPORT_DISABLED 1
#endif
#endif

namespace eu::core {

namespace {

#if !defined(EU_CRASH_REPORT_DISABLED)

/// Primeras líneas de cada informe, preparadas al instalar: dentro del
/// manejador no se puede reservar memoria.
QByteArray g_header;

constexpr const char *invalidPointerNote =
    "La instrucción no está en ningún programa ni biblioteca: se llamó a una función a través de un puntero "
    "no válido (a menudo, un objeto que ya se había destruido).\n";

#if !defined(Q_OS_WIN)

int g_fd = -1;
/// Pila propia para el manejador: si el fallo es por agotar la pila, no
/// quedaría sitio para informar.
alignas(16) char g_alternateStack[64 * 1024];

// Todo lo que sigue se usa dentro del manejador: solo llamadas que no
// reservan memoria ni toman cerrojos (write, backtrace_symbols_fd…).

void put(const char *text)
{
    if (g_fd >= 0) {
        const ssize_t written = ::write(g_fd, text, std::strlen(text));
        (void)written;
    }
}

void putNumber(unsigned long long value, unsigned base, int minDigits = 1)
{
    char reversed[32];
    int length = 0;
    do {
        const unsigned digit = unsigned(value % base);
        reversed[length++] = char(digit < 10 ? '0' + digit : 'a' + (digit - 10));
        value /= base;
    } while ((value != 0 || length < minDigits) && length < 31);
    char text[32];
    for (int i = 0; i < length; ++i) {
        text[i] = reversed[length - 1 - i];
    }
    text[length] = '\0';
    put(text);
}

void putAddress(const void *address)
{
    put("0x");
    putNumber(reinterpret_cast<std::uintptr_t>(address), 16);
}

/// Una dirección con el programa o biblioteca y la función que la contienen.
void putSymbol(void *address)
{
    void *one[1] = {address};
    put("  ");
    backtrace_symbols_fd(one, 1, g_fd);
}

bool isInLoadedCode(void *address)
{
    Dl_info info;
    return dladdr(address, &info) != 0 && info.dli_fname != nullptr;
}

const char *signalName(int number)
{
    switch (number) {
    case SIGSEGV: return "SIGSEGV, acceso a memoria no válida";
    case SIGBUS:  return "SIGBUS, acceso a memoria no válida";
    case SIGILL:  return "SIGILL, instrucción ilegal";
    case SIGFPE:  return "SIGFPE, error aritmético";
    case SIGABRT: return "SIGABRT, el programa se interrumpió a sí mismo";
    default:      return "señal desconocida";
    }
}

void onFatalSignal(int number, siginfo_t *info, void *rawContext)
{
    put("\n=== FALLO (");
    put(signalName(number));
    put(")\nHora (UTC): ");
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    putNumber(unsigned(utc.tm_year + 1900), 10, 4);
    put("-");
    putNumber(unsigned(utc.tm_mon + 1), 10, 2);
    put("-");
    putNumber(unsigned(utc.tm_mday), 10, 2);
    put(" ");
    putNumber(unsigned(utc.tm_hour), 10, 2);
    put(":");
    putNumber(unsigned(utc.tm_min), 10, 2);
    put(":");
    putNumber(unsigned(utc.tm_sec), 10, 2);
    put("\n");
    put(g_header.constData());
    if (info != nullptr && (number == SIGSEGV || number == SIGBUS)) {
        put("Dirección a la que se intentó acceder: ");
        putAddress(info->si_addr);
        put("\n");
    }

    // Dónde estaba el programa. Si saltó a una dirección que no es código
    // (un puntero a función roto), la pila no se puede recorrer desde ahí:
    // se dice quién hizo la llamada, que es lo que hace falta saber.
    void *instruction = nullptr;
    void *stackPointer = nullptr;
    void *caller = nullptr;
    bool knownPlace = false;   // se pudo leer dónde estaba (aunque sea la dirección 0)
#if defined(Q_OS_LINUX) && defined(__x86_64__)
    const auto *context = static_cast<const ucontext_t *>(rawContext);
    knownPlace = true;
    instruction = reinterpret_cast<void *>(static_cast<std::uintptr_t>(context->uc_mcontext.gregs[REG_RIP]));
    stackPointer = reinterpret_cast<void *>(static_cast<std::uintptr_t>(context->uc_mcontext.gregs[REG_RSP]));
    if (stackPointer != nullptr) {
        caller = *static_cast<void **>(stackPointer);   // la llamada acaba de dejar ahí su dirección de vuelta
    }
#elif defined(Q_OS_LINUX) && defined(__aarch64__)
    const auto *context = static_cast<const ucontext_t *>(rawContext);
    knownPlace = true;
    instruction = reinterpret_cast<void *>(static_cast<std::uintptr_t>(context->uc_mcontext.pc));
    stackPointer = reinterpret_cast<void *>(static_cast<std::uintptr_t>(context->uc_mcontext.sp));
    caller = reinterpret_cast<void *>(static_cast<std::uintptr_t>(context->uc_mcontext.regs[30]));
#elif defined(Q_OS_MACOS) && defined(__x86_64__)
    const auto *context = static_cast<const ucontext_t *>(rawContext);
    knownPlace = true;
    instruction = reinterpret_cast<void *>(static_cast<std::uintptr_t>(context->uc_mcontext->__ss.__rip));
    stackPointer = reinterpret_cast<void *>(static_cast<std::uintptr_t>(context->uc_mcontext->__ss.__rsp));
    if (stackPointer != nullptr) {
        caller = *static_cast<void **>(stackPointer);
    }
#elif defined(Q_OS_MACOS) && defined(__aarch64__)
    const auto *context = static_cast<const ucontext_t *>(rawContext);
    knownPlace = true;
    instruction = reinterpret_cast<void *>(
        static_cast<std::uintptr_t>(__darwin_arm_thread_state64_get_pc(context->uc_mcontext->__ss)));
    stackPointer = reinterpret_cast<void *>(
        static_cast<std::uintptr_t>(__darwin_arm_thread_state64_get_sp(context->uc_mcontext->__ss)));
    caller = reinterpret_cast<void *>(
        static_cast<std::uintptr_t>(__darwin_arm_thread_state64_get_lr(context->uc_mcontext->__ss)));
#else
    (void)rawContext;
#endif
    if (knownPlace) {
        put("Instrucción:\n");
        putSymbol(instruction);
        if (!isInLoadedCode(instruction)) {
            put(invalidPointerNote);
            if (caller != nullptr) {
                put("Llamada desde:\n");
                putSymbol(caller);
            }
        }
    }

    put("Pila de llamadas:\n");
    void *frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, g_fd);

    if (stackPointer != nullptr) {
        // Por si la pila de arriba se corta: las direcciones de funciones que
        // quedan en la pila suelen ser las de quienes llamaron.
        put("Direcciones de funciones guardadas en la pila (aproximado, de la más reciente a la más antigua):\n");
        auto *const *words = static_cast<void *const *>(stackPointer);
        int shown = 0;
        // La pila puede acabar antes de 4 KB (llamadas poco hondas): leer más
        // allá volvía a fallar dentro del informe y lo dejaba sin terminar.
        // Antes de entrar en cada página se comprueba que existe (msync falla
        // con ENOMEM si no está en el mapa del proceso).
        const long pageSize = ::sysconf(_SC_PAGESIZE);
        uintptr_t checkedPage = 0;
        for (int i = 0; i < 512 && shown < 40; ++i) {
            const uintptr_t page = reinterpret_cast<uintptr_t>(&words[i]) & ~uintptr_t(pageSize - 1);
            if (page != checkedPage) {
                if (::msync(reinterpret_cast<void *>(page), size_t(pageSize), MS_ASYNC) != 0) {
                    break;
                }
                checkedPage = page;
            }
            Dl_info symbol;
            if (dladdr(words[i], &symbol) != 0 && symbol.dli_sname != nullptr) {
                putSymbol(words[i]);
                ++shown;
            }
        }
    }
    put("=== fin del informe\n");
    // SA_RESETHAND ya devolvió la señal a su efecto normal: al volver, el
    // proceso termina como lo habría hecho sin este informe.
    ::raise(number);
}

#else // Q_OS_WIN

HANDLE g_file = INVALID_HANDLE_VALUE;
wchar_t g_directory[1024] = {};

void put(const char *text)
{
    DWORD written = 0;
    WriteFile(g_file, text, DWORD(std::strlen(text)), &written, nullptr);
}

template<typename... Args>
void putFormat(const char *format, Args... args)
{
    char line[1024];
    std::snprintf(line, sizeof line, format, args...);
    put(line);
}

/// Una dirección con su módulo, desplazamiento y, si está el .pdb, función y línea.
void putFrame(DWORD64 address)
{
    HMODULE module = nullptr;
    char moduleName[MAX_PATH] = "?";
    DWORD64 offset = address;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module)
        && module != nullptr) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char *slash = std::strrchr(path, '\\');
        strncpy_s(moduleName, slash != nullptr ? slash + 1 : path, _TRUNCATE);
        offset = address - reinterpret_cast<DWORD64>(module);
    }
    putFormat("  0x%016llx  %s+0x%llx", static_cast<unsigned long long>(address), moduleName,
              static_cast<unsigned long long>(offset));
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
        putFormat("  %s+0x%llx", symbol->Name, static_cast<unsigned long long>(displacement));
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof line;
        DWORD lineDisplacement = 0;
        if (SymGetLineFromAddr64(GetCurrentProcess(), address, &lineDisplacement, &line)) {
            putFormat("  (%s:%lu)", line.FileName, line.LineNumber);
        }
    }
    put("\n");
}

bool isInLoadedCode(DWORD64 address)
{
    HMODULE module = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCSTR>(address), &module)
        && module != nullptr;
}

const char *exceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "acceso a memoria no válida";
    case EXCEPTION_STACK_OVERFLOW:        return "pila agotada";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "instrucción ilegal";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "división entera por cero";
    case EXCEPTION_IN_PAGE_ERROR:         return "no se pudo leer una página de memoria (¿disco desconectado?)";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "fuera de los límites";
    case 0xE06D7363:                      return "excepción de C++ sin capturar";
    default:                              return "excepción";
    }
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS *exception)
{
    static LONG entered = 0;
    if (InterlockedExchange(&entered, 1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;   // un fallo dentro del informe: no se insiste
    }
    const EXCEPTION_RECORD *record = exception->ExceptionRecord;
    SYSTEMTIME now;
    GetLocalTime(&now);
    putFormat("\n=== FALLO (%s, 0x%08lx)\nHora: %04u-%02u-%02u %02u:%02u:%02u\n", exceptionName(record->ExceptionCode),
              static_cast<unsigned long>(record->ExceptionCode), unsigned(now.wYear), unsigned(now.wMonth),
              unsigned(now.wDay), unsigned(now.wHour), unsigned(now.wMinute), unsigned(now.wSecond));
    put(g_header.constData());
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const ULONG_PTR kind = record->ExceptionInformation[0];
        putFormat("Intento de %s en 0x%llx\n", kind == 0 ? "leer" : kind == 1 ? "escribir" : "ejecutar",
                  static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);   // busca el .pdb junto a cada módulo

    CONTEXT context = *exception->ContextRecord;
    STACKFRAME64 frame = {};
    DWORD machine = 0;
#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    put("Instrucción:\n");
    putFrame(context.Rip);
    if (!isInLoadedCode(context.Rip)) {
        put(invalidPointerNote);
        // Se sigue desde quien hizo la llamada: su dirección de vuelta está en lo alto de la pila.
        context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
        context.Rsp += 8;
    }
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_ARM64)
    machine = IMAGE_FILE_MACHINE_ARM64;
    put("Instrucción:\n");
    putFrame(context.Pc);
    if (!isInLoadedCode(context.Pc)) {
        put(invalidPointerNote);
        context.Pc = context.Lr;
    }
    frame.AddrPC.Offset = context.Pc;
    frame.AddrFrame.Offset = context.Fp;
    frame.AddrStack.Offset = context.Sp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;
    put("Pila de llamadas:\n");
    for (int depth = 0; machine != 0 && depth < 64; ++depth) {
        if (!StackWalk64(machine, GetCurrentProcess(), GetCurrentThread(), &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr)
            || frame.AddrPC.Offset == 0) {
            break;
        }
        putFrame(frame.AddrPC.Offset);
    }

    // El volcado: con él y el .pdb de la compilación se ve todo el estado.
    wchar_t dumpPath[1200];
    swprintf_s(dumpPath, L"%s\\fallo-%04u%02u%02u-%02u%02u%02u.dmp", g_directory, unsigned(now.wYear),
               unsigned(now.wMonth), unsigned(now.wDay), unsigned(now.wHour), unsigned(now.wMinute),
               unsigned(now.wSecond));
    const HANDLE dump = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION information = {};
        information.ThreadId = GetCurrentThreadId();
        information.ExceptionPointers = exception;
        information.ClientPointers = FALSE;
        const BOOL written = MiniDumpWriteDump(
            GetCurrentProcess(), GetCurrentProcessId(), dump,
            MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo), &information, nullptr,
            nullptr);
        CloseHandle(dump);
        char utf8[2400] = {};
        WideCharToMultiByte(CP_UTF8, 0, dumpPath, -1, utf8, int(sizeof utf8), nullptr, nullptr);
        putFormat(written ? "Volcado: %s\n" : "No se pudo escribir el volcado %s\n", utf8);
    }
    put("=== fin del informe\n");
    FlushFileBuffers(g_file);
    return EXCEPTION_CONTINUE_SEARCH;   // Windows termina el proceso como siempre
}

#endif // Q_OS_WIN
#endif // !EU_CRASH_REPORT_DISABLED

} // namespace

QString crashLogFile(const QString &directory)
{
    return QDir(directory).filePath(QStringLiteral("fallos.log"));
}

bool installCrashReport(const QString &directory)
{
#if defined(EU_CRASH_REPORT_DISABLED)
    Q_UNUSED(directory);
    return false;
#else
    QDir().mkpath(directory);
    g_header = QStringLiteral("%1 %2 (Qt %3), proceso %4\nEjecutable: %5\n")
                   .arg(applicationName(), versionString(), QString::fromLatin1(qVersion()))
                   .arg(QCoreApplication::applicationPid())
                   .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()))
                   .toUtf8();
#if defined(Q_OS_WIN)
    const std::wstring path = QDir::toNativeSeparators(crashLogFile(directory)).toStdWString();
    const HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
    }
    g_file = file;
    wcsncpy_s(g_directory, QDir::toNativeSeparators(directory).toStdWString().c_str(), _TRUNCATE);
    SetUnhandledExceptionFilter(onUnhandledException);
#else
    const QByteArray path = QFile::encodeName(crashLogFile(directory));
    const int fd = ::open(path.constData(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    if (g_fd >= 0) {
        ::close(g_fd);
    }
    g_fd = fd;
    // backtrace() carga su biblioteca (y reserva memoria) la primera vez:
    // mejor ahora que dentro del manejador.
    void *warmUp[2];
    backtrace(warmUp, 2);

    stack_t alternate{};
    alternate.ss_sp = g_alternateStack;
    alternate.ss_size = sizeof g_alternateStack;
    sigaltstack(&alternate, nullptr);
    struct sigaction action{};
    action.sa_sigaction = onFatalSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    for (const int number : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
        sigaction(number, &action, nullptr);
    }
#endif
    return true;
#endif
}

} // namespace eu::core
