// SPDX-License-Identifier: GPL-3.0-or-later
// function-call service for the unify360 plugin.
//
// every call runs on a fresh short-lived thread and is waited on with a timeout. a
// target that never returns (a bad address, or a function that blocks) therefore
// cannot wedge the xbdm server thread or the caller - it just reports a timeout and
// the orphaned thread is left parked (recovered by a reboot). this is the same
// isolation the native rpc command uses.
#ifndef UNIFY_CALL_H
#define UNIFY_CALL_H
#include "Kernel.h"

extern "C" unsigned long long UnifyCallGate(void* fn, const unsigned int* args8);
extern "C" unsigned long long UnifyCallGateF(void* fn, const unsigned int* gpr8,
                                             const double* fpr8, double* fret);

namespace unify {

struct CallJob {
    void* fn;
    unsigned int args[8];
    volatile unsigned long long result;
    volatile int done;
};

inline DWORD WINAPI CallThunk(LPVOID p) {
    CallJob* j = (CallJob*)p;
    j->result = UnifyCallGate(j->fn, j->args);
    __sync_synchronize();
    j->done = 1;
    return 0;
}

// resolve module+ordinal to a runtime address (0 on failure).
inline void* Resolve(const char* module, unsigned int ordinal) {
    HMODULE h = 0; void* fn = 0;
    if (XexGetModuleHandle(module, &h) == 0)
        XexGetProcedureAddress(h, ordinal, &fn);
    return fn;
}

// call fn with up to 8 int args (extra slots must be zero). ok is set false on a
// timeout or a failure to launch. runs on a fresh thread; isolated and bounded.
inline unsigned long long Call(void* fn, const unsigned int* args, int argc,
                               bool& ok, unsigned timeout_ms = 4000) {
    ok = false;
    if (!fn) return 0;
    CallJob* job = (CallJob*)ExAllocatePool(sizeof(CallJob));
    if (!job) return 0;
    job->fn = fn; job->result = 0; job->done = 0;
    for (int i = 0; i < 8; ++i) job->args[i] = (i < argc && args) ? args[i] : 0u;
    __sync_synchronize();
    HANDLE h = 0; DWORD id = 0;
    if (ExCreateThread(&h, 0x8000, &id, (PVOID)XapiThreadStartup, (PVOID)CallThunk,
                       job, 0x10000000u | CREATE_SUSPENDED) != 0) {
        ExFreePool(job);
        return 0;
    }
    ResumeThread(h);
    // poll for completion up to the timeout
    unsigned waited = 0;
    while (!job->done && waited < timeout_ms) { Sleep(10); waited += 10; }
    if (job->done) {
        unsigned long long r = job->result;
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        ExFreePool(job);
        ok = true;
        return r;
    }
    // timed out: leak the job + handle (the parked thread still owns the job).
    ok = false;
    return 0;
}

inline unsigned long long CallAddr(unsigned int address, const unsigned int* args,
                                   int argc, bool& ok) {
    return Call((void*)(size_t)address, args, argc, ok);
}

inline unsigned long long CallExport(const char* module, unsigned int ordinal,
                                     const unsigned int* args, int argc, bool& ok) {
    void* fn = Resolve(module, ordinal);
    if (!fn) { ok = false; return 0; }
    return Call(fn, args, argc, ok);
}

// float-capable call: gpr8 holds each arg's slot value (r3..r10), fpr8 holds the
// float args as doubles in order (f1..f8). fret receives the float result (f1).
// isolated on a fresh thread with a timeout, same as Call.
struct CallJobF {
    void* fn;
    unsigned int gpr[8];
    double fpr[8];
    volatile unsigned long long iret;
    double fret;
    volatile int done;
};
inline DWORD WINAPI CallThunkF(LPVOID p) {
    CallJobF* j = (CallJobF*)p;
    j->iret = UnifyCallGateF(j->fn, j->gpr, j->fpr, &j->fret);
    __sync_synchronize();
    j->done = 1;
    return 0;
}
inline unsigned long long CallF(void* fn, const unsigned int* gpr8, const double* fpr8,
                                bool& ok, double* fret_out, unsigned timeout_ms = 4000) {
    ok = false;
    if (fret_out) *fret_out = 0.0;
    if (!fn) return 0;
    CallJobF* job = (CallJobF*)ExAllocatePool(sizeof(CallJobF));
    if (!job) return 0;
    job->fn = fn; job->iret = 0; job->fret = 0.0; job->done = 0;
    for (int i = 0; i < 8; ++i) { job->gpr[i] = gpr8 ? gpr8[i] : 0u; job->fpr[i] = fpr8 ? fpr8[i] : 0.0; }
    __sync_synchronize();
    HANDLE h = 0; DWORD id = 0;
    if (ExCreateThread(&h, 0x8000, &id, (PVOID)XapiThreadStartup, (PVOID)CallThunkF,
                       job, 0x10000000u | CREATE_SUSPENDED) != 0) {
        ExFreePool(job);
        return 0;
    }
    ResumeThread(h);
    unsigned waited = 0;
    while (!job->done && waited < timeout_ms) { Sleep(10); waited += 10; }
    if (job->done) {
        unsigned long long r = job->iret;
        if (fret_out) *fret_out = job->fret;
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
        ExFreePool(job);
        ok = true;
        return r;
    }
    ok = false;  // timed out: leak the job (the parked thread still owns it)
    return 0;
}

}  // namespace unify
#endif
