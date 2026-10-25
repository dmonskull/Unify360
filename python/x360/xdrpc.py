"""xdrpc backend: microsoft's native `rpc` command, version 4.

the devkit xbdm answers this natively (HrRemoteProcedureCall); XDRPC.xex provides it
elsewhere. this is a faithful port of the disassembled XDRPC marshaler. v1 supports
integer / pointer args (up to 8) in both address mode and module+ordinal mode, which
covers almost every real call. float args and by-ref buffers are a documented follow
up. the wire framing is verified against the devkit's native rpc.
"""
import re
import struct

from .transport import XbdmError

SLOT = 8  # every buffer slot is one big endian qword
ERROR_IO_PENDING = 997
HEADER_SLOTS = 8  # 6 fixed (0,0,postcall,flags,argcount,floatcount) + 2 target slots


class Xdrpc:
    def __init__(self, tp, max_pending=300):
        self.tp = tp
        # each pending word is ~one console poll (~100ms); 300 ~= 30s before giving up
        self.max_pending = max_pending

    def _start(self, buf_size, system=True, processor=5, thread=""):
        mode = "system" if system else "title"
        line = (
            f"rpc {mode} version=4 buf_size={buf_size} "
            f"processor={processor} thread={thread}"
        )
        self.tp.send_raw(line.encode("latin-1") + b"\r\n")
        resp = self.tp.recv_line().rstrip(b"\r\n").decode("latin-1")
        if "error=" in resp:
            raise XbdmError(200, resp.split("error=", 1)[1])
        m = re.match(r"(\d+)-\s*buf_addr=([0-9a-fA-F]+)", resp)
        if not m:
            raise XbdmError(0, f"rpc start: {resp!r}")
        if int(m.group(1)) != 204:
            raise XbdmError(int(m.group(1)), resp)
        return int(m.group(2), 16)

    def call(self, address=0, args=(), system=True, module=None, ordinal=0,
             processor=5, thread="", float_return=False):
        if len(args) > 8:
            raise ValueError("native rpc supports at most 8 integer args")
        argc = len(args)

        def align8(n):
            return (n + SLOT - 1) & ~(SLOT - 1)

        # layout: HEADER_SLOTS + argc arg-value slots, then the module name (module
        # mode), then a blob region holding any str/bytes args. each str/bytes arg is
        # passed as a pointer into that blob - the same mechanism module mode already
        # uses for the module name, so the console just sees an integer pointer value.
        value_region = (HEADER_SLOTS + argc) * SLOT
        module_bytes = (module.encode("ascii") + b"\x00") if module is not None else b""
        module_region = align8(len(module_bytes))
        blobs = []  # (arg_index, raw_bytes)
        blob_region = 0
        for i, a in enumerate(args):
            if isinstance(a, str):
                raw = a.encode("ascii", "replace") + b"\x00"
            elif isinstance(a, (bytes, bytearray)):
                raw = bytes(a)
            else:
                continue
            blobs.append((i, raw))
            blob_region += align8(len(raw))
        buf_size = value_region + module_region + blob_region

        buf_addr = self._start(buf_size, system=system, processor=processor, thread=thread)

        data = bytearray(buf_size)
        def put(slot, value):
            struct.pack_into(">Q", data, slot * SLOT, value & 0xFFFFFFFFFFFFFFFF)

        put(0, 0)                      # [0] hresult (out)
        put(1, 0)                      # [1] return value (out)
        put(2, 0)                      # [2] post method call
        put(3, 1 if float_return else 0)  # [3] flags: bit0 = float return
        put(4, argc)                   # [4] integer arg count
        put(5, 0)                      # [5] float arg count
        if module is not None:
            put(6, buf_addr + value_region)  # [6] module name pointer
            put(7, ordinal)                  # [7] ordinal
            data[value_region:value_region + len(module_bytes)] = module_bytes
        else:
            put(6, 0)                  # [6] no module -> call address directly
            put(7, address)            # [7] function address
        # scalar int args
        for i, a in enumerate(args):
            if isinstance(a, (str, bytes, bytearray)):
                continue
            put(HEADER_SLOTS + i, int(a))
        # str/bytes args: write bytes into the blob, pass the pointer
        pos = value_region + module_region
        for i, raw in blobs:
            put(HEADER_SLOTS + i, buf_addr + pos)
            data[pos:pos + len(raw)] = raw
            pos += align8(len(raw))

        self.tp.send_raw(bytes(data))
        ok = self.tp.recv_line().rstrip(b"\r\n").decode("latin-1")
        if not ok.startswith("203"):
            raise XbdmError(0, f"rpc send: {ok!r}")

        # framing (verified live): the console streams 8-byte completion-status words,
        # 997 (ERROR_IO_PENDING) while the call runs. the first non-997 word is the
        # final status (0 = ok) and is NOT part of the buffer. the full buf_size
        # result buffer follows it. the return value is at slot 1, hresult at slot 0.
        # cap the pending stream so a function that never returns (a bad target that
        # parks its rpc worker) raises instead of hanging the client forever.
        pending = 0
        status = self.tp.recv_exact(SLOT)
        while int.from_bytes(status, "big") == ERROR_IO_PENDING:
            pending += 1
            if pending > self.max_pending:
                raise XbdmError(0, "rpc call did not complete (target never returned)")
            status = self.tp.recv_exact(SLOT)
        code = int.from_bytes(status, "big")
        if code != 0:
            raise XbdmError(0, f"rpc completion status 0x{code:X}")
        result = self.tp.recv_exact(buf_size)

        hresult = struct.unpack_from(">Q", result, 0)[0]
        retval = struct.unpack_from(">Q", result, SLOT)[0]
        if hresult & 0xFFFFFFFF:
            raise XbdmError(0, f"rpc call hresult 0x{hresult & 0xFFFFFFFF:08X}")
        return retval & 0xFFFFFFFFFFFFFFFF

    def call_int(self, address, args=(), **kw):
        return self.call(address, args, **kw) & 0xFFFFFFFF

    def available(self):
        """true if the console answers the native rpc command (devkit xbdm / XDRPC)."""
        # send an rpc line with no version; a present command replies about the
        # version, an absent one replies 407 unknown command.
        self.tp.send_raw(b"rpc\r\n")
        resp = self.tp.recv_line().rstrip(b"\r\n").decode("latin-1")
        return "error=" in resp or resp.startswith("20")
