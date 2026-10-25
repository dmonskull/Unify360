"""xrpc backend: the shared-buffer poll method.

no console command at all - pure getmem/setmem. a resident poller (RPC.xex / XRPC.xex,
or our own plugin's xrpc service) watches a fixed buffer. we read the buffer pointer,
stage the args, write a magic command word, then poll the result slot. the magic words
and the pointer address are specific to the resident poller, so they are overridable.
"""
import struct
import time

from .transport import XbdmError

# defaults taken from the classic XRPCLib (community RPC.xex)
POINTER_ADDR = 0x91C01A2E      # a pointer here holds the guest buffer base
MAGIC_CALL = 0x82000000        # write at base+0 to call an address
MAGIC_RESOLVE = 0x82000001     # write at base+0 to resolve an export
RESULT_OFFSET = 4092           # the 4 byte result lands here
ARG_OFFSET = 8                 # arg value array starts here
COUNT_OFFSET = 4               # arg count
STRING_OFFSET = 1500
FLOAT_OFFSET = 2700
BYTE_OFFSET = 3200


class Xrpc:
    def __init__(self, memory, pointer_addr=POINTER_ADDR, magic_call=MAGIC_CALL,
                 magic_resolve=MAGIC_RESOLVE, settle=0.05):
        self.mem = memory
        self.pointer_addr = pointer_addr
        self.magic_call = magic_call
        self.magic_resolve = magic_resolve
        self.settle = settle
        self._base = 0

    def _buffer(self):
        if not self._base:
            self._base = self.mem.u32(self.pointer_addr)
            if not self._base:
                raise XbdmError(0, "xrpc poller not present (null buffer pointer)")
        return self._base

    def _stage_args(self, base, args):
        values = []
        spos = base + STRING_OFFSET
        fpos = base + FLOAT_OFFSET
        bpos = base + BYTE_OFFSET
        for a in args:
            if isinstance(a, bool):
                values.append(1 if a else 0)
            elif isinstance(a, int):
                values.append(a & 0xFFFFFFFFFFFFFFFF)
            elif isinstance(a, float):
                self.mem.set_f32(fpos, a)
                values.append(fpos)
                fpos += 8
            elif isinstance(a, str):
                raw = a.encode("ascii", "replace") + b"\x00"
                self.mem.write(spos, raw)
                values.append(spos)
                spos += len(raw)
            elif isinstance(a, (bytes, bytearray)):
                self.mem.write(bpos, bytes(a))
                values.append(bpos)
                bpos += len(a) + 2
            else:
                raise TypeError(f"xrpc cannot stage {type(a).__name__}")
        return values

    def _run(self, base, magic, args):
        values = self._stage_args(base, args)
        blob = b"".join(struct.pack(">q", v) for v in values)
        if blob:
            self.mem.write(base + ARG_OFFSET, blob)
        self.mem.set_u32(base + COUNT_OFFSET, len(values))
        self.mem.set_u32(base, magic)  # fire
        time.sleep(self.settle)
        return self.mem.u32(base + RESULT_OFFSET)

    def call(self, address, args=()):
        base = self._buffer()
        return self._run(base, self.magic_call, [address] + list(args))

    def resolve(self, module, ordinal):
        base = self._buffer()
        return self._run(base, self.magic_resolve, [module, ordinal])

    def available(self):
        if self._base:  # bound directly (e.g. via the plugin status block)
            return True
        try:
            return self.mem.u32(self.pointer_addr) != 0
        except XbdmError:
            return False
