// SPDX-License-Identifier: GPL-3.0-or-later
// memory read/write over xbdm - the universal primitive. big endian on the console.
// faithful port of python x360.memory (getmem text, getmemex binary, chunked setmem).
using System;
using System.Collections.Generic;
using System.Text;

namespace X360
{
    public sealed class Memory
    {
        // xbdm rejects an over-long command line (446); 128 data bytes keeps setmem safe.
        public const int SetmemChunk = 128;

        private readonly Transport _tp;
        public bool Fast;

        public Memory(Transport tp, bool fast = true) { _tp = tp; Fast = fast; }

        // -- block io ----------------------------------------------------------
        public static byte[] ReadBlock(Transport tp, uint address, int length)
        {
            if (length < 0) throw new ArgumentException("length < 0");
            if (length == 0) return Array.Empty<byte>();
            var resp = tp.Command($"getmem addr=0x{address:X} length={length}");
            if (resp.Code != 202) throw new XbdmException(resp.Code, resp.Message);
            var sb = new StringBuilder();
            int got = 0, want = length * 2;
            bool unreadable = false;
            // always drain to the lone '.' so the stream stays in sync for the next cmd.
            while (true)
            {
                string s = Transport.Latin1.GetString(tp.RecvLine()).TrimEnd('\r', '\n');
                if (s == ".") break;
                if (s.Length > 0 && s.Replace("?", "").Length == 0) { unreadable = true; continue; }
                got += s.Length; sb.Append(s);
            }
            if (unreadable) throw new XbdmException(404, $"memory at 0x{address:X8} not readable");
            if (got != want) throw new XbdmException(0, "getmem length mismatch");
            return FromHex(sb.ToString());
        }

        public static byte[] ReadBlockFast(Transport tp, uint address, int length)
        {
            if (length < 0) throw new ArgumentException("length < 0");
            if (length == 0) return Array.Empty<byte>();
            tp.SendRaw($"getmemex addr=0x{address:X} length=0x{length:X}\r\n");
            var resp = Transport.Parse(tp.RecvLine());
            if (resp.Code != 203) throw new XbdmException(resp.Code, $"getmemex: unexpected {resp.Code}");
            var outl = new List<byte>(length);
            while (true)
            {
                var hdr = tp.RecvExact(2);
                int flags = hdr[0] | (hdr[1] << 8);       // 2-byte LE flags/count header
                int count = flags & 0x7FFF;
                if (count > 0) outl.AddRange(tp.RecvExact(count));
                if ((flags & 0x8000) != 0 || outl.Count >= length) break;
            }
            if (outl.Count < length) throw new XbdmException(404, $"0x{address + (uint)outl.Count:X8} not mapped");
            var res = new byte[length];
            outl.CopyTo(0, res, 0, length);
            return res;
        }

        public static int WriteBlock(Transport tp, uint address, byte[] data)
        {
            for (int off = 0; off < data.Length; off += SetmemChunk)
            {
                int n = Math.Min(SetmemChunk, data.Length - off);
                var chunk = new byte[n];
                Array.Copy(data, off, chunk, 0, n);
                var resp = tp.Command($"setmem addr=0x{address + (uint)off:X} data={ToHex(chunk)}");
                if (resp.Code != 200) throw new XbdmException(resp.Code, resp.Message);
            }
            return data.Length;
        }

        public byte[] Read(uint address, int length)
        {
            try { return Fast ? ReadBlockFast(_tp, address, length) : ReadBlock(_tp, address, length); }
            catch (XbdmException) { return ReadBlock(_tp, address, length); }  // text fallback
        }

        public int Write(uint address, byte[] data) => WriteBlock(_tp, address, data);

        // -- typed accessors (big endian) --------------------------------------
        public byte U8(uint a) => Read(a, 1)[0];
        public ushort U16(uint a) { var b = Read(a, 2); return (ushort)((b[0] << 8) | b[1]); }
        public uint U32(uint a) => Be.U32(Read(a, 4), 0);
        public ulong U64(uint a) => Be.U64(Read(a, 8), 0);
        public sbyte I8(uint a) => (sbyte)U8(a);
        public short I16(uint a) => (short)U16(a);
        public int I32(uint a) => (int)U32(a);
        public long I64(uint a) => (long)U64(a);
        public float F32(uint a) { var b = Read(a, 4); Array.Reverse(b); return BitConverter.ToSingle(b, 0); }
        public double F64(uint a) { var b = Read(a, 8); Array.Reverse(b); return BitConverter.ToDouble(b, 0); }

        public void SetU8(uint a, byte v) => Write(a, new[] { v });
        public void SetU16(uint a, ushort v) => Write(a, new[] { (byte)(v >> 8), (byte)v });
        public void SetU32(uint a, uint v) => Write(a, Be.Bytes32(v));
        public void SetU64(uint a, ulong v) => Write(a, Be.Bytes64(v));
        public void SetI32(uint a, int v) => SetU32(a, (uint)v);
        public void SetI64(uint a, long v) => SetU64(a, (ulong)v);
        public void SetF32(uint a, float v) { var b = BitConverter.GetBytes(v); Array.Reverse(b); Write(a, b); }
        public void SetF64(uint a, double v) { var b = BitConverter.GetBytes(v); Array.Reverse(b); Write(a, b); }

        public string CString(uint a, int maxlen = 256)
        {
            var sb = new StringBuilder();
            for (int i = 0; i < maxlen; i++)
            {
                byte c = U8(a + (uint)i);
                if (c == 0) break;
                sb.Append((char)c);
            }
            return sb.ToString();
        }

        public void SetCString(uint a, string s)
        {
            var raw = Encoding.ASCII.GetBytes(s);
            var buf = new byte[raw.Length + 1];
            Array.Copy(raw, buf, raw.Length);
            Write(a, buf);
        }

        // -- hex helpers -------------------------------------------------------
        internal static string ToHex(byte[] b)
        {
            var c = new char[b.Length * 2];
            const string H = "0123456789abcdef";
            for (int i = 0; i < b.Length; i++) { c[i * 2] = H[b[i] >> 4]; c[i * 2 + 1] = H[b[i] & 0xF]; }
            return new string(c);
        }

        internal static byte[] FromHex(string s)
        {
            var b = new byte[s.Length / 2];
            for (int i = 0; i < b.Length; i++)
                b[i] = Convert.ToByte(s.Substring(i * 2, 2), 16);
            return b;
        }
    }
}
