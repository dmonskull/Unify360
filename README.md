# Unify360

one .xex that replaces jrpc, jrpc2, xdrpc, xrpc and the xdevkit xbdm.dll — all of it, on a
single console plugin. load it on freeboot/rgh and any tool anyone ever built for those just
works, no extra plugins, no microsoft dll. our own python + .net libraries sit on top and
always pick the best method for you.

## how it works

unify360 answers on every wire protocol at once, over plain xbdm (port 730):

- **jrpc / jrpc2** — hooks the `consolefeatures` command (stands down if jrpc2 is already loaded)
- **xdrpc** — adds the native `rpc` command (version 4) when the console doesn't have one
- **xrpc** — runs the shared-buffer poller and publishes the classic pointer so stock tools find it
- **plus** memory read/write, function calls (int + float), resolve, cpukey, leds, temps, notifications, constant memory

on load it checks what's already resident and only fills the gaps, so nothing conflicts. it
never ships a method until it's tested live on hardware.

## what's here

- `plugin/` — the console plugin (Unify360.xex), load via dashlaunch `launch.ini`
- `python/` — the `x360` client library; `call()` auto-detects the best backend
- `dotnet/` — the .NET drop-in (netstandard2.0) with jrpc / xrpc / xdevkit compat shims
- `docs/DESIGN.md` — protocol notes + architecture

```python
import x360
xb = x360.Xbox("192.168.1.100").connect()
print(xb.backends())                                   # ['jrpc','xdrpc','xrpc']
xb.call(module="xam.xex", ordinal=463, ret="int")      # or call(address=0x..., args=[...])
data = xb.read(0x80040000, 16)
```

## credits

built by DMONSKULL. stands on a lot of earlier 360 scene work — protocols studied from the
originals and re-implemented clean, verified live, not copied:
- jrpc / jrpc2 by Xx jAmes t xX
- xdrpc (the microsoft xdk rpc) and its community disassembly
- xrpc / RPC.xex and XRPCLib
- py-xbdm by XeCrippy
- the 360 homebrew + reverse engineering community
