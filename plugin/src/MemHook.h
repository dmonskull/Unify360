// SPDX-License-Identifier: GPL-3.0-or-later
// getmem / getmemex read hook for the unify360 plugin.
//
// stock XRPCLib tools find the shared buffer by reading a u32 pointer at the fixed
// address 0x91C01A2E (inside the classic RPC.xex image). on freeboot RPC.xex is not
// loaded, so that page is unmapped and the read fails - the tool never finds our
// buffer. we cannot map that VA (it lives in a loader-owned module slot; the kernel
// refuses NtAllocateVirtualMemory there and it is not a physical-window address).
//
// instead we hook xbdm's getmem/getmemex command entries (the same data-safe command
// table repoint used for consolefeatures/rpc). when a read targets exactly 0x91C01A2E
// we rewrite the command's addr= to point at a real mapped cell holding our buffer
// address, then call the ORIGINAL handler - it streams the bytes with its own logic,
// so the tool reads our buffer pointer. every other address is forwarded unmodified,
// so normal memory reads are byte-for-byte unchanged.
#ifndef UNIFY_MEMHOOK_H
#define UNIFY_MEMHOOK_H
#include "Kernel.h"
#include "ConsoleFeatures.h"  // CmdProc, FindXbdmBase, FindCommandEntry, After, ReadHex, PageOk

namespace unify {

static const unsigned int XRPC_PTR_ADDR = 0x91C01A2Eu;  // classic XRPCLib pointer VA

struct MemHookState {
    unsigned char* getmem_entry;      // command-table entry {name(+0),flags(+4),handler(+8)}
    unsigned char* getmemex_entry;
    CmdProc getmem_orig;
    CmdProc getmemex_orig;
    volatile unsigned int ptr_cell[8];  // real mapped cell; [0] = buffer base (big endian)
    volatile int installed;
};
static MemHookState g_mh = { 0, 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 }, 0 };

// parse addr=0x.... (hex, optional 0x) from a getmem/getmemex command; 0 if absent.
inline unsigned int ParseGetAddr(const char* cmd) {
    const char* p = After(cmd, "addr=");
    if (!p) return 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    return ReadHex(p);
}

// copy cmd into out, replacing the addr= value with 0x<newaddr>. false if it won't fit.
inline bool RewriteGetAddr(const char* cmd, unsigned int newaddr, char* out, unsigned int outsz) {
    const char* ap = After(cmd, "addr=");
    if (!ap) return false;
    const char* v = ap;
    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) v += 2;
    while ((*v >= '0' && *v <= '9') || (*v >= 'a' && *v <= 'f') || (*v >= 'A' && *v <= 'F')) ++v;
    unsigned int pre = (unsigned int)(ap - cmd);   // chars up to and including "addr="
    unsigned int taillen = 0; while (v[taillen]) ++taillen;
    if (pre + 2 + 8 + taillen + 1 > outsz) return false;
    char* d = out;
    for (unsigned int i = 0; i < pre; ++i) *d++ = cmd[i];
    *d++ = '0'; *d++ = 'x';
    for (int i = 7; i >= 0; --i) {
        unsigned int nib = (newaddr >> (i * 4)) & 0xF;
        *d++ = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    for (unsigned int i = 0; i <= taillen; ++i) *d++ = v[i];  // copy the tail incl. NUL
    return true;
}

inline int UnifyGetMem(const char* cmd, char* resp, unsigned int cchResp, void* cont) {
    if (ParseGetAddr(cmd) == XRPC_PTR_ADDR) {
        char nc[256];
        if (RewriteGetAddr(cmd, (unsigned int)(size_t)&g_mh.ptr_cell[0], nc, sizeof(nc)))
            return g_mh.getmem_orig(nc, resp, cchResp, cont);
    }
    return g_mh.getmem_orig(cmd, resp, cchResp, cont);
}
inline int UnifyGetMemEx(const char* cmd, char* resp, unsigned int cchResp, void* cont) {
    if (ParseGetAddr(cmd) == XRPC_PTR_ADDR) {
        char nc[256];
        if (RewriteGetAddr(cmd, (unsigned int)(size_t)&g_mh.ptr_cell[0], nc, sizeof(nc)))
            return g_mh.getmemex_orig(nc, resp, cchResp, cont);
    }
    return g_mh.getmemex_orig(cmd, resp, cchResp, cont);
}

// install the read hook. buffer_addr = our xrpc shared-buffer base (published to tools).
// returns: 1 installed, 2 no xbdm base, 3 neither entry found.
inline int InstallMemHook(unsigned int buffer_addr) {
    unsigned char* base = FindXbdmBase();
    if (!base) return 2;
    g_mh.ptr_cell[0] = buffer_addr;             // big endian native store (ppc)
    for (int i = 1; i < 8; ++i) g_mh.ptr_cell[i] = 0;
    __sync_synchronize();
    unsigned char* ge = FindCommandEntry(base, 0x20000, "getmem");
    unsigned char* gx = FindCommandEntry(base, 0x20000, "getmemex");
    if (!ge && !gx) return 3;
    if (ge) {
        g_mh.getmem_entry = ge;
        g_mh.getmem_orig = (CmdProc)*(unsigned int*)(ge + 8);
        *(unsigned int*)(ge + 8) = (unsigned int)(size_t)&UnifyGetMem;
    }
    if (gx) {
        g_mh.getmemex_entry = gx;
        g_mh.getmemex_orig = (CmdProc)*(unsigned int*)(gx + 8);
        *(unsigned int*)(gx + 8) = (unsigned int)(size_t)&UnifyGetMemEx;
    }
    __sync_synchronize();
    g_mh.installed = 1;
    return 1;
}

inline void UninstallMemHook() {
    if (!g_mh.installed) return;
    if (g_mh.getmem_entry && g_mh.getmem_orig)
        *(unsigned int*)(g_mh.getmem_entry + 8) = (unsigned int)(size_t)g_mh.getmem_orig;
    if (g_mh.getmemex_entry && g_mh.getmemex_orig)
        *(unsigned int*)(g_mh.getmemex_entry + 8) = (unsigned int)(size_t)g_mh.getmemex_orig;
    __sync_synchronize();
    g_mh.installed = 0;
}

}  // namespace unify
#endif
