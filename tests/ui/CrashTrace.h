// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// A stack, when a test process dies of an access violation.
//
// gtest reports one as "SEH exception with code 0xc0000005 thrown in the test body" and a stack
// trace that is empty, which is the whole of what it knows. For a crash that happens once in a
// thousand runs - and only then, on a machine under load - that is not a message anybody can act
// on, and the next run will not reproduce it. This prints the address that was touched and the
// functions that were running, with their lines, to stderr, which ctest keeps for a failed test.
//
// A first-chance handler, so it sees the fault before anything else does; it prints and lets the
// exception go on, so gtest and the process behave exactly as they would without it. It prints
// for the first few faults only: some code probes memory on purpose and recovers.
//
// MSVC only, and a debug or RelWithDebInfo build for the names; without symbols it prints
// addresses.

#pragma once

#ifdef _MSC_VER

#include <windows.h>

#include <dbghelp.h>

#include <atomic>
#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

namespace torquebus::testing_support {

inline LONG WINAPI reportAccessViolation(EXCEPTION_POINTERS* info)
{
    if (info == nullptr || info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static std::atomic<int> reported{0};
    if (reported.fetch_add(1) >= 3) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    const char* kind = record.ExceptionInformation[0] == 0   ? "reading"
                       : record.ExceptionInformation[0] == 1 ? "writing"
                                                             : "executing";

    std::fprintf(stderr,
                 "\n[crash trace] access violation %s address 0x%llX, at 0x%llX\n",
                 kind,
                 static_cast<unsigned long long>(record.ExceptionInformation[1]),
                 reinterpret_cast<unsigned long long>(record.ExceptionAddress));

    const HANDLE process = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);

    CONTEXT context = *info->ContextRecord;

    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    for (int depth = 0; depth < 40; ++depth) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64,
                         process,
                         thread,
                         &frame,
                         &context,
                         nullptr,
                         SymFunctionTableAccess64,
                         SymGetModuleBase64,
                         nullptr)
            || frame.AddrPC.Offset == 0) {
            break;
        }

        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;

        DWORD64 displacement = 0;
        const bool named = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) != 0;

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        const bool located =
            SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line) != 0;

        std::fprintf(stderr,
                     "[crash trace] %2d  0x%016llX  %s  %s:%lu\n",
                     depth,
                     static_cast<unsigned long long>(frame.AddrPC.Offset),
                     named ? symbol->Name : "?",
                     located ? line.FileName : "",
                     located ? static_cast<unsigned long>(line.LineNumber) : 0UL);
    }

    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

/// Installs the handler. Called once, first thing in main().
inline void installCrashTrace()
{
    AddVectoredExceptionHandler(1, reportAccessViolation);
}

} // namespace torquebus::testing_support

#else

namespace torquebus::testing_support {
inline void installCrashTrace() { }
} // namespace torquebus::testing_support

#endif
