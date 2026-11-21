// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
//
// pilas: imprime la pila de cada hilo de un proceso en marcha, con los nombres
// de las funciones y las líneas (de los .pdb que haya junto a los .exe y .dll).
// Sirve para ver dónde está una aplicación colgada sin instalar un depurador.
//
//   Compilar (en la consola de Visual Studio):  cl /nologo /EHsc /O1 pilas.cpp dbghelp.lib
//   Usar:                                       pilas <PID>
//
// Detiene un instante cada hilo para leer su estado y lo deja seguir.

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

int wmain(int argc, wchar_t *argv[])
{
    if (argc != 2) {
        std::fwprintf(stderr, L"Uso: pilas <PID>\n");
        return 1;
    }
    const DWORD pid = DWORD(std::wcstoul(argv[1], nullptr, 10));
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) {
        std::fwprintf(stderr, L"No se pudo abrir el proceso %lu (error %lu)\n", pid, GetLastError());
        return 1;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (!SymInitializeW(process, nullptr, TRUE)) {
        std::fwprintf(stderr, L"SymInitialize falló (error %lu)\n", GetLastError());
        return 1;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::vector<DWORD> threads;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == pid) {
            threads.push_back(entry.th32ThreadID);
        }
    }
    CloseHandle(snapshot);

    for (const DWORD id : threads) {
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
        if (thread == nullptr) {
            continue;
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        GetThreadTimes(thread, &created, &exited, &kernel, &user);
        const auto ms = [](const FILETIME &t) {
            return (ULONGLONG(t.dwHighDateTime) << 32 | t.dwLowDateTime) / 10000;
        };
        std::printf("=== hilo %lu (CPU: usuario %llu ms, núcleo %llu ms)\n", id, ms(user), ms(kernel));
        SuspendThread(thread);
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context)) {
            STACKFRAME64 frame{};
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            for (int depth = 0; depth < 64; ++depth) {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr)
                    || frame.AddrPC.Offset == 0) {
                    break;
                }
                const DWORD64 address = frame.AddrPC.Offset;
                char buffer[sizeof(SYMBOL_INFO) + 512] = {};
                auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 511;
                DWORD64 displacement = 0;
                IMAGEHLP_MODULE64 module{};
                module.SizeOfStruct = sizeof(module);
                const char *moduleName = SymGetModuleInfo64(process, address, &module) ? module.ModuleName : "?";
                if (SymFromAddr(process, address, &displacement, symbol)) {
                    IMAGEHLP_LINE64 line{};
                    line.SizeOfStruct = sizeof(line);
                    DWORD column = 0;
                    if (SymGetLineFromAddr64(process, address, &column, &line)) {
                        const char *file = line.FileName;
                        for (const char *p = line.FileName; *p != '\0'; ++p) {
                            if (*p == '\\') {
                                file = p + 1;   // solo el nombre del archivo
                            }
                        }
                        std::printf("  %s!%s  (%s:%lu)\n", moduleName, symbol->Name, file, line.LineNumber);
                    } else {
                        std::printf("  %s!%s+0x%llx\n", moduleName, symbol->Name, displacement);
                    }
                } else {
                    std::printf("  %s+0x%llx\n", moduleName, address - module.BaseOfImage);
                }
            }
        }
        ResumeThread(thread);
        CloseHandle(thread);
    }
    SymCleanup(process);
    CloseHandle(process);
    return 0;
}
