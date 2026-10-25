# Unify360 - one way to talk to an xbox 360

the goal is simple to say and hard to do. there are a bunch of ways people talk to a
jtag/rgh or devkit xbox 360 to read memory, write memory, call functions and do
system stuff. each one has its own plugin on the console and its own library on the
pc, and none of them talk to each other. unify360 is one console plugin plus one pc
library (python and .net) that does everything all of them do, picks the right method
on its own, and does not break any of the old ones if they are still loaded.

this doc is the plan and the protocol notes. everything in here was checked against a
live devkit (DMON, 2.0.17489.0) and the decrypted plugin binaries, not from memory.

## the methods we are replacing or absorbing

| method | console side | pc side | how a function call happens |
|---|---|---|---|
| xbdm | built in (xbdm.xex) | raw socket on port 730 | no native call, memory + commands only (devkit xbdm also has the `rpc` command) |
| jrpc / jrpc2 | JRPC.xex / JRPC2.xex (hooks the built in `consolefeatures` command) | text command over xbdm | `consolefeatures ver=2 type=N ...` |
| xdrpc | XDRPC.xex (adds / mirrors the native `rpc` command) | text + binary over xbdm | `rpc ... version=4` then a binary buffer |
| xrpc | RPC.xex / XRPC.xex (a resident poller) | pure setmem/getmem | poke a magic value into a shared buffer, poll for the answer |
| xdevkit | nothing (it is the pc side) | the microsoft `xbdm.dll` COM wrapper (IXboxConsole) | whatever the tool layers on top |

the important realisation: **memory read and write is the one primitive they all share**.
xrpc is only memory. jrpc, xdrpc and the native rpc are three different ways of asking
the console to run a function. xdevkit is just microsoft's COM object around the same
xbdm socket. so unify360 is:

1. a solid xbdm core (memory, files, modules, threads, debug, screenshot) that every
   layer sits on,
2. three function call backends (consolefeatures, native rpc, shared buffer poll),
3. a chooser that detects what the console supports and uses the best one,
4. a drop in compatibility surface so old jrpc / xrpc / xdevkit pc code keeps working.

## protocol notes (verified)

### xbdm line protocol (port 730)
- connect, read the `201- connected` banner. commands are crlf terminated.
- replies start with a 3 digit code and `-`. `200` ok, `202` multiline (lines until a
  lone `.`), `203` binary follows, `204` send binary, `4xx` error.
- memory read text: `getmem addr=0x.. length=N` -> 202, hex lines, `.`. unreadable
  bytes come back as `?`.
- memory read binary (faster): `getmemex addr=0x.. length=0xN` -> 203, then blocks of
  up to 0x400 bytes, each with a 2 byte little endian header (low 15 bits = count,
  top bit = last). short count means the rest is not mapped.
- memory write: `setmem addr=0x.. data=HEX` -> 200.
- files: `getfile` / `sendfile` / `dirlist` / `delete` / `mkdir` / `rename`.
- modules: `modules`, `xbeinfo running`. threads: `threads`, `getcontext`,
  `stop` / `go`. reboot: `magicboot`. screenshot: `screenshot`.
- devkit note: writing into xbdm's own code freezes the console. we never do that.

### jrpc / jrpc2 (consolefeatures)
`consolefeatures` is actually a built in xbdm command. jrpc2 hooks it and extends it.
request:
```
consolefeatures ver=2 type=T [system] [module="NAME" ord=O] as=ARRAYSIZE params="A\ADDR\A\NARGS\<arg><arg>..."
```
- `type` is the return / operation type: 0 void, 1 int, 2 string, 3 float, 4 byte,
  8 uint64, 5/6/7/9 the int/float/byte/uint64 arrays; plus the special ops 9 resolve
  function, 10 cpu key, 11 shutdown, 12 xnotify, 13 kernel version, 14 set leds,
  15 temperature, 16 current title id, 17 console type, 18 constant-memory writer.
- `system` means run on a system thread. `module`+`ord` calls an export instead of a
  raw address (ADDR is then 0).
- argument encoding inside params: int `1\VALUE\`, uint `1\VALUE\`, bool `1/0or1\`,
  float `3\VALUE\`, string/bytes/float-array `7/LEN\HEXBYTES\`.
- reply `200- <hex or text>`. if the reply contains `buf_addr=XXXXXXXX` the result is
  large: keep sending `consolefeatures buf_addr=0xXXXX` until it stops.
- errors come back as `200- error=....` (yes, 200 with an error= body) or as a real
  4xx. max 37 params.

### xdrpc / native rpc (microsoft xdk, version 4)
the devkit xbdm has this built in (`HrRemoteProcedureCall`). XDRPC.xex provides it for
consoles whose xbdm does not.
```
rpc {system|title} version=4 buf_size=N processor=P thread=NAME
```
- reply `204- buf_addr=XXXXXXXX`. the console allocated an N byte scratch buffer at
  that guest address and is waiting for the binary call data.
- the client builds the whole call-data buffer in pc memory (all offsets are relative
  to buf_addr), sends it as binary, gets `203- OK`.
- then it polls: receive 8 bytes big endian. `997` (ERROR_IO_PENDING) = still running,
  keep reading; `0` = done; anything else = error. then receive the full buffer back.
- buffer layout (each slot 8 bytes big endian): `[0]=0 [1]=0 [2]=postcall [3]=flags
  [4]=argcount [5]=floatcount` then the 2 "target" slots (module ptr + ordinal, or 0 +
  address), then the integer arg values, then the float arg values, then the packed
  buffer regions each arg points at. on return `[0]`=hresult, `[1]`=return value,
  `[2]`=postcall return.
- up to 8 integer args. floating point args are pulled out and appended; the call flag
  bit 0 picks integer vs float return.

### xrpc (shared buffer poll)
no command at all, pure memory. the resident poller watches a fixed buffer.
- read a pointer at `0x91C01A2E` -> the guest buffer base.
- write arg values (8 bytes big endian each) at `base+8`, the arg count at `base+4`,
  then write the magic command word at `base+0`: `0x82000000` to call an address,
  `0x82000001` to resolve an export. strings/bytes get staged at `base+1500`, floats
  at `base+2700`, byte blobs at `base+3200` and the slot holds the pointer.
- the poller runs it and writes the 4 byte result at `base+4092`. the client sleeps,
  then reads it back.
- the magic words and the pointer address depend on which RPC.xex is resident, so they
  are configurable in our client.

### xdevkit
microsoft's `xbdm.dll` is a COM wrapper that exposes `IXboxConsole` with
`SetMemory` / `GetMemory` / `SendTextCommand` / `SendBinary` / `ReceiveBinary` and the
connection plumbing. every c# tool (jrpc.cs, xrpc.cs, xdrpcplusplus.cs) is written
against it. our .net dll re-implements that same surface over a plain socket so those
tools run with no microsoft dll and no windows, and our python does the same in python.

## console plugin design (Unify360.xex)

one resident system dll, built with the open source openxechain toolchain (no microsoft
sdk), same proven path as the user's existing trainer xex. it provides, in order of
safety:

1. **a fresh command** `unify` registered with `DmRegisterCommandProcessor`. this is the
   safe path: xbdm keeps a table of up to 16 extra command processors and dispatches any
   line starting `unify!...` to us. no patching of xbdm code, so it cannot fight the
   built in commands. this carries our best-in-class features (fast typed calls, batched
   memory, peek/poke, call-with-floats, freeze, resident mod hooks).
2. **the xrpc shared buffer poller**, so xrpc pc tools work against us with no RPC.xex.
   it only reads and writes guest ram, never xbdm.
3. **consolefeatures compatibility**: only if JRPC2.xex is NOT already resident do we
   install the consolefeatures extension, and we do it the same way jrpc2 does so jrpc
   pc tools work against us. if jrpc2 is loaded we stand down and let it own that
   command (this is the "do not mess with each other" rule).
4. **native rpc**: the devkit xbdm already answers `rpc version=4`; we do not touch it.
   on a console whose xbdm lacks it we can provide it, again only if XDRPC.xex is absent.

coexistence rule: on load we walk the module list. for every method another resident
plugin already provides, we leave that method alone and just use it. we only fill gaps.
that way a user can keep JRPC2/XDRPC/xbdm exactly as they are, or drop them entirely and
run only Unify360, and every old pc tool still connects.

## pc library design

```
x360.Xbox(ip)                      high level facade, auto picks transport
  .xbdm      core line protocol + memory + files + modules + threads
  .jrpc      consolefeatures backend (jrpc/jrpc2 compatible)
  .xdrpc     native rpc version=4 backend
  .xrpc      shared buffer backend
  .call(addr/module+ord, args, ret) -> uses the best available backend
  .read_* / .write_*                 typed memory (always xbdm)
```

compatibility shims expose the old names (`JRPC.Call`, `XRPC.Call`,
`IXboxConsole.GetMemory`, ...) so porting a tool is a one line import change.

## devkit vs freeboot (both must work flawlessly)

the same plugin and library serve both a devkit and a regular freeboot/rgh console.
the differences that matter, found live:
- **which call backend is active.** on the devkit (DMON, kernel 2.0.17489), JRPC2.xex
  is loaded but inactive - it rejected that xbdm build as unsupported, so the stock
  `consolefeatures` answers with feature flags, not the jrpc hook. the live backend
  there is the native `rpc` command. on a freeboot console jrpc2 is usually active.
  the library auto-detects this (backends()), so the same code works on both.
- **runtime module loading.** `XexLoadImage` is ordinal 409 on both kernels. calling
  it through the native `rpc` command on a devkit parks: it runs on the rpc system
  worker thread and the loader blocks there and never returns. through an active jrpc2
  consolefeatures call (freeboot) it works. conclusion: load the plugin with the
  dashlaunch plugin slot (see plugin/INSTALL.md), which is context-correct and
  identical on both; keep the rpc `load_module` helper only as a freeboot convenience.
- everything else (xbdm core, memory, native rpc calls) behaves the same on both.

## status / what is tested
- xbdm core + jrpc consolefeatures: tested live on DMON (devkit, jrpc2 resident).
- xdrpc native rpc: ported from the disassembled XDRPC source, exercised against the
  devkit's native rpc command.
- xrpc shared buffer: implemented; needs a resident poller to exercise (our plugin, or
  a console with RPC.xex).
- console plugin: designed, build is the openxechain path; loading + the hook stand-down
  logic are the live steps still to prove.

prior art credited in the readme. nothing here ships until it is tested live.
