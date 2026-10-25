// SPDX-License-Identifier: GPL-3.0-or-later
// consolefeatures compatibility hook - makes unmodified jrpc/jrpc2 pc tools work.
//
// `consolefeatures` is a BUILTIN xbdm command (not a registerable one), so the only
// way to extend it is to replace its handler pointer in xbdm's builtin command table.
// that is exactly what JRPC2 does. we do it with heavy validation and a clear stand
// down rule:
//   - if JRPC2.xex is resident, we do NOT hook (it owns the command; no conflict).
//   - otherwise we locate the consolefeatures table entry by finding the exact
//     "consolefeatures" string in xbdm's image and the table slot that points at it,
//     and we only patch if the slot's handler points into xbdm's code. if anything is
//     uncertain we stand down and leave xbdm untouched.
// our handler implements the jrpc ver=2 protocol (function calls + the common ops) and
// forwards everything else - including the stock, non ver=2 consolefeatures - to the
// original handler, so nothing that worked before breaks.
//
// this targets freeboot/rgh consoles (where jrpc tools and the retail xam/kernel
// ordinals live). on a devkit the hook stands down; devkit tooling uses native rpc.
#ifndef UNIFY_CONSOLEFEATURES_H
#define UNIFY_CONSOLEFEATURES_H
#include "Kernel.h"
#include "Call.h"

namespace unify {

typedef int (*CmdProc)(const char* cmd, char* resp, unsigned int cchResp, void* cont);

#define XBDM_NOERR 0x02DA0000

struct CfState {
    void* xbdm_base;
    unsigned int xbdm_size;
    void** handler_slot;   // the table slot we patched (0 if not hooked)
    CmdProc original;      // saved original handler
    volatile int installed;
};

// --- tiny parsing over the command string (no CRT) ---
inline bool Contains(const char* s, const char* sub) {
    for (; *s; ++s) {
        const char* a = s; const char* b = sub;
        while (*b && *a == *b) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}
inline const char* After(const char* s, const char* key) {
    for (; *s; ++s) {
        const char* a = s; const char* b = key;
        while (*b && *a == *b) { ++a; ++b; }
        if (!*b) return a;
    }
    return 0;
}
inline unsigned int ReadDec(const char*& p) {
    bool neg = false;
    if (*p == '-') { neg = true; ++p; }
    unsigned int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned)(*p - '0'); ++p; }
    return neg ? (unsigned int)(-(int)v) : v;
}
inline unsigned int ReadHex(const char*& p) {
    unsigned int v = 0;
    for (;;) {
        char c = *p;
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else break;
        v = v * 16 + d; ++p;
    }
    return v;
}
inline char* PutHex(char* d, unsigned int v, int digits) {
    const char* H = "0123456789ABCDEF";
    for (int i = (digits - 1) * 4; i >= 0; i -= 4) *d++ = H[(v >> i) & 0xF];
    *d = 0; return d;
}
inline char* PutHex64(char* d, unsigned long long v) {
    d = PutHex(d, (unsigned int)(v >> 32), 8);
    d = PutHex(d, (unsigned int)v, 8);
    return d;
}
inline char* PutStr(char* d, const char* s) { while (*s) *d++ = *s++; *d = 0; return d; }
inline unsigned int HexNib(char c) {
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (unsigned)(c - 'A' + 10);
    return 0;
}

// scratch for staging string/byte-array args during a call (valid for the duration of
// the synchronous call, which blocks this handler). single xbdm server thread -> safe.
static unsigned char g_cf_scratch[0x1000];

// --- canonical retail ordinals / config (confirm on freeboot; calls are isolated so
// a wrong value just times out and returns benign) ---
const unsigned int ORD_XAM_TITLEID   = 463;  // XamGetCurrentTitleId (verified)
const unsigned int ORD_XAM_XNOTIFYUI = 656;  // XNotifyUI (used by the trainer)
const unsigned int ORD_KRNL_VERSION  = 344;  // XboxKrnlVersion data export (0x158; verified
                                             // vs JRPC2 imports + live: {0002 0000 Build Qfe};
                                             // ordinal 2 is a FUNCTION here, not the version)
const unsigned int ORD_HAL_SMC       = 41;   // HalSendSMCMessage (retail; verify)
static unsigned int g_cpukey_addr = 0;       // set via `unify!cpukey addr=` on freeboot

// --- float text <-> double (no CRT) ---
inline double ParseDouble(const char*& p) {
    bool neg = false;
    if (*p == '-') { neg = true; ++p; } else if (*p == '+') ++p;
    double v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; }
    if (*p == '.') {
        ++p; double f = 0.1;
        while (*p >= '0' && *p <= '9') { v += (*p - '0') * f; f *= 0.1; ++p; }
    }
    if (*p == 'e' || *p == 'E') {
        ++p; bool en = false;
        if (*p == '-') { en = true; ++p; } else if (*p == '+') ++p;
        int e = 0; while (*p >= '0' && *p <= '9') { e = e * 10 + (*p - '0'); ++p; }
        double m = 1; for (int i = 0; i < e; ++i) m *= 10;
        if (en) v /= m; else v *= m;
    }
    return neg ? -v : v;
}
inline char* PutDec(char* d, unsigned int v) {
    char t[12]; int n = 0;
    if (!v) { *d++ = '0'; *d = 0; return d; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *d++ = t[--n];
    *d = 0; return d;
}
inline char* PutDouble(char* d, double v) {
    if (v < 0) { *d++ = '-'; v = -v; }
    unsigned long long ip = (unsigned long long)v;
    double fp = v - (double)ip;
    char t[24]; int n = 0;
    if (!ip) t[n++] = '0';
    while (ip) { t[n++] = (char)('0' + (int)(ip % 10)); ip /= 10; }
    while (n) *d++ = t[--n];
    *d++ = '.';
    for (int i = 0; i < 6; ++i) {
        fp *= 10; int dig = (int)fp;
        if (dig < 0) dig = 0; if (dig > 9) dig = 9;
        *d++ = (char)('0' + dig); fp -= dig;
    }
    *d = 0; return d;
}

// --- constant-memory writers (jrpc type 18): write a value repeatedly while a title
// runs. serviced from the plugin heartbeat; no external ordinals needed. ---
struct ConstEntry {
    unsigned int addr, value, ifValue, titleId;
    bool useIf, useTid, active;
};
static ConstEntry g_const[16];
static volatile unsigned int g_curtitle = 0;

inline void AddConst(unsigned int addr, unsigned int value, bool useIf,
                     unsigned int ifValue, bool useTid, unsigned int titleId) {
    for (int i = 0; i < 16; ++i) {
        if (g_const[i].active && g_const[i].addr == addr) {  // update in place
            g_const[i].value = value; g_const[i].useIf = useIf; g_const[i].ifValue = ifValue;
            g_const[i].useTid = useTid; g_const[i].titleId = titleId; return;
        }
    }
    for (int i = 0; i < 16; ++i) {
        if (!g_const[i].active) {
            g_const[i].addr = addr; g_const[i].value = value; g_const[i].useIf = useIf;
            g_const[i].ifValue = ifValue; g_const[i].useTid = useTid;
            g_const[i].titleId = titleId; g_const[i].active = true; return;
        }
    }
}

// called from the heartbeat loop (plugin.cpp). writes each active constant when its
// conditions hold. guards every access with MmIsAddressValid.
inline void ServiceConstMem() {
    for (int i = 0; i < 16; ++i) {
        if (!g_const[i].active) continue;
        if (g_const[i].useTid && g_curtitle != g_const[i].titleId) continue;
        void* a = (void*)(size_t)g_const[i].addr;
        if (!MmIsAddressValid(a)) continue;
        if (g_const[i].useIf && *(volatile unsigned int*)a != g_const[i].ifValue) continue;
        *(volatile unsigned int*)a = g_const[i].value;
    }
}

// parse the params="..." body and run the requested call / op. result text -> resp.
inline int HandleVer2(const char* cmd, char* resp, unsigned int cchResp) {
    if (cchResp < 48) { resp[0] = 0; return XBDM_NOERR; }
    unsigned int type = 0;
    const char* t = After(cmd, "type=");
    if (t) type = ReadDec(t);
    char module[64]; module[0] = 0;
    unsigned int ordinal = 0;
    const char* m = After(cmd, "module=\"");
    if (m) {
        int i = 0;
        while (*m && *m != '"' && i < 63) module[i++] = *m++;
        module[i] = 0;
        const char* o = After(cmd, "ord=");
        if (o) ordinal = ReadDec(o);
    }
    // params="A\ADDR\A\N\ <args>"
    const char* p = After(cmd, "params=\"");
    unsigned int address = 0, nargs = 0;
    unsigned int gpr[8]; double fpr[8];
    unsigned int rawdec[8];  // decoded scalar values (for op args like titleid/leds)
    for (int i = 0; i < 8; ++i) { gpr[i] = 0; fpr[i] = 0.0; rawdec[i] = 0; }
    int fprIdx = 0;
    unsigned int scratch_used = 0;
    if (p) {
        if (*p == 'A') ++p; if (*p == '\\') ++p;
        address = ReadHex(p);
        if (*p == '\\') ++p;
        if (*p == 'A') ++p; if (*p == '\\') ++p;
        nargs = ReadDec(p);
        if (*p == '\\') ++p;
        for (unsigned int a = 0; a < nargs && a < 8; ++a) {
            unsigned int atype = ReadDec(p);
            if (*p == '/') {  // 7/LEN\HEX\ -> stage bytes, pass pointer
                ++p;
                unsigned int len = ReadDec(p);
                if (*p == '\\') ++p;
                unsigned int base = scratch_used;
                for (unsigned int b = 0; b < len && scratch_used < sizeof(g_cf_scratch) - 1; ++b) {
                    if (!p[0] || !p[1]) break;
                    g_cf_scratch[scratch_used++] = (unsigned char)((HexNib(p[0]) << 4) | HexNib(p[1]));
                    p += 2;
                }
                g_cf_scratch[scratch_used++] = 0;
                if (*p == '\\') ++p;
                gpr[a] = (unsigned int)(size_t)&g_cf_scratch[base];
                rawdec[a] = gpr[a];
            } else {
                if (*p == '\\') ++p;
                if (atype == 3) {  // float arg: into the next FPR; its gpr slot unused
                    double df = ParseDouble(p);
                    if (fprIdx < 8) fpr[fprIdx++] = df;
                    gpr[a] = 0;
                } else {
                    gpr[a] = ReadDec(p);
                    rawdec[a] = gpr[a];
                }
                if (*p == '\\') ++p;
            }
        }
    }

    bool ok = false;
    unsigned long long r = 0;
    double fret = 0.0;

    // --- special ops (type doubles as an opcode) ---
    if (type == 9) {  // resolve export -> address
        void* fn = Resolve((const char*)(size_t)gpr[0], rawdec[1]);
        PutHex(resp, (unsigned int)(size_t)fn, 8);
        return XBDM_NOERR;
    }
    if (type == 10) {  // cpu key: 16 bytes from the configured source (0 -> zeros)
        char* d = resp;
        for (int i = 0; i < 16; ++i) {
            unsigned char b = 0;
            if (g_cpukey_addr && MmIsAddressValid((void*)(size_t)(g_cpukey_addr + i)))
                b = *(volatile unsigned char*)(size_t)(g_cpukey_addr + i);
            d = PutHex(d, b, 2);
        }
        return XBDM_NOERR;
    }
    if (type == 11) { resp[0] = 0; return XBDM_NOERR; }  // shutdown: benign no-op
    if (type == 12) {  // xnotify: [text ptr, notify type] -> XNotifyUI (utf-16)
        unsigned char* a0 = (unsigned char*)(size_t)gpr[0];
        unsigned short* w = (unsigned short*)&g_cf_scratch[0x800];
        int i = 0;
        while (a0 && a0[i] && i < 200) { w[i] = a0[i]; ++i; }
        w[i] = 0;
        unsigned int na[5] = { rawdec[1], 0, 2, (unsigned int)(size_t)w, 0 };
        CallExport("xam.xex", ORD_XAM_XNOTIFYUI, na, 5, ok);
        resp[0] = 0;
        return XBDM_NOERR;
    }
    if (type == 13) {  // kernel version: read XboxKrnlVersion build, return decimal
        void* vp = Resolve("xboxkrnl.exe", ORD_KRNL_VERSION);
        unsigned int build = 0;
        if (vp && MmIsAddressValid((void*)((char*)vp + 5)))
            build = *(volatile unsigned short*)((char*)vp + 4);  // WORD Build at +4
        PutDec(resp, build);
        return XBDM_NOERR;
    }
    if (type == 14) {  // set leds: 4 states (OFF/RED=8/GREEN=128/ORANGE=136)
        unsigned char* msg = &g_cf_scratch[0xC00];
        for (int i = 0; i < 16; ++i) msg[i] = 0;
        unsigned char green = 0, red = 0;
        for (int i = 0; i < 4; ++i) {
            if (rawdec[i] & 128) green |= (unsigned char)(1 << (3 - i));
            if (rawdec[i] & 8)   red   |= (unsigned char)(1 << (3 - i));
        }
        msg[0] = 0x99; msg[1] = 0x01; msg[2] = green; msg[3] = red;
        unsigned int na[1] = { (unsigned int)(size_t)msg };
        CallExport("xboxkrnl.exe", ORD_HAL_SMC, na, 1, ok);
        resp[0] = 0;
        return XBDM_NOERR;
    }
    if (type == 15) {  // temperature query (best effort; verify format on freeboot)
        unsigned char* msg = &g_cf_scratch[0xC00];
        for (int i = 0; i < 16; ++i) msg[i] = 0;
        msg[0] = 0x07;  // SMC query-temperature
        unsigned int na[1] = { (unsigned int)(size_t)msg };
        CallExport("xboxkrnl.exe", ORD_HAL_SMC, na, 1, ok);
        unsigned int kind = rawdec[0];
        unsigned int temp = ok ? (unsigned int)msg[1 + kind * 2] : 0;
        PutHex(resp, temp, 8);
        return XBDM_NOERR;
    }
    if (type == 16) {  // current title id via xam!463 (same ordinal jrpc2 uses). on this
        // reviewer kit the dashboard reports 0x00040006; that is the real value, not a bug
        // (retail boxes show 0xFFFE07D1). isolated call, same as every other, for safety.
        r = CallExport("xam.xex", ORD_XAM_TITLEID, 0, 0, ok);
        PutHex(resp, (unsigned int)r, 8);
        return XBDM_NOERR;
    }
    if (type == 17) {  // console type: devkit flag at 0x8E038610 (02 dev / 03 retail)
        const char* s = "Retail";
        if (MmIsAddressValid((void*)0x8E038610)) {
            unsigned char v = *(volatile unsigned char*)0x8E038610;
            s = (v == 0x02) ? "DevKit" : "Retail";
        }
        char* d = resp; while (*s) *d++ = *s++; *d = 0;
        return XBDM_NOERR;
    }
    if (type == 18) {  // constant memory: ADDR + [value, useIf, ifValue, useTid, titleId]
        AddConst(address, rawdec[0], rawdec[1] != 0, rawdec[2], rawdec[3] != 0, rawdec[4]);
        resp[0] = 0;
        return XBDM_NOERR;
    }

    // --- function-call return types: 0 void,1 int,2 string,3 float,4 byte,8 uint64 ---
    void* fn = module[0] ? Resolve(module, ordinal) : (void*)(size_t)address;
    r = CallF(fn, gpr, fpr, ok, &fret);

    if (type == 0) { resp[0] = 0; }
    else if (type == 3) { PutDouble(resp, fret); }
    else if (type == 8) { PutHex64(resp, r); }
    else if (type == 2) {  // string return: r is a guest char*
        char* d = resp;
        const unsigned char* s = (const unsigned char*)(size_t)(unsigned int)r;
        unsigned int n = 0;
        while (s && MmIsAddressValid((void*)s) && *s && n < cchResp - 2) { *d++ = (char)*s++; ++n; }
        *d = 0;
    } else { PutHex(resp, (unsigned int)r, 8); }
    return XBDM_NOERR;
}

static CfState g_cf = { 0, 0, 0, 0, 0 };

// our installed handler
inline int UnifyConsoleFeatures(const char* cmd, char* resp, unsigned int cchResp, void* cont) {
    if (Contains(cmd, "ver=2"))
        return HandleVer2(cmd, resp, cchResp);
    // anything else (incl. stock consolefeatures) -> original behaviour
    if (g_cf.original)
        return g_cf.original(cmd, resp, cchResp, cont);
    resp[0] = 0;
    return XBDM_NOERR;
}

// --- hook install ---
inline bool PageOk(const void* a) { return MmIsAddressValid((void*)(size_t)a); }

inline unsigned int ImageSize(unsigned char* base) {
    if (!PageOk(base) || base[0] != 'M' || base[1] != 'Z') return 0;
    unsigned int peoff = *(unsigned int*)(base + 0x3C);
    if (peoff > 0x400) return 0;
    unsigned char* pe = base + peoff;
    if (!PageOk(pe) || pe[0] != 'P' || pe[1] != 'E') return 0;
    return *(unsigned int*)(pe + 24 + 56);  // optional header SizeOfImage
}

// find the "consolefeatures\0" string inside xbdm, then the table slot pointing at it
// whose +8 handler lands in xbdm code. returns the handler slot, or 0.
inline void** FindHandlerSlot(unsigned char* base, unsigned int size) {
    const char* target = "consolefeatures";
    unsigned int tlen = 15;
    unsigned char* str_at = 0;
    for (unsigned int i = 0; i + tlen + 1 < size; i += 1) {
        unsigned char* at = base + i;
        if ((i & 0xFFF) == 0 && !PageOk(at)) { i += 0xFFF; continue; }
        if (at[0] != 'c') continue;
        bool eq = true;
        for (unsigned int k = 0; k < tlen; ++k) if (at[k] != (unsigned char)target[k]) { eq = false; break; }
        if (eq && at[tlen] == 0) { str_at = at; break; }
    }
    if (!str_at) return 0;
    unsigned int str_va = (unsigned int)(size_t)str_at;
    unsigned int base_va = (unsigned int)(size_t)base;
    unsigned int end_va = base_va + size;
    // scan 4-byte aligned for a pointer to the string; validate +8 is a code pointer.
    for (unsigned int i = 0; i + 16 < size; i += 4) {
        unsigned char* at = base + i;
        if ((i & 0xFFF) == 0 && !PageOk(at)) { i += 0xFFC; continue; }
        if (*(unsigned int*)at != str_va) continue;
        // verified entry layout on xbdm 0x4F59F205 (and 21256): {name(+0), flags(+4),
        // handler(+8)}, 12 bytes. require flags to be a small value and the handler to
        // point into xbdm code, so we only patch a genuine command-table entry.
        unsigned int flags = *(unsigned int*)(at + 4);
        unsigned int handler = *(unsigned int*)(at + 8);
        if (flags <= 0xFFFF && handler >= base_va && handler < end_va)
            return (void**)(at + 8);
    }
    return 0;
}

inline bool Jrpc2Resident() {
    HMODULE h = 0;
    return XexGetModuleHandle("JRPC2.xex", &h) == 0 && h != 0;
}

// find xbdm's mapped PE base (NOT the HMODULE): resolve any xbdm export, scan down to
// the 'MZ' header. returns 0 on failure.
inline unsigned char* FindXbdmBase() {
    void* anyfn = Resolve("xbdm.xex", 1);
    if (!anyfn) return 0;
    unsigned int a = (unsigned int)(size_t)anyfn & ~0xFFFu;
    for (int i = 0; i < 0x40; ++i) {
        unsigned char* p = (unsigned char*)(size_t)(a - (unsigned int)(i * 0x1000));
        if (PageOk(p) && p[0] == 'M' && p[1] == 'Z') return p;
    }
    return 0;
}

// find a builtin command table entry by exact name. the table stores 12-byte entries
// {name_ptr(+0), flags(+4), handler(+8)}; the dispatch is a linear scan (verified), so
// any entry can be located by matching its name string. returns the entry address, or 0.
inline unsigned char* FindCommandEntry(unsigned char* base, unsigned int size,
                                       const char* name) {
    unsigned int nlen = 0;
    while (name[nlen]) ++nlen;
    unsigned char* str_at = 0;
    for (unsigned int i = 0; i + nlen + 1 < size; ++i) {
        unsigned char* at = base + i;
        if ((i & 0xFFF) == 0 && !PageOk(at)) { i += 0xFFF; continue; }
        if (at[0] != (unsigned char)name[0]) continue;
        bool eq = true;
        for (unsigned int k = 0; k < nlen; ++k)
            if (at[k] != (unsigned char)name[k]) { eq = false; break; }
        if (eq && at[nlen] == 0) { str_at = at; break; }
    }
    if (!str_at) return 0;
    unsigned int str_va = (unsigned int)(size_t)str_at;
    unsigned int base_va = (unsigned int)(size_t)base, end_va = base_va + size;
    for (unsigned int i = 0; i + 12 < size; i += 4) {
        unsigned char* at = base + i;
        if ((i & 0xFFF) == 0 && !PageOk(at)) { i += 0xFFC; continue; }
        if (*(unsigned int*)at != str_va) continue;
        unsigned int flags = *(unsigned int*)(at + 4);
        unsigned int handler = *(unsigned int*)(at + 8);
        if (flags <= 0xFFFF && handler >= base_va && handler < end_va)
            return at;
    }
    return 0;
}

// returns: 0 stood down (jrpc2), 1 installed, 2 entry not found, 3 bad image
inline int InstallHook() {
    if (Jrpc2Resident()) return 0;  // stand down: jrpc2 owns consolefeatures
    // the HMODULE from XexGetModuleHandle is NOT the mapped PE base on this kernel, so
    // find the real base: resolve any xbdm export, then scan down to its 'MZ' header.
    void* anyfn = Resolve("xbdm.xex", 1);
    if (!anyfn) return 3;
    unsigned int a = (unsigned int)(size_t)anyfn & ~0xFFFu;
    unsigned char* base = 0;
    for (int i = 0; i < 0x40; ++i) {
        unsigned char* p = (unsigned char*)(size_t)(a - (unsigned int)(i * 0x1000));
        if (PageOk(p) && p[0] == 'M' && p[1] == 'Z') { base = p; break; }
    }
    if (!base) return 3;
    // NOTE: do not parse the PE SizeOfImage - PE headers are little-endian and this is
    // a big-endian cpu, so those fields read byte-swapped. the command table pointers
    // are written big-endian by the loader, so scanning for them is correct. use a
    // fixed range that covers xbdm (~0x1F000); unmapped pages are skipped by PageOk.
    unsigned int size = 0x20000;
    void** slot = FindHandlerSlot(base, size);
    if (!slot) return 2;
    g_cf.xbdm_base = base; g_cf.xbdm_size = size;
    g_cf.original = (CmdProc)*slot;
    g_cf.handler_slot = slot;
    *slot = (void*)UnifyConsoleFeatures;   // the one 4-byte table write (data, not code)
    __sync_synchronize();
    g_cf.installed = 1;
    return 1;
}

inline void Unhook() {
    if (g_cf.installed && g_cf.handler_slot && g_cf.original) {
        *g_cf.handler_slot = (void*)g_cf.original;
        __sync_synchronize();
        g_cf.installed = 0;
    }
}

}  // namespace unify
#endif
