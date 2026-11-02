/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "win_crash_report.h"

#if defined(_WIN32) && defined(_M_X64)

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{

    // Exported names past this displacement are a guess (the frame is in a
    // non-exported function after the nearest export)
    constexpr DWORD64 kReliableDisplacement = 0x1000;

    void printFrame(HANDLE process, DWORD64 address)
    {
        char modulePath[MAX_PATH] = "?";
        HMODULE module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(address), &module))
            GetModuleFileNameA(module, modulePath, MAX_PATH);
        const char *slash = std::strrchr(modulePath, '\\');
        const char *moduleName = slash ? slash + 1 : modulePath;
        const DWORD64 rva = module ? address - reinterpret_cast<DWORD64>(module) : address;

        // module+RVA always, so a frame can be resolved offline against the PDBs
        std::fprintf(stderr, "  %s+0x%llx", moduleName, rva);

        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, address, &displacement, symbol))
            std::fprintf(stderr, "  %s+0x%llx%s", symbol->Name, displacement,
                         displacement > kReliableDisplacement ? " (nearest export, unreliable)" : "");
        std::fputc('\n', stderr);
    }

    // Search the executable's folder (where the build puts ddb.dll and the PDBs) and
    // then _NT_SYMBOL_PATH, which CI points at the vcpkg PDBs
    std::string symbolSearchPath()
    {
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string path(exePath);
        const auto slash = path.find_last_of('\\');
        path = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
        if (const char *env = std::getenv("_NT_SYMBOL_PATH"))
            path += std::string(";") + env;
        return path;
    }

    LONG WINAPI reportUnhandledException(EXCEPTION_POINTERS *info)
    {
        const EXCEPTION_RECORD *record = info->ExceptionRecord;
        std::fprintf(stderr, "\n=== ddbtest: unhandled exception 0x%08lX at %p on thread %lu ===\n",
                     record->ExceptionCode, record->ExceptionAddress, GetCurrentThreadId());
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        {
            const ULONG_PTR kind = record->ExceptionInformation[0];
            std::fprintf(stderr, "  %s of address %p\n",
                         kind == 0 ? "read" : kind == 1 ? "write" : "execute",
                         reinterpret_cast<void *>(record->ExceptionInformation[1]));
        }

        const HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, symbolSearchPath().c_str(), TRUE);

        CONTEXT context = *info->ContextRecord;
        STACKFRAME64 frame = {};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 64; i++)
        {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context,
                             nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                frame.AddrPC.Offset == 0)
                break;
            printFrame(process, frame.AddrPC.Offset);
        }
        std::fflush(stderr);

        // Keep the default handling: WER writes the dump and the exit code stays the
        // exception code.
        return EXCEPTION_CONTINUE_SEARCH;
    }

} // namespace

void installWindowsCrashReport()
{
    SetUnhandledExceptionFilter(&reportUnhandledException);
}

#else

void installWindowsCrashReport() {}

#endif
