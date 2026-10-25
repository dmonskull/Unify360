// SPDX-License-Identifier: GPL-3.0-or-later
// unify360 console plugin.
//
// loads as a resident dashlaunch system dll. on load the worker:
//   1. registers a new xbdm command "unify" (safe: DmRegisterCommandProcessor, no
//      xbdm patching). this carries our own premium api: unify!call, unify!xrpc, etc.
//   2. starts the xrpc shared-buffer poller so classic xrpc pc tools work (pure ram).
//   3. installs the consolefeatures hook so jrpc/jrpc2 pc tools work - but only if
//      JRPC2.xex is NOT already resident, and only after positively identifying the
//      xbdm table entry. if anything is uncertain it stands down and leaves xbdm
//      untouched.
// the whole point: run this xex and old jrpc/xdrpc/xrpc tools just work, while nothing
// already loaded is disturbed. native rpc (xdrpc) is answered by xbdm itself on a
// devkit, so we leave it alone.
#include "Kernel.h"
#include "Call.h"
#include "Xrpc.h"
#include "ConsoleFeatures.h"
#include "Rpc.h"
#include "MemHook.h"

#define LOAD_BASE   0x91E00000u
#define XBDM_NOERR  0x02DA0000

typedef int (*DmRegisterCommandProcessor_t)(const char* name, void* proc);

// diagnostic block (read big endian over xbdm). magic UNIFY001 then:
// [2]=state [3]=register-rc [4]=heartbeat [6]=cmd-count [7]=xrpc-buffer-addr
// [8]=xrpc-pointer-published [9]=consolefeatures-hook-state [14]=worker tid
__declspec(align(16)) static volatile uint32_t g_status[16] = {
    0x554E4946u, 0x59303031u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
static void Fence() { __sync_synchronize(); }

static unify::XrpcState g_xrpc;

// --- tiny no-CRT text helpers for unify! responses ---
static char* sstr(char* d, const char* s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char* shex(char* d, uint32_t v) {
    const char* H = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) *d++ = H[(v >> i) & 0xF];
    *d = 0; return d;
}
static char* sdec(char* d, uint32_t v) {
    char t[12]; int n = 0;
    if (!v) { *d++ = '0'; *d = 0; return d; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *d++ = t[--n];
    *d = 0; return d;
}
static bool starts(const char* a, const char* b) {
    while (*b) { if (*a != *b) return false; ++a; ++b; }
    return true;
}
static const char* skipword(const char* p) {  // to next token after spaces
    while (*p && *p != ' ') ++p;
    while (*p == ' ') ++p;
    return p;
}

// the "unify" command. cmd is the whole line; skip past '!'.
static int UnifyCommand(const char* cmd, char* resp, uint32_t cchResp, void* /*cont*/) {
    ++g_status[6]; Fence();
    const char* p = cmd;
    while (*p && *p != '!') ++p;
    if (*p == '!') ++p;
    if (cchResp < 64) { resp[0] = 0; return XBDM_NOERR; }
    char* out = resp;

    if (*p == 0 || starts(p, "ping")) {
        out = sstr(out, "unify ok v=0.2 base=0x");
        out = shex(out, LOAD_BASE);
        out = sstr(out, " beats="); out = sdec(out, g_status[4]);
        out = sstr(out, " xrpc=0x"); out = shex(out, g_status[7]);
        out = sstr(out, " cfhook="); out = sdec(out, g_status[9]);
    } else if (starts(p, "status")) {
        out = sstr(out, "state="); out = sdec(out, g_status[2]);
        out = sstr(out, " regrc=0x"); out = shex(out, g_status[3]);
        out = sstr(out, " xrpcbuf=0x"); out = shex(out, g_status[7]);
        out = sstr(out, " xrpcptr="); out = sdec(out, g_status[8]);
        out = sstr(out, " cfhook="); out = sdec(out, g_status[9]);
        out = sstr(out, " tid=0x"); out = shex(out, g_status[14]);
    } else if (starts(p, "xrpc")) {
        out = sstr(out, "buffer=0x"); out = shex(out, g_xrpc.buffer_addr);
        out = sstr(out, " published="); out = sdec(out, g_xrpc.pointer_published ? 1 : 0);
    } else if (starts(p, "hook ")) {
        const char* a = p + 5;
        if (starts(a, "off")) { unify::Unhook(); g_status[9] = (uint32_t)unify::g_cf.installed; out = sstr(out, "hook off"); }
        else if (starts(a, "on")) { g_status[9] = (uint32_t)unify::InstallHook(); out = sstr(out, "hook rc="); out = sdec(out, g_status[9]); }
        else { out = sstr(out, "hook state="); out = sdec(out, (uint32_t)unify::g_cf.installed); }
    } else if (starts(p, "cpukey ")) {
        const char* q = p + 7;
        if (starts(q, "addr=")) { q += 5; if (starts(q, "0x")) q += 2; unify::g_cpukey_addr = unify::ReadHex(q); }
        out = sstr(out, "cpukey source=0x"); out = shex(out, unify::g_cpukey_addr);
    } else if (starts(p, "call ")) {
        // unify!call addr=0xADDR a=N fa=1.5 ... [ret=i|u|f|v|s]   OR   mod=NAME ord=O ...
        // a= integer arg, fa= float arg. returns the result, or "err ..." on failure.
        const char* q = p + 5;
        unsigned int address = 0, ordinal = 0; char module[64]; module[0] = 0;
        unsigned int gpr[8]; double fpr[8];
        for (int i = 0; i < 8; ++i) { gpr[i] = 0; fpr[i] = 0.0; }
        int slot = 0, fprIdx = 0; char ret = 'i';
        while (*q) {
            if (starts(q, "addr=")) { q += 5; if (starts(q, "0x")) q += 2; address = unify::ReadHex(q); }
            else if (starts(q, "mod=")) { q += 4; int i = 0; while (*q && *q != ' ' && i < 63) module[i++] = *q++; module[i] = 0; }
            else if (starts(q, "ord=")) { q += 4; ordinal = unify::ReadDec(q); }
            else if (starts(q, "ret=")) { q += 4; ret = *q; }
            else if (starts(q, "fa=") && slot < 8) { q += 3; double d = unify::ParseDouble(q); if (fprIdx < 8) fpr[fprIdx++] = d; gpr[slot] = 0; ++slot; }
            else if (starts(q, "a=") && slot < 8) { q += 2; if (starts(q, "0x")) { q += 2; gpr[slot] = unify::ReadHex(q); } else gpr[slot] = unify::ReadDec(q); ++slot; }
            q = skipword(q);
        }
        void* fn = module[0] ? unify::Resolve(module, ordinal) : (void*)(size_t)address;
        if (!fn) { out = sstr(out, "err unresolved"); }
        else {
            bool ok = false; double fret = 0.0;
            unsigned long long r = unify::CallF(fn, gpr, fpr, ok, &fret);
            if (!ok) out = sstr(out, "err timeout");
            else if (ret == 'v') out = sstr(out, "ok");
            else if (ret == 'f') out = unify::PutDouble(out, fret);
            else if (ret == 'u') { out = shex(out, (uint32_t)(r >> 32)); out = shex(out, (uint32_t)r); }
            else out = shex(out, (uint32_t)r);
        }
    } else {
        out = sstr(out, "unknown unify subcommand (ping|status|xrpc|hook|call)");
    }
    (void)out;
    return XBDM_NOERR;
}

static volatile bool g_stop = false;
static HANDLE g_worker = 0;

static DWORD WINAPI Run(LPVOID) {
    g_status[14] = GetCurrentThreadId();
    g_status[2] = 1; Fence();

    // 1) register our own command (safe, no xbdm patching)
    void* reg = ModuleExport("xbdm.xex", 30);  // DmRegisterCommandProcessor
    if (reg) {
        int rc = ((DmRegisterCommandProcessor_t)reg)("unify", (void*)UnifyCommand);
        g_status[3] = (uint32_t)rc;
        g_status[2] = 2;
    } else {
        g_status[2] = 0xE;
    }
    Fence();

    // 2) xrpc shared-buffer poller (pure ram; never touches xbdm)
    if (unify::XrpcInit(g_xrpc)) {
        g_status[7] = g_xrpc.buffer_addr;
        g_status[8] = g_xrpc.pointer_published ? 1 : 0;
    }
    Fence();

    // 3) consolefeatures hook for jrpc tools. auto-installed a moment after boot (not
    //    instantly, so xbdm's command table is fully initialized first). the hook is
    //    self-validating - it only patches a genuine consolefeatures table entry and
    //    otherwise stands down - so auto-install is safe across xbdm builds. the memory
    //    trigger (status[10]) stays available to unhook/re-install for control.
    g_status[9] = 0xFF;  // 0xFF = pending auto-install
    Fence();
    bool autohooked = false;

    // heartbeat + xrpc poll + constant-memory service loop
    unsigned int tick = 0;
    while (!g_stop) {
        ++g_status[4];
        if (g_xrpc.buffer) unify::XrpcPollOnce(g_xrpc);
        unify::ServiceConstMem();
        // auto-install the consolefeatures hook ~1s into the worker (after xbdm settles)
        if (!autohooked && g_status[4] > 50) {
            g_status[9] = (uint32_t)unify::InstallHook();       // consolefeatures (JRPC)
            // rpc (XDRPC): the version=4 binary-transfer handler is now verified against
            // this xbdm's continuation ABI (async worker + 997 keep-alive streaming, no
            // xbdm-thread blocking), so it auto-installs alongside the jrpc hook. the
            // status[11] trigger stays available to uninstall/reinstall for control.
            g_status[12] = (uint32_t)unify::InstallRpc();       // rpc (XDRPC)
            // getmem/getmemex read hook: synthesizes the classic XRPC pointer read at
            // 0x91C01A2E so unmodified XRPCLib tools find our buffer. forwards every
            // other read unmodified, so normal memory access is untouched.
            g_status[5] = (uint32_t)unify::InstallMemHook(g_xrpc.buffer_addr);
            autohooked = true; Fence();
        }
        // status[11] = 1 installs rpc (repurpose "help"); 2 uninstalls. for controlled
        // testing only, since a bad rpc handler can wedge xbdm's command loop.
        if (g_status[11] == 1) { g_status[12] = (uint32_t)unify::InstallRpc(); g_status[11] = 0; Fence(); }
        else if (g_status[11] == 2) { unify::UninstallRpc(); g_status[12] = (uint32_t)unify::g_rpc.installed; g_status[11] = 0; Fence(); }
        // status[13] = 1: install the getmem/getmemex read hook so stock XRPCLib tools
        // reading the classic pointer 0x91C01A2E get our buffer base; 2 = uninstall. the
        // hook forwards every other read unmodified. result -> [5] (1 ok, 2 no base, 3 no entry).
        if (g_status[13] == 1) { g_status[5] = (uint32_t)unify::InstallMemHook(g_xrpc.buffer_addr); g_status[13] = 0; Fence(); }
        else if (g_status[13] == 2) { unify::UninstallMemHook(); g_status[5] = (uint32_t)unify::g_mh.installed; g_status[13] = 0; Fence(); }
        // memory-triggered control (set via setmem into the status block, since this
        // xbdm does not export DmRegisterCommandProcessor so unify!hook is unavailable):
        //   status[10] = 1 -> install consolefeatures hook; 2 -> unhook. reset after.
        if (g_status[10] == 1) { g_status[9] = (uint32_t)unify::InstallHook(); g_status[10] = 0; Fence(); }
        else if (g_status[10] == 2) { unify::Unhook(); g_status[9] = (uint32_t)unify::g_cf.installed; g_status[10] = 0; Fence(); }
        // refresh the current title id only when a title-gated constant is active, so
        // the loop never stalls on a title query it does not need. short timeout.
        if ((++tick % 50) == 0) {
            bool need = false;
            for (int i = 0; i < 16; ++i)
                if (unify::g_const[i].active && unify::g_const[i].useTid) { need = true; break; }
            if (need) {
                void* fn = unify::Resolve("xam.xex", unify::ORD_XAM_TITLEID);
                if (fn) {
                    bool ok = false;
                    unsigned long long tid = unify::Call(fn, 0, 0, ok, 500);
                    if (ok) unify::g_curtitle = (unsigned int)tid;
                }
            }
        }
        Fence();
        Sleep(20);
    }
    unify::Unhook();        // restore consolefeatures on unload
    unify::UninstallRpc();  // restore the repurposed command entry
    return 0;
}

BOOL APIENTRY DllMain(HANDLE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DWORD id = 0;
        if (ExCreateThread(&g_worker, 0x10000, &id, (PVOID)XapiThreadStartup,
                           (PVOID)Run, 0, 0x10000000u | 0x2 | CREATE_SUSPENDED) != 0)
            return FALSE;
        if (ResumeThread(g_worker) == 0xffffffffu) return FALSE;
    } else if (reason == DLL_PROCESS_DETACH) {
        g_stop = true; Fence();
        if (g_worker) { WaitForSingleObject(g_worker, INFINITE); CloseHandle(g_worker); g_worker = 0; }
    }
    return TRUE;
}

extern "C" BOOL UnifyEntryCpp(HANDLE module, DWORD reason, LPVOID reserved) {
    return DllMain(module, reason, reserved);
}
