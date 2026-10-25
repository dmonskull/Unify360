# installing Unify360.xex

the plugin is a resident dashlaunch system dll. it loads the same way on a devkit and
on a regular freeboot/rgh console - no kernel ordinals, no pc rpc needed. this is the
recommended install for both.

## dashlaunch plugin slot (recommended, devkit + freeboot)

1. copy `build/Unify360.xex` to the console, e.g. `Hdd:\Unify360.xex`.
2. open `launch.ini` (usually `Hdd:\launch.ini`) and add it to a free `[Plugins]`
   slot. existing setups often look like:
   ```
   [Plugins]
   plugin1 = Hdd:\xbdm.xex
   plugin3 = Hdd:\XDRPC.xex
   plugin4 = Hdd:\JRPC2.xex
   plugin5 = Hdd:\Unify360.xex   <- add this (any free slot 1..5)
   ```
   you can keep xbdm, XDRPC and JRPC2 in place - Unify360 checks what is already
   resident on load and only fills the gaps, so nothing conflicts. or you can remove
   JRPC2/XDRPC and let Unify360 provide those surfaces itself.
3. reboot. dashlaunch loads plugins at boot in the correct context.
4. verify from a pc: connect xbdm and send `unify!ping`; it should answer
   `200- unify ok v=0.2 base=0x91E00000 beats=... xrpc=0x... cfhook=...`. other
   subcommands: `unify!status`, `unify!xrpc`, `unify!call`, `unify!hook on|off`.
   (there is also a diagnostic block in the image marked by the ascii magic
   `UNIFY001`; its address drifts as the code grows, so find it by scanning rather
   than hardcoding. `unify!ping` is the simple check.)

## why not load it live at runtime

the pc library has a `load_module` helper that calls the kernel `XexLoadImage`
(ordinal 409) over rpc. that works on a freeboot console through an active jrpc2 /
consolefeatures call. it does NOT work reliably through the native `rpc` command on a
devkit: `XexLoadImage` runs on the rpc system worker thread and blocks there (the
loader waits on a context the worker does not provide), so the call never returns and
nothing loads. this is a calling-context limit, not a wrong ordinal - 409 is
XexLoadImage on both kernels. so for first install, and always on a devkit, use the
dashlaunch slot above. once Unify360 is resident it can hot-load future updates of
itself in the right context.

## base address

Unify360 links at `0x91E00000`, chosen to sit clear of the common residents on these
consoles (xbdm `0x91F00000`, JRPC2 `0x91900000`, XDRPC `0x91060000`, and the user's
DMONCORE at `0x91D00000`). if a future console maps something there, rebuild with a
different `/BASE` in `build.py`.
