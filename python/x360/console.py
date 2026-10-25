"""the one object you use: x360.Xbox(ip).

it owns the xbdm socket and exposes every layer. the headline feature is call():
it detects which function-call backend the console supports and uses the best one,
so you never have to care whether the box runs jrpc2, xdrpc, xrpc or our own plugin.
"""
import threading

from .transport import Transport, XbdmError
from .memory import Memory
from .xbdm import Xbdm
from .jrpc import Jrpc, INT, UINT64, FLOAT, STRING, VOID
from .xdrpc import Xdrpc
from .xrpc import Xrpc


class Xbox:
    def __init__(self, host, port=730, timeout=10.0, fast_memory=True):
        self.tp = Transport(host, port, timeout)
        self._lock = threading.RLock()
        self.mem = Memory(self.tp, fast=fast_memory)
        self.xbdm = Xbdm(self.tp)
        self.jrpc = Jrpc(self.tp)
        self.xdrpc = Xdrpc(self.tp)
        self.xrpc = Xrpc(self.mem)
        self._backends = None  # cached detection

    # -- connection --------------------------------------------------------
    def connect(self):
        with self._lock:
            self.tp.connect()
        return self

    def close(self):
        with self._lock:
            self.tp.close()

    def __enter__(self):
        return self.connect()

    def __exit__(self, *exc):
        self.close()

    # -- detection ---------------------------------------------------------
    def use_plugin_xrpc(self):
        """point the xrpc backend at Unify360's shared buffer, found via the plugin's
        status block (UNIFY001 magic, status[7] = buffer address). needed because the
        classic xrpc pointer at 0x91C01A2E is unmapped on some consoles. returns the
        buffer address, or 0 if the plugin is not loaded."""
        import struct
        with self._lock:
            base = self.xbdm.module_base("Unify360.xex")
            if not base:
                return 0
            try:
                blob = self.mem.read(base, 0x9000)
            except XbdmError:
                return 0
            mi = blob.find(b"UNIFY001")
            if mi < 0:
                return 0
            buf = struct.unpack_from(">I", blob, mi + 7 * 4)[0]  # status[7]
            if buf:
                self.xrpc._base = buf
            return buf

    def backends(self, refresh=False):
        """which function-call methods this console answers, best first.
        returns a list of names from {'jrpc','xdrpc','xrpc'}."""
        with self._lock:
            if self._backends is not None and not refresh:
                return self._backends
            found = []
            # if our plugin is resident, bind xrpc to its buffer up front
            try:
                self.use_plugin_xrpc()
            except XbdmError:
                pass
            # jrpc2 / our consolefeatures hook: text, robust, no buffer juggling
            try:
                if self.jrpc.available():
                    found.append("jrpc")
            except (XbdmError, ValueError):
                pass
            # native rpc (devkit xbdm / xdrpc)
            try:
                if self.xdrpc.available():
                    found.append("xdrpc")
            except (XbdmError, ValueError):
                pass
            # xrpc shared buffer poller
            try:
                if self.xrpc.available():
                    found.append("xrpc")
            except (XbdmError, ValueError):
                pass
            self._backends = found
            return found

    # -- the unified call --------------------------------------------------
    def call(self, address=0, args=(), ret="int", system=True,
             module=None, ordinal=0, prefer=None):
        """call a function and return its result, using the best backend.

        address or (module, ordinal). ret is 'void'|'int'|'uint64'|'float'|'string'.
        prefer forces a backend name if you want a specific one.
        """
        order = [prefer] if prefer else self.backends()
        if not order:
            raise XbdmError(0, "no function-call backend on this console "
                               "(load Unify360.xex, JRPC2, XDRPC or an xrpc poller)")
        last = None
        for name in order:
            try:
                return self._call_with(name, address, args, ret, system, module, ordinal)
            except (XbdmError, ValueError, NotImplementedError) as e:
                last = e
        raise last

    def _call_with(self, name, address, args, ret, system, module, ordinal):
        with self._lock:
            if name == "jrpc":
                rt = {"void": VOID, "int": INT, "uint64": UINT64,
                      "float": FLOAT, "string": STRING}[ret]
                if ret == "void":
                    self.jrpc.call(address, args, ret=VOID, system=system,
                                   module=module, ordinal=ordinal)
                    return None
                raw = self.jrpc.call(address, args, ret=rt, system=system,
                                     module=module, ordinal=ordinal)
                if ret in ("int", "uint64"):
                    return int(raw, 16)
                if ret == "float":
                    return float(raw)
                return raw
            if name == "xdrpc":
                if module is not None and ret == "float":
                    raise NotImplementedError("xdrpc float return pending")
                v = self.xdrpc.call(address, args, system=system,
                                    module=module, ordinal=ordinal,
                                    float_return=(ret == "float"))
                if ret == "void":
                    return None
                if ret in ("int",):
                    return v & 0xFFFFFFFF
                return v
            if name == "xrpc":
                if module is not None:
                    addr = self.xrpc.resolve(module, ordinal)
                else:
                    addr = address
                v = self.xrpc.call(addr, args)
                return None if ret == "void" else v
            raise ValueError(f"unknown backend {name!r}")

    # -- convenience that works on any backend -----------------------------
    def resolve(self, module, ordinal):
        for name in self.backends():
            try:
                if name == "jrpc":
                    return self.jrpc.resolve(module, ordinal)
                if name == "xrpc":
                    return self.xrpc.resolve(module, ordinal)
            except XbdmError:
                continue
        # xdrpc resolves implicitly inside its call; expose via a tiny call
        raise XbdmError(0, "no backend can resolve exports here")

    def notify(self, text, kind=34):
        """on-screen toast. uses jrpc if present, else a direct xam call."""
        if "jrpc" in self.backends():
            return self.jrpc.notify(text, kind)
        addr = self.resolve("xam.xex", 656)
        self.call(addr, [kind, 0, 2, text, 0], ret="void")

    def title_id(self):
        if "jrpc" in self.backends():
            return self.jrpc.title_id()
        return self.call(module="xam.xex", ordinal=463, ret="int")

    def load_module(self, remote_path):
        """load a resident xex via the kernel XexLoadImage (xboxkrnl ordinal 409).

        works through an active jrpc2 / consolefeatures call (freeboot). NOTE: through
        the native rpc command on a devkit, XexLoadImage parks on the rpc worker thread
        and never returns - on a devkit, install plugins via the dashlaunch slot
        instead (see plugin/INSTALL.md). returns the NTSTATUS (0 = success)."""
        return self.call(module="xboxkrnl.exe", ordinal=409,
                         args=[remote_path, 8, 0, 0], ret="int")

    def unload_module(self, module_name):
        """unload a resident module by name (xam GetModuleHandle + kernel unload)."""
        handle = self.call(module="xam.xex", ordinal=1102, args=[module_name], ret="int")
        if handle:
            self.mem.set_u16(handle + 0x40, 1)
            self.call(module="xboxkrnl.exe", ordinal=417, args=[handle], ret="int")
        return handle

    # pass-through typed memory for the common one-liners
    def read(self, address, length):
        with self._lock:
            return self.mem.read(address, length)

    def write(self, address, data):
        with self._lock:
            return self.mem.write(address, data)
