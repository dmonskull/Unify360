"""the built in xbdm command surface: console info, files, modules, threads,
debug control, screenshots, reboot. no function calling here - that lives in the
jrpc / xdrpc / xrpc backends. everything is a thin wrapper over the line protocol."""
import os
import re
import struct
import time

from .transport import XbdmError


class Xbdm:
    def __init__(self, tp):
        self.tp = tp

    # -- console info ------------------------------------------------------
    def dmversion(self):
        return self.tp.command("dmversion").message.strip()

    def name(self):
        return self.tp.command("dbgname").message.strip()

    def console_type(self):
        """'devkit' / 'testkit' / 'retail' from the built in consoletype command."""
        return self.tp.command("consoletype").message.strip()

    def systeminfo(self):
        _, lines = self.tp.command_lines("systeminfo")
        info = {}
        for line in lines:
            for pair in line.split():
                if "=" in pair:
                    k, v = pair.split("=", 1)
                    info[k] = v
        return info

    def features(self):
        """the built in consolefeatures reply (feature flags), not the jrpc hook."""
        return self.tp.command("consolefeatures").message.strip()

    def pid(self):
        m = self.tp.command("getpid").message
        return int(m.split("pid=")[-1].strip(), 16)

    def is_devkit(self):
        t = self.console_type().lower()
        return "devkit" in t or "test" in t

    # -- memory ownership helpers are in memory.py; here are files ---------
    def drives(self):
        _, lines = self.tp.command_lines("drivelist")
        out = []
        for line in lines:
            m = re.search(r'drivename="([^"]+)"', line)
            if m:
                out.append(m.group(1))
        return out

    def dirlist(self, path):
        _, lines = self.tp.command_lines(f'dirlist name="{path}"')
        out = []
        for line in lines:
            e = {}
            m = re.search(r'name="([^"]+)"', line)
            if m:
                e["name"] = m.group(1)
            hi = re.search(r"sizehi=0x([0-9a-fA-F]+)", line)
            lo = re.search(r"sizelo=0x([0-9a-fA-F]+)", line)
            if hi and lo:
                e["size"] = (int(hi.group(1), 16) << 32) | int(lo.group(1), 16)
            e["directory"] = line.rstrip().endswith("directory")
            out.append(e)
        return out

    def get_file(self, remote, local):
        self.tp.send_raw(f'getfile name="{remote}"\r\n'.encode())
        header = self.tp.recv_line()
        if not header.startswith(b"203"):
            raise XbdmError(0, f"getfile: {header!r}")
        size = int.from_bytes(self.tp.recv_exact(4), "little")
        with open(local, "wb") as f:
            left = size
            while left:
                chunk = self.tp.recv_exact(min(4096, left))
                f.write(chunk)
                left -= len(chunk)
        return size

    def send_file(self, local, remote, overwrite=True):
        size = os.path.getsize(local)
        def begin():
            self.tp.send_raw(
                f'sendfile name="{remote}" length=0x{size:X}\r\n'.encode()
            )
            return self.tp.recv_line()
        header = begin()
        if not header.startswith(b"204"):
            if header.startswith(b"410") and overwrite:
                self.delete(remote)
                header = begin()
                if not header.startswith(b"204"):
                    raise XbdmError(0, f"sendfile after overwrite: {header!r}")
            else:
                raise XbdmError(0, f"sendfile: {header!r}")
        with open(local, "rb") as f:
            while True:
                chunk = f.read(4096)
                if not chunk:
                    break
                self.tp.send_raw(chunk)
        done = self.tp.recv_line()
        if not done.startswith(b"200"):
            raise XbdmError(0, f"sendfile result: {done!r}")
        return size

    def delete(self, remote, directory=False):
        cmd = f'delete name="{remote}"' + (" dir" if directory else "")
        return self.tp.command(cmd).code == 200

    def mkdir(self, remote):
        return self.tp.command(f'mkdir name="{remote}"').code

    def rename(self, old, new):
        return self.tp.command(f'rename name="{old}" newname="{new}"').code == 200

    # -- modules / titles --------------------------------------------------
    def modules(self):
        _, lines = self.tp.command_lines("modules")
        out = []
        for line in lines:
            e = {}
            m = re.search(r'name="([^"]+)"', line)
            if m:
                e["name"] = m.group(1)
            for key in ("base", "size", "timestamp", "check"):
                mm = re.search(rf"{key}=0x([0-9a-fA-F]+)", line)
                if mm:
                    e[key] = int(mm.group(1), 16)
            out.append(e)
        return out

    def module_base(self, name):
        for m in self.modules():
            if m.get("name", "").lower() == name.lower():
                return m.get("base")
        return None

    def running_xbe(self):
        _, lines = self.tp.command_lines("xbeinfo running")
        for line in lines:
            m = re.search(r'name="([^"]+)"', line)
            if m:
                return m.group(1)
        return None

    # -- debug control -----------------------------------------------------
    def stop(self):
        self.tp.command("stop")

    def go(self):
        self.tp.command("go")

    def threads(self):
        _, lines = self.tp.command_lines("threads")
        return [int(x) & 0xFFFFFFFF for x in lines if x.strip().lstrip("-").isdigit()]

    # -- system actions ----------------------------------------------------
    def reboot(self, title=None, directory=None, cold=True):
        if title:
            d = directory or title.rsplit("\\", 1)[0]
            return self.tp.command(f'magicboot title="{title}" directory="{d}"').code
        return self.tp.command("magicboot" + (" cold" if cold else "")).code

    def set_system_time(self, unix_time=None):
        if unix_time is None:
            unix_time = int(time.time())
        ft = (unix_time + 11644473600) * 10_000_000
        return self.tp.command(
            f"setsystime clockhi=0x{(ft >> 32) & 0xFFFFFFFF:08X} "
            f"clocklo=0x{ft & 0xFFFFFFFF:08X}"
        ).code == 200

    # -- screenshot (devkit xbdm) -----------------------------------------
    def screenshot_raw(self):
        """returns (meta dict, framebuffer bytes). untiling is left to the caller
        (pillow/numpy are optional). the hard numbers (pitch/width/height) are in
        meta so a caller can untile with the same xenos math py-xbdm uses."""
        self.tp.send_raw(b"screenshot\r\n")
        resp = self.tp.parse(self.tp.recv_line())
        if resp.code != 203:
            raise XbdmError(resp.code or 0, "screenshot not supported here")
        meta_line = self.tp.recv_line().decode("latin-1")
        meta = {}
        for m in re.finditer(r"(\w+)=0x([0-9a-fA-F]+)", meta_line):
            meta[m.group(1)] = int(m.group(2), 16)
        fb = self.tp.recv_exact(meta["framebuffersize"])
        return meta, fb
