// SPDX-License-Identifier: GPL-3.0-or-later
// xrpc shared-buffer poller for the unify360 plugin.
//
// classic xrpc tools (RPC.xex / XRPCLib) do not send a console command. they read a
// pointer at a fixed address to find a shared buffer, write the call args and a magic
// command word into it, then poll a result slot. we reproduce the resident poller so
// those tools work against unify360: allocate a buffer, publish its address at the
// classic pointer (best effort, only if that page is writable), and watch the magic
// word. everything here is plain guest-ram access - we never touch xbdm.
#ifndef UNIFY_XRPC_H
#define UNIFY_XRPC_H
#include "Kernel.h"
#include "Call.h"

namespace unify {

// classic XRPCLib layout (big endian, matches the pc side byte for byte)
const unsigned int XRPC_POINTER   = 0x91C01A2Eu;  // a pointer here -> buffer base
const unsigned int XRPC_MAGIC_CALL    = 0x82000000u;
const unsigned int XRPC_MAGIC_RESOLVE = 0x82000001u;
const unsigned int XRPC_RESULT    = 4092;
const unsigned int XRPC_ARGS      = 8;
const unsigned int XRPC_COUNT     = 4;
const unsigned int XRPC_BUFSIZE   = 0x2000;

struct XrpcState {
    volatile unsigned char* buffer;  // guest-ram shared buffer
    unsigned int buffer_addr;        // its address (also published for our own tools)
    bool pointer_published;          // did the classic 0x91C01A2E pointer get set
    volatile bool stop;
};

inline unsigned int BeU32(volatile const unsigned char* p) {
    return (unsigned int)p[0] << 24 | (unsigned int)p[1] << 16 |
           (unsigned int)p[2] << 8 | (unsigned int)p[3];
}
inline void PutBeU32(volatile unsigned char* p, unsigned int v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}
inline unsigned long long BeU64(volatile const unsigned char* p) {
    return (unsigned long long)BeU32(p) << 32 | BeU32(p + 4);
}

// one poll pass: if a magic word is present, run the request and clear it.
inline void XrpcPollOnce(XrpcState& s) {
    volatile unsigned char* b = s.buffer;
    unsigned int magic = BeU32(b);
    if (magic != XRPC_MAGIC_CALL && magic != XRPC_MAGIC_RESOLVE) return;
    unsigned int count = BeU32(b + XRPC_COUNT);
    if (count > 9) count = 9;
    unsigned long long slots[9];
    for (unsigned int i = 0; i < count; ++i) slots[i] = BeU64(b + XRPC_ARGS + i * 8);
    unsigned int result = 0;
    bool ok = false;
    if (magic == XRPC_MAGIC_RESOLVE && count >= 2) {
        // slots[0] = pointer to module name string (already staged in the buffer),
        // slots[1] = ordinal
        const char* module = (const char*)(size_t)(unsigned int)slots[0];
        void* fn = Resolve(module, (unsigned int)slots[1]);
        result = (unsigned int)(size_t)fn;
        ok = true;
    } else if (magic == XRPC_MAGIC_CALL && count >= 1) {
        // slots[0] = function address, slots[1..] = up to 8 args
        unsigned int args[8];
        int argc = (int)count - 1;
        if (argc > 8) argc = 8;
        for (int i = 0; i < argc; ++i) args[i] = (unsigned int)slots[1 + i];
        unsigned long long r = CallAddr((unsigned int)slots[0], args, argc, ok);
        result = (unsigned int)r;
    }
    (void)ok;
    PutBeU32(b + XRPC_RESULT, result);
    __sync_synchronize();
    PutBeU32(b, 0);  // clear the magic: request consumed
}

// try to make the classic pointer at 0x91C01A2E hold our buffer address, so
// unmodified xrpc tools find us. only writes if that page is valid+writable.
inline bool PublishPointer(unsigned int buffer_addr) {
    void* p = (void*)(size_t)XRPC_POINTER;
    if (!MmIsAddressValid(p) || !MmIsAddressValid((void*)(size_t)(XRPC_POINTER + 3)))
        return false;
    // big endian store of the buffer address
    volatile unsigned char* q = (volatile unsigned char*)p;
    q[0] = (unsigned char)(buffer_addr >> 24); q[1] = (unsigned char)(buffer_addr >> 16);
    q[2] = (unsigned char)(buffer_addr >> 8);  q[3] = (unsigned char)buffer_addr;
    __sync_synchronize();
    return true;
}

inline bool XrpcInit(XrpcState& s) {
    s.stop = false;
    s.pointer_published = false;
    void* buf = ExAllocatePool(XRPC_BUFSIZE);
    if (!buf) { s.buffer = 0; s.buffer_addr = 0; return false; }
    memset(buf, 0, XRPC_BUFSIZE);
    s.buffer = (volatile unsigned char*)buf;
    s.buffer_addr = (unsigned int)(size_t)buf;
    s.pointer_published = PublishPointer(s.buffer_addr);
    return true;
}

}  // namespace unify
#endif
