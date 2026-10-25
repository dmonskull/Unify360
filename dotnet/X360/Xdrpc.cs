// SPDX-License-Identifier: GPL-3.0-or-later
// xdrpc backend: microsoft's native `rpc` command, version 4. a faithful port of the
// disassembled marshaler: integer/pointer args (<=8) in address or module+ordinal mode.
// wire framing verified against our plugin's rpc handler and the devkit native rpc.
using System;
using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;

namespace X360
{
    public sealed class Xdrpc
    {
        private const int SLOT = 8;              // every buffer slot is one big-endian qword
        private const ulong ERROR_IO_PENDING = 997;
        private const int HEADER_SLOTS = 8;      // 6 fixed + 2 target slots

        private readonly Transport _tp;
        public int MaxPending;                   // ~one console poll each; 300 ~= 30s

        public Xdrpc(Transport tp, int maxPending = 300) { _tp = tp; MaxPending = maxPending; }

        private static int Align8(int n) => (n + SLOT - 1) & ~(SLOT - 1);

        private uint Start(int bufSize, bool system, int processor, string thread)
        {
            string mode = system ? "system" : "title";
            _tp.SendRaw($"rpc {mode} version=4 buf_size={bufSize} processor={processor} thread={thread}\r\n");
            string resp = Transport.Latin1.GetString(_tp.RecvLine()).TrimEnd('\r', '\n');
            if (resp.Contains("error="))
                throw new XbdmException(200, resp.Split(new[] { "error=" }, 2, StringSplitOptions.None)[1]);
            var m = Regex.Match(resp, @"(\d+)-\s*buf_addr=([0-9a-fA-F]+)");
            if (!m.Success) throw new XbdmException(0, $"rpc start: {resp}");
            if (int.Parse(m.Groups[1].Value) != 204) throw new XbdmException(int.Parse(m.Groups[1].Value), resp);
            return Convert.ToUInt32(m.Groups[2].Value, 16);
        }

        public ulong Call(uint address = 0, object[] args = null, bool system = true,
                          string module = null, int ordinal = 0, int processor = 5,
                          string thread = "", bool floatReturn = false)
        {
            args = args ?? Array.Empty<object>();
            if (args.Length > 8) throw new ArgumentException("native rpc supports at most 8 integer args");
            int argc = args.Length;

            int valueRegion = (HEADER_SLOTS + argc) * SLOT;
            byte[] moduleBytes = module != null ? NullTerm(module) : Array.Empty<byte>();
            int moduleRegion = Align8(moduleBytes.Length);

            var blobs = new List<(int idx, byte[] raw)>();
            int blobRegion = 0;
            for (int i = 0; i < argc; i++)
            {
                byte[] raw = null;
                if (args[i] is string s) raw = NullTerm(s);
                else if (args[i] is byte[] b) raw = b;
                if (raw != null) { blobs.Add((i, raw)); blobRegion += Align8(raw.Length); }
            }
            int bufSize = valueRegion + moduleRegion + blobRegion;

            uint bufAddr = Start(bufSize, system, processor, thread);

            var data = new byte[bufSize];
            void Put(int slot, ulong value) => Be.PutU64(data, slot * SLOT, value);

            Put(0, 0);                                   // hresult (out)
            Put(1, 0);                                   // return value (out)
            Put(2, 0);                                   // post method call
            Put(3, floatReturn ? 1UL : 0UL);             // flags: bit0 = float return
            Put(4, (ulong)argc);                         // integer arg count
            Put(5, 0);                                   // float arg count
            if (module != null)
            {
                Put(6, bufAddr + (ulong)valueRegion);    // module name pointer
                Put(7, (ulong)ordinal);
                Array.Copy(moduleBytes, 0, data, valueRegion, moduleBytes.Length);
            }
            else { Put(6, 0); Put(7, address); }

            // scalar int args
            for (int i = 0; i < argc; i++)
            {
                if (args[i] is string || args[i] is byte[]) continue;
                Put(HEADER_SLOTS + i, (ulong)Convert.ToInt64(args[i]) & 0xFFFFFFFFFFFFFFFF);
            }
            // str/bytes args: write bytes into the blob region, pass the pointer
            int pos = valueRegion + moduleRegion;
            foreach (var (idx, raw) in blobs)
            {
                Put(HEADER_SLOTS + idx, bufAddr + (ulong)pos);
                Array.Copy(raw, 0, data, pos, raw.Length);
                pos += Align8(raw.Length);
            }

            _tp.SendRaw(data);
            string ok = Transport.Latin1.GetString(_tp.RecvLine()).TrimEnd('\r', '\n');
            if (!ok.StartsWith("203", StringComparison.Ordinal)) throw new XbdmException(0, $"rpc send: {ok}");

            // framing: the console streams 8-byte big-endian status words (997 while the
            // call runs); the first non-997 word is the final status (0 = ok) and is NOT
            // part of the buffer. the full buf_size result buffer follows it.
            int pending = 0;
            ulong status = Be.U64(_tp.RecvExact(SLOT), 0);
            while (status == ERROR_IO_PENDING)
            {
                if (++pending > MaxPending) throw new XbdmException(0, "rpc call did not complete (target never returned)");
                status = Be.U64(_tp.RecvExact(SLOT), 0);
            }
            if (status != 0) throw new XbdmException(0, $"rpc completion status 0x{status:X}");
            var result = _tp.RecvExact(bufSize);

            ulong hresult = Be.U64(result, 0);
            ulong retval = Be.U64(result, SLOT);
            if ((hresult & 0xFFFFFFFF) != 0) throw new XbdmException(0, $"rpc call hresult 0x{hresult & 0xFFFFFFFF:X8}");
            return retval;
        }

        public uint CallInt(uint address, object[] args = null, bool system = true, string module = null, int ordinal = 0)
            => (uint)(Call(address, args, system, module, ordinal) & 0xFFFFFFFF);

        public bool Available()
        {
            // a present `rpc` command replies about the version; an absent one -> 407.
            _tp.SendRaw("rpc\r\n");
            string resp = Transport.Latin1.GetString(_tp.RecvLine()).TrimEnd('\r', '\n');
            return resp.Contains("error=") || resp.StartsWith("20", StringComparison.Ordinal);
        }

        private static byte[] NullTerm(string s)
        {
            var a = Encoding.ASCII.GetBytes(s);
            var b = new byte[a.Length + 1];
            Array.Copy(a, b, a.Length);
            return b;
        }
    }
}
