// SPDX-License-Identifier: GPL-3.0-or-later
// the built-in xbdm command surface: console info, files, modules, threads, reboot.
// thin wrappers over the line protocol; no function calling (that is jrpc/xdrpc/xrpc).
using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;

namespace X360
{
    public sealed class ModuleInfo
    {
        public string Name; public uint Base; public uint Size; public uint Timestamp; public uint Check;
    }

    public sealed class DirEntry
    {
        public string Name; public ulong Size; public bool Directory;
    }

    public sealed class Xbdm
    {
        private readonly Transport _tp;
        public Xbdm(Transport tp) { _tp = tp; }

        // -- console info ------------------------------------------------------
        public string DmVersion() => _tp.Command("dmversion").Message.Trim();
        public string Name() => _tp.Command("dbgname").Message.Trim();
        public string ConsoleType() => _tp.Command("consoletype").Message.Trim();
        public string Features() => _tp.Command("consolefeatures").Message.Trim();

        public Dictionary<string, string> SystemInfo()
        {
            var (_, lines) = _tp.CommandLines("systeminfo");
            var info = new Dictionary<string, string>();
            foreach (var line in lines)
                foreach (var pair in line.Split(' '))
                {
                    int eq = pair.IndexOf('=');
                    if (eq > 0) info[pair.Substring(0, eq)] = pair.Substring(eq + 1);
                }
            return info;
        }

        public uint Pid()
        {
            var m = _tp.Command("getpid").Message;
            var idx = m.IndexOf("pid=", StringComparison.Ordinal);
            return Convert.ToUInt32(m.Substring(idx + 4).Trim(), 16);
        }

        public bool IsDevkit()
        {
            var t = ConsoleType().ToLowerInvariant();
            return t.Contains("devkit") || t.Contains("test");
        }

        // -- files -------------------------------------------------------------
        public List<string> Drives()
        {
            var (_, lines) = _tp.CommandLines("drivelist");
            var outl = new List<string>();
            foreach (var line in lines)
            {
                var m = Regex.Match(line, "drivename=\"([^\"]+)\"");
                if (m.Success) outl.Add(m.Groups[1].Value);
            }
            return outl;
        }

        public List<DirEntry> DirList(string path)
        {
            var (_, lines) = _tp.CommandLines($"dirlist name=\"{path}\"");
            var outl = new List<DirEntry>();
            foreach (var line in lines)
            {
                var e = new DirEntry();
                var m = Regex.Match(line, "name=\"([^\"]+)\"");
                if (m.Success) e.Name = m.Groups[1].Value;
                var hi = Regex.Match(line, "sizehi=0x([0-9a-fA-F]+)");
                var lo = Regex.Match(line, "sizelo=0x([0-9a-fA-F]+)");
                if (hi.Success && lo.Success)
                    e.Size = ((ulong)Convert.ToUInt32(hi.Groups[1].Value, 16) << 32)
                             | Convert.ToUInt32(lo.Groups[1].Value, 16);
                e.Directory = line.TrimEnd().EndsWith("directory", StringComparison.Ordinal);
                outl.Add(e);
            }
            return outl;
        }

        public long GetFile(string remote, string local)
        {
            _tp.SendRaw($"getfile name=\"{remote}\"\r\n");
            var header = _tp.RecvLine();
            if (!StartsWith(header, "203")) throw new XbdmException(0, "getfile failed");
            uint size = (uint)(_tp.RecvExact(4) is var s ? (s[0] | (s[1] << 8) | (s[2] << 16) | (s[3] << 24)) : 0);
            using (var f = File.Create(local))
            {
                long left = size;
                while (left > 0)
                {
                    var chunk = _tp.RecvExact((int)Math.Min(4096, left));
                    f.Write(chunk, 0, chunk.Length);
                    left -= chunk.Length;
                }
            }
            return size;
        }

        public long SendFile(string local, string remote, bool overwrite = true)
        {
            long size = new FileInfo(local).Length;
            byte[] Begin()
            {
                _tp.SendRaw($"sendfile name=\"{remote}\" length=0x{size:X}\r\n");
                return _tp.RecvLine();
            }
            var header = Begin();
            if (!StartsWith(header, "204"))
            {
                if (StartsWith(header, "410") && overwrite)
                {
                    Delete(remote);
                    header = Begin();
                    if (!StartsWith(header, "204")) throw new XbdmException(0, "sendfile after overwrite failed");
                }
                else throw new XbdmException(0, "sendfile failed");
            }
            using (var f = File.OpenRead(local))
            {
                var buf = new byte[4096];
                int n;
                while ((n = f.Read(buf, 0, buf.Length)) > 0)
                {
                    if (n == buf.Length) _tp.SendRaw(buf);
                    else { var last = new byte[n]; Array.Copy(buf, last, n); _tp.SendRaw(last); }
                }
            }
            var done = _tp.RecvLine();
            if (!StartsWith(done, "200")) throw new XbdmException(0, "sendfile result not 200");
            return size;
        }

        public bool Delete(string remote, bool directory = false)
            => _tp.Command($"delete name=\"{remote}\"" + (directory ? " dir" : "")).Code == 200;

        public int Mkdir(string remote) => _tp.Command($"mkdir name=\"{remote}\"").Code;

        public bool Rename(string oldName, string newName)
            => _tp.Command($"rename name=\"{oldName}\" newname=\"{newName}\"").Code == 200;

        // -- modules / threads -------------------------------------------------
        public List<ModuleInfo> Modules()
        {
            var (_, lines) = _tp.CommandLines("modules");
            var outl = new List<ModuleInfo>();
            foreach (var line in lines)
            {
                var e = new ModuleInfo();
                var m = Regex.Match(line, "name=\"([^\"]+)\"");
                if (m.Success) e.Name = m.Groups[1].Value;
                e.Base = HexField(line, "base");
                e.Size = HexField(line, "size");
                e.Timestamp = HexField(line, "timestamp");
                e.Check = HexField(line, "check");
                outl.Add(e);
            }
            return outl;
        }

        public uint ModuleBase(string name)
        {
            foreach (var m in Modules())
                if (m.Name != null && string.Equals(m.Name, name, StringComparison.OrdinalIgnoreCase))
                    return m.Base;
            return 0;
        }

        public void Stop() => _tp.Command("stop");
        public void Go() => _tp.Command("go");

        // -- system actions ----------------------------------------------------
        public int Reboot(string title = null, string directory = null, bool cold = true)
        {
            if (!string.IsNullOrEmpty(title))
            {
                string d = directory ?? title.Substring(0, Math.Max(0, title.LastIndexOf('\\')));
                return _tp.Command($"magicboot title=\"{title}\" directory=\"{d}\"").Code;
            }
            return _tp.Command("magicboot" + (cold ? " cold" : "")).Code;
        }

        // -- helpers -----------------------------------------------------------
        private static uint HexField(string line, string key)
        {
            var m = Regex.Match(line, key + "=0x([0-9a-fA-F]+)");
            return m.Success ? Convert.ToUInt32(m.Groups[1].Value, 16) : 0;
        }

        private static bool StartsWith(byte[] line, string prefix)
        {
            if (line.Length < prefix.Length) return false;
            for (int i = 0; i < prefix.Length; i++) if (line[i] != (byte)prefix[i]) return false;
            return true;
        }
    }
}
