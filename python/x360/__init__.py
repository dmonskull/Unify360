"""x360 - one library for every xbox 360 connection method.

    import x360
    xb = x360.Xbox("192.168.1.100").connect()
    print(xb.backends())              # ['jrpc', 'xdrpc']
    print(hex(xb.title_id()))
    xb.notify("hello from unify360")
    val = xb.call(module="xboxkrnl.exe", ordinal=1, ret="uint64")  # XamGetSystemVersion

it speaks raw xbdm, jrpc/jrpc2 consolefeatures, the native xdrpc rpc command and the
xrpc shared-buffer poll, and picks the best one on its own. prior art is credited in
the readme; the protocol was verified against a live console.
"""
from .console import Xbox
from .transport import Transport, XbdmError, Response
from .memory import Memory
from .xbdm import Xbdm
from .jrpc import Jrpc
from .xdrpc import Xdrpc
from .xrpc import Xrpc
from . import discovery

__version__ = "0.1.0"
__all__ = [
    "Xbox", "Transport", "XbdmError", "Response",
    "Memory", "Xbdm", "Jrpc", "Xdrpc", "Xrpc", "discovery",
]
