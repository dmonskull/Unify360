"""build the unify360 console plugin with the openxechain toolchain (no microsoft sdk).

same proven path as the user's trainer xex: clang++ -> lld-link (system dll) ->
materialize trailing bss -> synthxex (sysdll). verifies the resulting ppc xbox 360
PE/XEX headers before publishing. base 0x91E00000 (clear of DMONCORE at 0x91D00000
and xbdm at 0x91F00000).
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BASE = 0x91E00000


def toolchain(explicit=None):
    base = Path(explicit or os.environ.get("OPENXECHAIN_ROOT")
                or ROOT.parent.parent / "OpenXeChain").expanduser().resolve()
    sysroot = base if (base / "bin/clang++").is_file() else base / "sysroot"
    for name in ("clang++", "synthxex", "llvm-readobj", "llvm-objdump"):
        if not (sysroot / "bin" / name).is_file():
            raise ValueError(f"openxechain missing {name}; set OPENXECHAIN_ROOT")
    if not (sysroot / "bin/clang.cfg").is_file():
        raise ValueError("openxechain clang.cfg missing (xbox target not configured)")
    return sysroot


def materialize_bss(raw):
    """back zero-init section tails with bytes before synthxex maps the image.
    the pinned converter sizes from the last raw section and drops trailing bss;
    this physically backs each section's virtual tail. rvas/code/data unchanged."""
    pe = bytearray(raw)
    if len(pe) < 64 or pe[:2] != b"MZ":
        raise ValueError("missing PE")
    nt = struct.unpack_from("<I", pe, 0x3C)[0]
    if pe[nt:nt + 4] != b"PE\0\0":
        raise ValueError("invalid PE")
    count = struct.unpack_from("<H", pe, nt + 6)[0]
    opt_size = struct.unpack_from("<H", pe, nt + 20)[0]
    opt = nt + 24
    table = opt + opt_size
    if opt_size != 224 or count == 0 or table + count * 40 > len(pe):
        raise ValueError("invalid section table")
    alignment = struct.unpack_from("<I", pe, opt + 36)[0]
    headers = struct.unpack_from("<I", pe, opt + 60)[0]
    image_size = struct.unpack_from("<I", pe, opt + 56)[0]
    if alignment != 512 or headers < table + count * 40 or headers > len(pe):
        raise ValueError("unsupported PE layout")
    if struct.unpack_from("<I", pe, opt + 128)[0] or struct.unpack_from("<I", pe, opt + 144)[0]:
        raise ValueError("cannot rewrite certificate/debug file pointers")
    out = bytearray(pe[:headers])
    extra = 0
    tails = []
    for i in range(count):
        at = table + i * 40
        virtual, rva, size, offset = struct.unpack_from("<IIII", pe, at + 8)
        if offset > len(pe) or size > len(pe) - offset or rva > image_size or max(virtual, size) > image_size - rva:
            raise ValueError("section outside file/image")
        new_size = (max(virtual, size) + alignment - 1) & ~(alignment - 1)
        new_offset = (len(out) + alignment - 1) & ~(alignment - 1)
        out.extend(bytes(new_offset - len(out)))
        struct.pack_into("<II", out, at + 16, new_size, new_offset if new_size else 0)
        out.extend(pe[offset:offset + size])
        out.extend(bytes(new_size - size))
        if new_size > size:
            tails.append({"section": pe[at:at + 8].rstrip(b"\0").decode("ascii", "replace"),
                          "zero_bytes_added": new_size - size})
            extra += new_size - size
            ch = struct.unpack_from("<I", out, at + 36)[0]
            struct.pack_into("<I", out, at + 36, (ch & ~0x80) | 0x40)
    struct.pack_into("<I", out, opt + 8, struct.unpack_from("<I", pe, opt + 8)[0] + extra)
    struct.pack_into("<I", out, opt + 12, 0)
    return bytes(out), tails


def verify(pe, xex):
    if len(pe) < 0x100 or pe[:2] != b"MZ":
        raise ValueError("missing PE image")
    off = struct.unpack_from("<I", pe, 0x3C)[0]
    machine, _, _, _, _, opt_size, flags = struct.unpack_from("<HHIIIHH", pe, off + 4)
    opt = off + 24
    magic = struct.unpack_from("<H", pe, opt)[0]
    entry = struct.unpack_from("<I", pe, opt + 16)[0]
    base = struct.unpack_from("<I", pe, opt + 28)[0]
    image_size = struct.unpack_from("<I", pe, opt + 56)[0]
    subsystem = struct.unpack_from("<H", pe, opt + 68)[0]
    alignment = struct.unpack_from("<I", pe, opt + 32)[0]
    if machine != 0x1F2 or magic != 0x10B or opt_size != 224 or not flags & 0x2000 or subsystem != 14:
        raise ValueError("expected a powerpc xbox 360 PE32 DLL")
    if base != BASE or not 0 < entry < image_size:
        raise ValueError(f"unexpected base/entry (base=0x{base:08X})")
    if alignment != 4096:
        raise ValueError("expected 4 KiB system dll alignment")
    if len(xex) < 24 or xex[:4] != b"XEX2":
        raise ValueError("missing XEX2 image")
    module_flags, pe_off = struct.unpack_from(">II", xex, 4)
    security, ocount = struct.unpack_from(">II", xex, 16)
    if module_flags != 10:
        raise ValueError("expected a system dll (DLL + exports-to-title)")
    options = dict(struct.unpack_from(">II", xex, 24 + 8 * i) for i in range(ocount))
    if options.get(0x10100) != base + entry or 0x103FF not in options:
        raise ValueError("XEX entry/imports do not match the linked plugin")
    xbase = struct.unpack_from(">I", xex, security + 0x110)[0]
    if xbase != base:
        raise ValueError("XEX and PE bases differ")
    xsize = struct.unpack_from(">I", xex, security + 4)[0]
    if xsize != image_size or len(xex) - pe_off < image_size:
        raise ValueError("XEX truncated the PE image (trailing bss lost)")
    return {"pe_machine": hex(machine), "image_base": hex(base),
            "entry_point": hex(base + entry), "image_size": image_size,
            "module_flags": hex(module_flags), "xex_bytes": len(xex),
            "xex_sha256": hashlib.sha256(xex).hexdigest()}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--toolchain")
    ap.add_argument("--output", type=Path)
    args = ap.parse_args(argv)
    try:
        sysroot = toolchain(args.toolchain)
        output = (args.output or ROOT / "build").resolve()
        output.mkdir(parents=True, exist_ok=True)
        cxx = str(sysroot / "bin/clang++")
        version = subprocess.check_output([cxx, "--version"], text=True)
        if "xbox360" not in version:
            raise ValueError("compiler not configured for xbox 360")
        with tempfile.TemporaryDirectory(prefix="unify-", dir=output) as work:
            work = Path(work)
            obj = work / "plugin.obj"
            entry_obj = work / "UnifyEntry.obj"
            pe = work / "Unify360.exe"
            xex = work / "Unify360.xex"
            subprocess.run([cxx, "-std=c++03", "-O2", "-g0", "-fno-exceptions",
                            "-fno-rtti", "-fno-threadsafe-statics", "-Wall", "-Wextra",
                            "-Werror", "-I", str(ROOT / "src"),
                            "-c", str(ROOT / "src/plugin.cpp"), "-o", str(obj)], check=True)
            subprocess.run([cxx, "-mcpu=970", "-c", str(ROOT / "src/UnifyEntry.S"),
                            "-o", str(entry_obj)], check=True)
            subprocess.run([cxx, str(obj), str(entry_obj),
                            f"-Wl,/timestamp:0,/dll,/OSVERSION:0.0,/NXCOMPAT:NO,/FIXED,"
                            f"/BASE:0x{BASE:08X},/ALIGN:0x1000,/ENTRY:UnifyEntry",
                            "-Xlinker", "/SUBSYSTEM:xbox360,1.0",
                            "-o", str(pe)], check=True)
            linked = pe.read_bytes()
            prepared, tails = materialize_bss(linked)
            pe.write_bytes(prepared)
            subprocess.run([str(sysroot / "bin/synthxex"), "-t", "sysdll",
                            "-i", str(pe), "-o", str(xex)], check=True)
            proof = verify(pe.read_bytes(), xex.read_bytes())
            proof.update(materialized_sections=tails, compiler=version.splitlines()[0],
                         console_loaded=False)
            (work / "build-proof.json").write_text(json.dumps(proof, indent=2) + "\n")
            for f in work.iterdir():
                if f.name.startswith("._"):
                    continue  # macOS AppleDouble sidecar on exFAT; skip
                try:
                    f.replace(output / f.name)
                except FileNotFoundError:
                    pass
        print("built:", output / "Unify360.xex")
        print(json.dumps(proof, indent=2))
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError) as e:
        print("build stopped:", e)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
