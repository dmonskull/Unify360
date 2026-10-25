// SPDX-License-Identifier: GPL-3.0-or-later
// minimal openxechain platform shims for the unify360 plugin. no microsoft sdk.
#ifndef UNIFY_KERNEL_H
#define UNIFY_KERNEL_H
#include <stdint.h>
#include <xecore/xboxkrnl_modules.h>
#include <xecore/xboxkrnl_threads.h>
#include <xecore/xboxkrnl_io.h>
#include <xecore/xboxkrnl_mem.h>
#include <string.h>

typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef int BOOL;
typedef void VOID;
typedef void* PVOID;
typedef void* LPVOID;
#define TRUE 1
#define FALSE 0
#define WINAPI
#define APIENTRY
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define CREATE_SUSPENDED 4
#define INFINITE 0xffffffffu

inline void* ModuleExport(const char* name, DWORD ordinal) {
    HMODULE module = 0; void* fn = 0;
    if (XexGetModuleHandle(name, &module) == 0)
        XexGetProcedureAddress(module, ordinal, &fn);
    return fn;
}
inline void* XamExport(DWORD ordinal) { return ModuleExport("xam.xex", ordinal); }

inline DWORD GetCurrentThreadId() {
    typedef DWORD (*Fn)();
    static Fn fn = (Fn)XamExport(1040);
    return fn ? fn() : 0;
}
inline void Sleep(DWORD ms) {
    int64_t interval = -(int64_t)ms * 10000;
    KeDelayExecutionThread(0, 0, &interval);
}
inline DWORD ResumeThread(HANDLE h) {
    DWORD prev = 0;
    return NtResumeThread(h, &prev) == 0 ? prev : 0xffffffffu;
}
inline DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    int64_t interval = -(int64_t)ms * 10000;
    return NtWaitForSingleObjectEx((uint32_t)(size_t)h, 0, 0,
                                   ms == INFINITE ? 0 : &interval);
}
inline void CloseHandle(HANDLE h) { NtClose(h); }
// the loader thunk the kernel expects: run the start routine, then terminate.
inline void XapiThreadStartup(DWORD (*start)(LPVOID), LPVOID ctx) {
    ExTerminateThread(start(ctx));
}
#endif
