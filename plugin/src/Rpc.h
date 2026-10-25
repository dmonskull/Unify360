// SPDX-License-Identifier: GPL-3.0-or-later
// rpc (XDRPC / microsoft xdk version=4) command provider for unify360.
//
// this xbdm (0x4F59F205) has no native `rpc` command and does not export
// DmRegisterCommandProcessor, so the way to add `rpc` is the same data-safe trick
// XDRPC.xex uses: the builtin command dispatch is a LINEAR scan, so repurpose an
// unused builtin entry by repointing its name->"rpc" and handler->ours. that is a
// pure data patch of the command table (never xbdm .text). we repurpose "help".
//
// the version=4 binary-transfer protocol is modelled on the native 21256 xbdm:
//   HrRemoteProcedureCall       -> UnifyRpc      (parse, alloc, start receive)
//   HrRemoteProcedureCallReceive-> RpcReceive    (stream the request in, run async)
//   HrRemoteProcedureCallResponse-> RpcResponse  (stream 997 keep-alives, then result)
//   DmCallProcedure             -> RpcWorkerThunk (the call, on its own thread)
//
// CRITICAL: the call must NOT run on xbdm's command thread. the continuation
// callbacks are invoked from xbdm's DoSendReceive loop; if a callback blocks (a slow
// or hung target, or Sleep in that context) the whole command server wedges and the
// console hangs. so the call runs on a worker thread while RpcResponse streams the
// 997 (ERROR_IO_PENDING) keep-alive marker until the worker reports completion - the
// exact shape the native rpc uses.
#ifndef UNIFY_RPC_H
#define UNIFY_RPC_H
#include "Kernel.h"
#include "Call.h"
#include "ConsoleFeatures.h"  // FindXbdmBase, FindCommandEntry, After, ReadDec, XBDM_NOERR

namespace unify {

static const char RPC_CMD_NAME[] = "rpc";

struct RpcState {
    unsigned char* entry;      // repurposed table entry {name(+0), flags(+4), handler(+8)}
    unsigned int orig_name;
    unsigned int orig_flags;
    unsigned int orig_handler;
    volatile int installed;
};
static RpcState g_rpc = { 0, 0, 0, 0, 0 };

// the xbdm command-continuation context (DM_CMDCONT), as DWORDs (confirmed from the
// 21256 DoSendReceive per-connection block):
//   [0] callback   [1] length (recv count in / send len out)   [2] buffer ptr
//   [3] capacity   [4] user data (our RpcCall*)                 [5] remaining
// in receive mode xbdm recv()s min(capacity,remaining) into buffer ptr, sets length,
// then calls the callback. in send mode the callback sets buffer ptr/length, returns
// a positive code, and xbdm sends length bytes; 0x82DA0104 ends the response.
#define RPC_MORE  0x02DA0000u   // 200: more data this phase
#define RPC_RESP  0x02DA0003u   // 203: switch to binary response
#define RPC_RECV  0x02DA0004u   // 204: send binary data to console
#define RPC_END   0x82DA0104u   // end of response
#define RPC_PENDING 0x3e5ull    // ERROR_IO_PENDING (997), the keep-alive marker

struct RpcCall {
    unsigned char* data;                 // the version=4 buffer (buf_size bytes)
    unsigned int size;                   // buf_size
    volatile int done;                   // worker sets 1 once the call has completed
    unsigned int thread;                 // worker thread handle (0 if none/inline)
    volatile unsigned long long marker;  // streamed 8-byte word: 997 pending, 0 = done
    unsigned int phase;                  // response phase: 0=status, 1=buffer, 2=end
};

int UnifyRpc(const char* cmd, char* resp, unsigned int cchResp, void* cont);
int RpcReceive(void* cont);
int RpcResponse(void* cont);

// --- version=4 binary buffer helpers (big endian, matches the pc client) ---
inline unsigned int RBe32(const unsigned char* p) {
    return (unsigned int)p[0] << 24 | (unsigned int)p[1] << 16 |
           (unsigned int)p[2] << 8 | (unsigned int)p[3];
}
inline void RPutBe64(unsigned char* p, unsigned long long v) {
    for (int i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (56 - i * 8));
}

// execute the version=4 call buffer: resolve/call the target, write hresult+retval.
// runs on the worker thread (see RpcWorkerThunk), never on xbdm's command thread. the
// target is invoked DIRECTLY via the call gate: the worker thread IS the isolation (a
// hung target parks only the worker, never xbdm), exactly as the native DmCallProcedure
// does - no nested helper thread, so completion is reported promptly.
inline void ExecuteRpcCall(unsigned char* b, unsigned int size) {
    if (size < 0x48) return;
    unsigned int argc = RBe32(b + 4 * 8 + 4);       // slot 4 = arg count
    if (argc > 8) argc = 8;
    unsigned int moduleptr = RBe32(b + 6 * 8 + 4);  // slot 6 = module-name ptr (0 => addr)
    unsigned int target = RBe32(b + 7 * 8 + 4);     // slot 7 = ordinal or address
    void* fn = moduleptr ? Resolve((const char*)(size_t)moduleptr, target)
                         : (void*)(size_t)target;
    unsigned int args[8];
    for (unsigned int i = 0; i < 8; ++i)
        args[i] = (i < argc) ? RBe32(b + (8 + i) * 8 + 4) : 0u;
    unsigned long long r = fn ? UnifyCallGate(fn, args) : 0ull;
    RPutBe64(b + 0, fn ? 0ull : 0x82DA0000ull);  // slot 0 = hresult
    RPutBe64(b + 8, r);                          // slot 1 = return value
}

// the async worker: run the call, then flag completion. xbdm's command thread is
// never waited on here - it is off streaming 997 markers in RpcResponse meanwhile.
inline DWORD WINAPI RpcWorkerThunk(LPVOID p) {
    RpcCall* call = (RpcCall*)p;
    ExecuteRpcCall(call->data, call->size);
    __sync_synchronize();
    call->done = 1;
    return 0;
}

// request bytes have been received into the buffer. advance; when the whole buffer is
// in, kick off the worker and switch to streaming the response. returns RPC_MORE
// (keep receiving) or RPC_RESP (203, binary response follows).
inline int RpcReceive(void* cont) {
    unsigned int* c = (unsigned int*)cont;
    RpcCall* call = (RpcCall*)(size_t)c[4];
    unsigned int recv = c[1];
    if (recv) { c[2] += recv; c[5] -= recv; }
    if (c[5] != 0) return (int)RPC_MORE;          // more to receive

    call->done = 0;
    call->marker = RPC_PENDING;
    __sync_synchronize();
    // run the call on a worker thread so the callbacks never block xbdm's loop.
    HANDLE h = 0; DWORD id = 0;
    if (ExCreateThread(&h, 0x8000, &id, (PVOID)XapiThreadStartup, (PVOID)RpcWorkerThunk,
                       call, 0x10000000u | CREATE_SUSPENDED) != 0) {
        // could not spawn: fall back to inline (the worker body is self-contained).
        call->thread = 0;
        ExecuteRpcCall(call->data, call->size);
        __sync_synchronize();
        call->done = 1;
    } else {
        call->thread = (unsigned int)(size_t)h;
        ResumeThread(h);
    }
    c[0] = (unsigned int)(size_t)&RpcResponse;
    return (int)RPC_RESP;                          // 203 binary response follows
}

// stream the result. the generic send loop is RETURN-CODE driven: it sends c[1] bytes
// from c[2] and re-invokes while we return a positive code, and stops when we return
// RPC_END (0x82DA0104). we track our own phase (NOT c[5], which this xbdm zeroes at
// the start of the send phase). while the worker runs, emit the 8-byte 997 keep-alive
// marker; once it completes, emit a single 8-byte completion status (0), then the
// whole buffer, then end.
inline int RpcResponse(void* cont) {
    unsigned int* c = (unsigned int*)cont;
    RpcCall* call = (RpcCall*)(size_t)c[4];

    if (call->phase >= 2) {                        // buffer already sent -> finish
        if (call->thread) {
            HANDLE h = (HANDLE)(size_t)call->thread;
            WaitForSingleObject(h, INFINITE);
            CloseHandle(h);
            call->thread = 0;
        }
        ExFreePool(call->data);
        ExFreePool(call);
        c[1] = 0;
        return (int)RPC_END;
    }
    if (call->phase == 1) {                        // completion sent: send the buffer
        c[2] = (unsigned int)(size_t)call->data;
        c[1] = call->size;
        call->phase = 2;
        return (int)RPC_MORE;
    }
    // phase 0: keep-alive until the worker completes, then the completion status.
    if (!call->done) {                             // still running: 997 keep-alive
        call->marker = RPC_PENDING;
        c[2] = (unsigned int)(size_t)&call->marker;
        c[1] = 8;
        return (int)RPC_MORE;
    }
    call->marker = 0;                              // done: 8-byte completion status (0)
    c[2] = (unsigned int)(size_t)&call->marker;
    c[1] = 8;
    call->phase = 1;
    return (int)RPC_MORE;
}

// repurpose "help" -> "rpc" (data patch). 1 installed, 2 not found, 3 no base.
inline int InstallRpc() {
    unsigned char* base = FindXbdmBase();
    if (!base) return 3;
    unsigned char* entry = FindCommandEntry(base, 0x20000, "help");
    if (!entry) return 2;
    g_rpc.entry = entry;
    g_rpc.orig_name = *(unsigned int*)(entry + 0);
    g_rpc.orig_flags = *(unsigned int*)(entry + 4);
    g_rpc.orig_handler = *(unsigned int*)(entry + 8);
    // patch handler + flags first, then the name last so the entry only matches "rpc"
    // once it is fully wired.
    *(unsigned int*)(entry + 8) = (unsigned int)(size_t)&UnifyRpc;
    *(unsigned int*)(entry + 4) = 0;
    __sync_synchronize();
    *(unsigned int*)(entry + 0) = (unsigned int)(size_t)RPC_CMD_NAME;
    __sync_synchronize();
    g_rpc.installed = 1;
    return 1;
}

inline void UninstallRpc() {
    if (g_rpc.installed && g_rpc.entry) {
        *(unsigned int*)(g_rpc.entry + 0) = g_rpc.orig_name;
        *(unsigned int*)(g_rpc.entry + 4) = g_rpc.orig_flags;
        *(unsigned int*)(g_rpc.entry + 8) = g_rpc.orig_handler;
        __sync_synchronize();
        g_rpc.installed = 0;
    }
}

inline int UnifyRpc(const char* cmd, char* resp, unsigned int cchResp, void* cont) {
    if (cchResp < 64) { resp[0] = 0; return XBDM_NOERR; }
    char* d = resp;
    const char* v = After(cmd, "version=");
    if (!v) {
        const char* s = "error=Version is not specified, expecting major version 4";
        while (*s) *d++ = *s++; *d = 0; return XBDM_NOERR;
    }
    if (ReadDec(v) != 4) {
        const char* s = "error=Version mismatch, expected major version 4";
        while (*s) *d++ = *s++; *d = 0; return XBDM_NOERR;
    }
    const char* bs = After(cmd, "buf_size=");
    unsigned int buf_size = bs ? ReadDec(bs) : 0;
    if (!buf_size || buf_size > 0x100000) {
        const char* s = "error=invalid buf_size";
        while (*s) *d++ = *s++; *d = 0; return XBDM_NOERR;
    }
    RpcCall* call = (RpcCall*)ExAllocatePool(sizeof(RpcCall));
    if (!call) { const char* s = "error=out of memory"; while (*s) *d++ = *s++; *d = 0; return XBDM_NOERR; }
    call->data = (unsigned char*)ExAllocatePool(buf_size);
    if (!call->data) { ExFreePool(call); const char* s = "error=out of memory"; while (*s) *d++ = *s++; *d = 0; return XBDM_NOERR; }
    memset(call->data, 0, buf_size);
    call->size = buf_size;
    call->done = 0;
    call->thread = 0;
    call->marker = RPC_PENDING;
    call->phase = 0;
    // set up the continuation to receive buf_size binary bytes into call->data
    unsigned int* c = (unsigned int*)cont;
    c[4] = (unsigned int)(size_t)call;          // user data
    c[2] = (unsigned int)(size_t)call->data;    // receive destination
    c[3] = buf_size;                            // capacity (recv up to this per read)
    c[5] = buf_size;                            // remaining
    c[0] = (unsigned int)(size_t)&RpcReceive;   // receive callback
    // the client computes its internal pointers relative to this buf_addr
    d = PutStr(d, "buf_addr=");
    d = PutHex(d, (unsigned int)(size_t)call->data, 8);
    return (int)RPC_RECV;  // 204 send binary data
}

}  // namespace unify
#endif
