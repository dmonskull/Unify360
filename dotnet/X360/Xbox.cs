// SPDX-License-Identifier: GPL-3.0-or-later
// the one object you use: new Xbox(ip). owns the xbdm socket and exposes every layer.
// Call() auto-detects the best function-call backend (jrpc / xdrpc / xrpc / our plugin),
// so callers never care which method the console supports.
using System;
using System.Collections.Generic;

namespace X360
{
    public sealed class Xbox : IDisposable
    {
        public Transport Tp { get; }
        public Memory Mem { get; }
        public Xbdm Xbdm { get; }
        public Jrpc Jrpc { get; }
        public Xdrpc Xdrpc { get; }
        public Xrpc Xrpc { get; }

        private readonly object _lock = new object();
        private List<string> _backends;

        public Xbox(string host, int port = 730, double timeout = 10.0, bool fastMemory = true)
        {
            Tp = new Transport(host, port, timeout);
            Mem = new Memory(Tp, fastMemory);
            Xbdm = new Xbdm(Tp);
            Jrpc = new Jrpc(Tp);
            Xdrpc = new Xdrpc(Tp);
            Xrpc = new Xrpc(Mem);
        }

        public Xbox Connect() { lock (_lock) Tp.Connect(); return this; }
        public void Close() { lock (_lock) Tp.Close(); }
        public void Dispose() => Close();

        // -- detection ---------------------------------------------------------
        // point the xrpc backend at Unify360's shared buffer via the plugin status block
        // (UNIFY001 magic, status[7] = buffer address). returns the buffer addr, or 0.
        public uint UsePluginXrpc()
        {
            lock (_lock)
            {
                uint baseAddr = Xbdm.ModuleBase("Unify360.xex");
                if (baseAddr == 0) return 0;
                byte[] blob = null;
                foreach (int sz in new[] { 0xC000, 0xB000, 0xA000, 0x9000, 0x8000 })
                {
                    try { blob = Mem.Read(baseAddr, sz); break; } catch (XbdmException) { }
                }
                if (blob == null) return 0;
                int mi = IndexOf(blob, System.Text.Encoding.ASCII.GetBytes("UNIFY001"));
                if (mi < 0) return 0;
                uint buf = Be.U32(blob, mi + 7 * 4);   // status[7]
                if (buf != 0) Xrpc.BindBase(buf);
                return buf;
            }
        }

        // which function-call methods this console answers, best first.
        public List<string> Backends(bool refresh = false)
        {
            lock (_lock)
            {
                if (_backends != null && !refresh) return _backends;
                var found = new List<string>();
                try { UsePluginXrpc(); } catch (XbdmException) { }
                try { if (Jrpc.Available()) found.Add("jrpc"); } catch (Exception) { }
                try { if (Xdrpc.Available()) found.Add("xdrpc"); } catch (Exception) { }
                try { if (Xrpc.Available()) found.Add("xrpc"); } catch (Exception) { }
                _backends = found;
                return found;
            }
        }

        // -- the unified call --------------------------------------------------
        public object Call(uint address = 0, object[] args = null, string ret = "int",
                           bool system = true, string module = null, int ordinal = 0, string prefer = null)
        {
            args = args ?? Array.Empty<object>();
            var order = prefer != null ? new List<string> { prefer } : Backends();
            if (order.Count == 0)
                throw new XbdmException(0, "no function-call backend on this console "
                    + "(load Unify360.xex, JRPC2, XDRPC or an xrpc poller)");
            Exception last = null;
            foreach (var name in order)
            {
                try { return CallWith(name, address, args, ret, system, module, ordinal); }
                catch (Exception e) when (e is XbdmException || e is NotImplementedException) { last = e; }
            }
            throw last;
        }

        private object CallWith(string name, uint address, object[] args, string ret,
                                bool system, string module, int ordinal)
        {
            lock (_lock)
            {
                if (name == "jrpc")
                {
                    int rt = ret == "void" ? JrpcType.Void : ret == "uint64" ? JrpcType.UInt64
                           : ret == "float" ? JrpcType.Float : ret == "string" ? JrpcType.String : JrpcType.Int;
                    if (ret == "void") { Jrpc.Call(address, args, JrpcType.Void, system, module, ordinal); return null; }
                    string raw = Jrpc.Call(address, args, rt, system, module, ordinal);
                    if (ret == "int") return (uint)(Jrpc.ParseHex64(raw) & 0xFFFFFFFF);
                    if (ret == "uint64") return Jrpc.ParseHex64(raw);
                    if (ret == "float") return float.Parse(raw, System.Globalization.CultureInfo.InvariantCulture);
                    return raw;
                }
                if (name == "xdrpc")
                {
                    if (module != null && ret == "float") throw new NotImplementedException("xdrpc float return pending");
                    ulong v = Xdrpc.Call(address, args, system, module, ordinal, 5, "", ret == "float");
                    if (ret == "void") return null;
                    if (ret == "int") return (uint)(v & 0xFFFFFFFF);
                    return v;
                }
                if (name == "xrpc")
                {
                    uint addr = module != null ? Xrpc.Resolve(module, ordinal) : address;
                    uint v = Xrpc.Call(addr, args);
                    return ret == "void" ? (object)null : v;
                }
                throw new ArgumentException($"unknown backend {name}");
            }
        }

        // -- convenience -------------------------------------------------------
        public uint Resolve(string module, int ordinal)
        {
            foreach (var name in Backends())
            {
                try
                {
                    if (name == "jrpc") return Jrpc.Resolve(module, ordinal);
                    if (name == "xrpc") return Xrpc.Resolve(module, ordinal);
                }
                catch (XbdmException) { }
            }
            throw new XbdmException(0, "no backend can resolve exports here");
        }

        public uint TitleId()
        {
            if (Backends().Contains("jrpc")) return Jrpc.TitleId();
            return System.Convert.ToUInt32(Call(module: "xam.xex", ordinal: 463, ret: "int"));
        }

        public void Notify(string text, int kind = 34)
        {
            if (Backends().Contains("jrpc")) { Jrpc.Notify(text, kind); return; }
            uint addr = Resolve("xam.xex", 656);
            Call(addr, new object[] { kind, 0, 2, text, 0 }, "void");
        }

        public byte[] Read(uint address, int length) { lock (_lock) return Mem.Read(address, length); }
        public int Write(uint address, byte[] data) { lock (_lock) return Mem.Write(address, data); }

        private static int IndexOf(byte[] hay, byte[] needle)
        {
            for (int i = 0; i + needle.Length <= hay.Length; i++)
            {
                bool ok = true;
                for (int k = 0; k < needle.Length; k++) if (hay[i + k] != needle[k]) { ok = false; break; }
                if (ok) return i;
            }
            return -1;
        }
    }
}
