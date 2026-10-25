// SPDX-License-Identifier: GPL-3.0-or-later
// xrpc backend: the shared-buffer poll method. no console command - pure getmem/setmem.
// a resident poller (RPC.xex, or our own plugin's xrpc service) watches a fixed buffer.
// read the buffer pointer, stage args, write a magic command word, poll the result slot.
using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;

namespace X360
{
    public sealed class Xrpc
    {
        // defaults from the classic XRPCLib (community RPC.xex); our plugin's getmem hook
        // publishes the buffer pointer at PointerAddr so these tools work unmodified.
        public const uint PointerAddr = 0x91C01A2E;
        public const uint MagicCall = 0x82000000;
        public const uint MagicResolve = 0x82000001;
        private const int RESULT_OFFSET = 4092, ARG_OFFSET = 8, COUNT_OFFSET = 4;
        private const int STRING_OFFSET = 1500, FLOAT_OFFSET = 2700, BYTE_OFFSET = 3200;

        private readonly Memory _mem;
        public uint PointerAddress;
        public uint MagicCallWord;
        public uint MagicResolveWord;
        public int SettleMs;
        private uint _base;

        public Xrpc(Memory mem, uint pointerAddr = PointerAddr, uint magicCall = MagicCall,
                    uint magicResolve = MagicResolve, int settleMs = 50)
        {
            _mem = mem; PointerAddress = pointerAddr; MagicCallWord = magicCall;
            MagicResolveWord = magicResolve; SettleMs = settleMs;
        }

        // bind directly to a known buffer base (e.g. via the plugin status block).
        public void BindBase(uint baseAddr) { _base = baseAddr; }

        private uint Buffer()
        {
            if (_base == 0)
            {
                _base = _mem.U32(PointerAddress);
                if (_base == 0) throw new XbdmException(0, "xrpc poller not present (null buffer pointer)");
            }
            return _base;
        }

        private List<ulong> StageArgs(uint baseAddr, object[] args)
        {
            var values = new List<ulong>();
            uint spos = baseAddr + STRING_OFFSET, fpos = baseAddr + FLOAT_OFFSET, bpos = baseAddr + BYTE_OFFSET;
            foreach (var a in args)
            {
                switch (a)
                {
                    case bool b: values.Add(b ? 1UL : 0UL); break;
                    case float f: _mem.SetF32(fpos, f); values.Add(fpos); fpos += 8; break;
                    case double d: _mem.SetF32(fpos, (float)d); values.Add(fpos); fpos += 8; break;
                    case string s:
                        {
                            var raw = NullTerm(s);
                            _mem.Write(spos, raw); values.Add(spos); spos += (uint)raw.Length;
                            break;
                        }
                    case byte[] by:
                        _mem.Write(bpos, by); values.Add(bpos); bpos += (uint)by.Length + 2; break;
                    default:
                        values.Add((ulong)Convert.ToInt64(a) & 0xFFFFFFFFFFFFFFFF); break;
                }
            }
            return values;
        }

        private uint Run(uint baseAddr, uint magic, object[] args)
        {
            var values = StageArgs(baseAddr, args);
            if (values.Count > 0)
            {
                var blob = new byte[values.Count * 8];
                for (int i = 0; i < values.Count; i++) Be.PutU64(blob, i * 8, values[i]);
                _mem.Write(baseAddr + ARG_OFFSET, blob);
            }
            _mem.SetU32(baseAddr + COUNT_OFFSET, (uint)values.Count);
            _mem.SetU32(baseAddr, magic);        // fire
            Thread.Sleep(SettleMs);
            return _mem.U32(baseAddr + RESULT_OFFSET);
        }

        public uint Call(uint address, object[] args = null)
        {
            uint baseAddr = Buffer();
            var all = new List<object> { address };
            if (args != null) all.AddRange(args);
            return Run(baseAddr, MagicCallWord, all.ToArray());
        }

        public uint Resolve(string module, int ordinal)
        {
            uint baseAddr = Buffer();
            return Run(baseAddr, MagicResolveWord, new object[] { module, ordinal });
        }

        public bool Available()
        {
            if (_base != 0) return true;   // bound directly
            try { return _mem.U32(PointerAddress) != 0; }
            catch (XbdmException) { return false; }
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
