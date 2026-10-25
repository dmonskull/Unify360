"""jrpc / jrpc2 backend: the extended `consolefeatures` command.

compatible with consoles running JRPC2.xex (or our own plugin's consolefeatures
hook). this speaks exactly the wire format jrpc2 expects, so anything a jrpc pc tool
could do, this does."""
import struct

from .transport import XbdmError

# consolefeatures type / operation codes
VOID = 0
INT = 1
STRING = 2
FLOAT = 3
BYTE = 4
INT_ARRAY = 5
FLOAT_ARRAY = 6
BYTE_ARRAY = 7
UINT64 = 8
UINT64_ARRAY = 9

# special operations (type field doubles as an opcode for these)
OP_RESOLVE = 9
OP_CPUKEY = 10
OP_SHUTDOWN = 11
OP_XNOTIFY = 12
OP_KERNELVERSION = 13
OP_SETLEDS = 14
OP_TEMPERATURE = 15
OP_TITLEID = 16
OP_CONSOLETYPE = 17
OP_CONSTMEM = 18

VERSION = 2


class Jrpc:
    def __init__(self, tp):
        self.tp = tp

    # -- argument encoding -------------------------------------------------
    @staticmethod
    def _encode(arg):
        if isinstance(arg, bool):
            return f"{INT}\\{1 if arg else 0}\\"
        if isinstance(arg, int):
            return f"{INT}\\{arg & 0xFFFFFFFF}\\"
        if isinstance(arg, float):
            return f"{FLOAT}\\{arg}\\"
        if isinstance(arg, str):
            raw = arg.encode("ascii", "replace")
            return f"{BYTE_ARRAY}/{len(raw)}\\{raw.hex().upper()}\\"
        if isinstance(arg, (bytes, bytearray)):
            return f"{BYTE_ARRAY}/{len(arg)}\\{bytes(arg).hex().upper()}\\"
        raise TypeError(f"cannot encode jrpc arg of type {type(arg).__name__}")

    def _send(self, body):
        resp = self.tp.command(body)
        msg = resp.message
        # large results stream back through buf_addr continuations
        while "buf_addr=" in msg:
            buf = msg.split("buf_addr=")[1][:8]
            resp = self.tp.command(f"consolefeatures buf_addr=0x{buf}")
            msg = resp.message
        if "error=" in msg:
            raise XbdmError(resp.code, msg.split("error=", 1)[1])
        return msg.strip()

    # -- the generic call --------------------------------------------------
    def call(self, address=0, args=(), ret=INT, system=True, module=None, ordinal=0,
             array_size=0):
        parts = [f"consolefeatures ver={VERSION}", f"type={ret}"]
        if system:
            parts.append("system")
        if module is not None:
            parts.append(f'module="{module}" ord={ordinal}')
        parts.append(f"as={array_size}")
        params = [f"A\\{address:X}\\A\\{len(args)}\\"]
        for a in args:
            params.append(self._encode(a))
        body = " ".join(parts) + ' params="' + "".join(params) + '"'
        return self._send(body)

    # typed convenience wrappers
    def call_void(self, address, args=(), **kw):
        self.call(address, args, ret=VOID, **kw)

    def call_int(self, address, args=(), **kw):
        return int(self.call(address, args, ret=INT, **kw), 16)

    def call_uint64(self, address, args=(), **kw):
        return int(self.call(address, args, ret=UINT64, **kw), 16)

    def call_float(self, address, args=(), **kw):
        return float(self.call(address, args, ret=FLOAT, **kw))

    def call_string(self, address, args=(), **kw):
        return self.call(address, args, ret=STRING, **kw)

    # -- special operations ------------------------------------------------
    def resolve(self, module, ordinal):
        """resolve an export to its runtime address."""
        hexmod = module.encode("ascii").hex().upper()
        body = (
            f'consolefeatures ver={VERSION} type={OP_RESOLVE} params="A\\0\\A\\2\\'
            f"{BYTE_ARRAY}/{len(module)}\\{hexmod}\\{INT}\\{ordinal}\\\""
        )
        return int(self._send(body), 16)

    def cpukey(self):
        body = f'consolefeatures ver={VERSION} type={OP_CPUKEY} params="A\\0\\A\\0\\"'
        return self._send(body)

    def kernel_version(self):
        body = f'consolefeatures ver={VERSION} type={OP_KERNELVERSION} params="A\\0\\A\\0\\"'
        return self._send(body)

    def title_id(self):
        body = f'consolefeatures ver={VERSION} type={OP_TITLEID} params="A\\0\\A\\0\\"'
        return int(self._send(body), 16)

    def console_type(self):
        body = f'consolefeatures ver={VERSION} type={OP_CONSOLETYPE} params="A\\0\\A\\0\\"'
        return self._send(body)

    def temperature(self, kind=0):
        """0 cpu, 1 gpu, 2 edram, 3 motherboard."""
        body = (
            f'consolefeatures ver={VERSION} type={OP_TEMPERATURE} params="A\\0\\A\\1\\'
            f"{INT}\\{kind}\\\""
        )
        return int(self._send(body), 16)

    def set_leds(self, tl, tr, bl, br):
        body = (
            f'consolefeatures ver={VERSION} type={OP_SETLEDS} params="A\\0\\A\\4\\'
            f"{INT}\\{tl}\\{INT}\\{tr}\\{INT}\\{bl}\\{INT}\\{br}\\\""
        )
        self._send(body)

    def notify(self, text, kind=34):
        raw = text.encode("ascii", "replace")
        body = (
            f'consolefeatures ver={VERSION} type={OP_XNOTIFY} params="A\\0\\A\\2\\'
            f"{BYTE_ARRAY}/{len(raw)}\\{raw.hex().upper()}\\{INT}\\{kind}\\\""
        )
        self._send(body)

    def shutdown(self):
        body = f'consolefeatures ver={VERSION} type={OP_SHUTDOWN} params="A\\0\\A\\0\\"'
        try:
            self._send(body)
        except XbdmError:
            pass

    def constant_memory(self, address, value, if_value=None, title_id=None):
        """jrpc2's persistent poke: keep writing value at address while the given
        title runs, optionally only when the current word equals if_value."""
        use_if = 1 if if_value is not None else 0
        use_tid = 1 if title_id is not None else 0
        body = (
            f'consolefeatures ver={VERSION} type={OP_CONSTMEM} params="A\\{address:X}\\A\\5\\'
            f"{INT}\\{value & 0xFFFFFFFF}\\{INT}\\{use_if}\\{INT}\\{(if_value or 0) & 0xFFFFFFFF}\\"
            f"{INT}\\{use_tid}\\{INT}\\{(title_id or 0) & 0xFFFFFFFF}\\\""
        )
        self._send(body)

    # -- probe -------------------------------------------------------------
    def available(self):
        """true if the extended consolefeatures hook answers (jrpc2 or our plugin).
        the stock xbdm consolefeatures replies with feature flags like 'DEBUGGING',
        which is not a hex title id - that means the jrpc hook is not active."""
        try:
            self.title_id()
            return True
        except (XbdmError, ValueError):
            return False
