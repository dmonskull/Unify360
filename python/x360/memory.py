"""memory read/write over xbdm. the universal primitive.

big endian on the console (ppc). these helpers always present native-endian values
to python and handle the byte order for you.
"""
import struct

from .transport import XbdmError


def read_block(tp, address, length):
    """read with getmem (text hex). works everywhere, slower. raises if any byte
    in the range is unreadable (the console marks those with '?')."""
    if length < 0:
        raise ValueError("length < 0")
    if length == 0:
        return b""
    resp = tp.command(f"getmem addr=0x{address:X} length={length}")
    if resp.code != 202:
        raise XbdmError(resp.code, resp.message)
    parts = []
    got = 0
    want = length * 2
    unreadable = False
    # always drain to the lone '.' terminator, even on error, so the stream stays
    # in sync for the next command.
    while True:
        s = tp.recv_line().rstrip(b"\r\n")
        if s == b".":
            break
        part = s.decode("latin-1")
        if part and set(part) == {"?"}:
            unreadable = True
            continue
        got += len(part)
        parts.append(part)
    if unreadable:
        raise XbdmError(404, f"memory at 0x{address:08X} not readable")
    if got != want:
        raise XbdmError(0, "getmem length mismatch")
    return bytes.fromhex("".join(parts))


def read_block_fast(tp, address, length):
    """read with getmemex (binary). much faster on a 360. raises XbdmError(404)
    with the first unreadable address if the range is not fully mapped."""
    if length < 0:
        raise ValueError("length < 0")
    if length == 0:
        return b""
    tp.send_raw(f"getmemex addr=0x{address:X} length=0x{length:X}\r\n".encode())
    resp = tp.parse(tp.recv_line())
    if resp.code != 203:
        raise XbdmError(resp.code or 0, f"getmemex: unexpected {resp.code}")
    out = bytearray()
    while True:
        flags = int.from_bytes(tp.recv_exact(2), "little")
        count = flags & 0x7FFF
        if count:
            out += tp.recv_exact(count)
        if flags & 0x8000 or len(out) >= length:
            break
    if len(out) < length:
        raise XbdmError(404, f"0x{address + len(out):08X} not mapped")
    return bytes(out[:length])


# xbdm rejects an over-long command line (error 446), so setmem data is chunked.
# 128 bytes -> 256 hex + the "setmem addr=0x........ data=" prefix stays well under
# xbdm's line limit.
SETMEM_CHUNK = 128


def write_block(tp, address, data):
    data = bytes(data)
    for off in range(0, len(data), SETMEM_CHUNK):
        chunk = data[off:off + SETMEM_CHUNK]
        resp = tp.command(f"setmem addr=0x{address + off:X} data={chunk.hex()}")
        if resp.code != 200:
            raise XbdmError(resp.code, resp.message)
    return len(data)


class Memory:
    """typed accessors bound to a transport. endianness handled here."""

    def __init__(self, tp, fast=True):
        self.tp = tp
        self.fast = fast

    def read(self, address, length):
        reader = read_block_fast if self.fast else read_block
        try:
            return reader(self.tp, address, length)
        except XbdmError:
            if self.fast:
                # some ranges reject getmemex; fall back to text once
                return read_block(self.tp, address, length)
            raise

    def write(self, address, data):
        return write_block(self.tp, address, bytes(data))

    # unsigned / signed integers, big endian
    def u8(self, a):
        return self.read(a, 1)[0]

    def u16(self, a):
        return struct.unpack(">H", self.read(a, 2))[0]

    def u32(self, a):
        return struct.unpack(">I", self.read(a, 4))[0]

    def u64(self, a):
        return struct.unpack(">Q", self.read(a, 8))[0]

    def i8(self, a):
        return struct.unpack(">b", self.read(a, 1))[0]

    def i16(self, a):
        return struct.unpack(">h", self.read(a, 2))[0]

    def i32(self, a):
        return struct.unpack(">i", self.read(a, 4))[0]

    def i64(self, a):
        return struct.unpack(">q", self.read(a, 8))[0]

    def f32(self, a):
        return struct.unpack(">f", self.read(a, 4))[0]

    def f64(self, a):
        return struct.unpack(">d", self.read(a, 8))[0]

    def set_u8(self, a, v):
        self.write(a, struct.pack(">B", v & 0xFF))

    def set_u16(self, a, v):
        self.write(a, struct.pack(">H", v & 0xFFFF))

    def set_u32(self, a, v):
        self.write(a, struct.pack(">I", v & 0xFFFFFFFF))

    def set_u64(self, a, v):
        self.write(a, struct.pack(">Q", v & 0xFFFFFFFFFFFFFFFF))

    def set_i8(self, a, v):
        self.write(a, struct.pack(">b", v))

    def set_i16(self, a, v):
        self.write(a, struct.pack(">h", v))

    def set_i32(self, a, v):
        self.write(a, struct.pack(">i", v))

    def set_i64(self, a, v):
        self.write(a, struct.pack(">q", v))

    def set_f32(self, a, v):
        self.write(a, struct.pack(">f", v))

    def set_f64(self, a, v):
        self.write(a, struct.pack(">d", v))

    def cstring(self, a, maxlen=256):
        raw = self.read(a, maxlen)
        return raw.split(b"\x00", 1)[0].decode("utf-8", "replace")

    def wstring(self, a, maxchars=256):
        raw = self.read(a, maxchars * 2)
        end = raw.find(b"\x00\x00")
        if end >= 0 and end % 2 == 0:
            raw = raw[:end]
        return raw.decode("utf-16-be", "ignore").rstrip("\x00")

    def set_cstring(self, a, s):
        self.write(a, s.encode("utf-8") + b"\x00")

    def set_wstring(self, a, s):
        self.write(a, s.encode("utf-16-be") + b"\x00\x00")
