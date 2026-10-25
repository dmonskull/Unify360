// SPDX-License-Identifier: GPL-3.0-or-later
// big-endian byte helpers. the console is ppc (big endian); every multi-byte value on
// the wire and in the rpc/xrpc buffers is big endian, so we pack/unpack by hand rather
// than rely on BitConverter (which is little endian on x86/arm).
using System;

namespace X360
{
    internal static class Be
    {
        public static uint U32(byte[] b, int off)
        {
            return (uint)((b[off] << 24) | (b[off + 1] << 16) | (b[off + 2] << 8) | b[off + 3]);
        }

        public static ulong U64(byte[] b, int off)
        {
            return ((ulong)U32(b, off) << 32) | U32(b, off + 4);
        }

        public static void PutU32(byte[] b, int off, uint v)
        {
            b[off] = (byte)(v >> 24); b[off + 1] = (byte)(v >> 16);
            b[off + 2] = (byte)(v >> 8); b[off + 3] = (byte)v;
        }

        public static void PutU64(byte[] b, int off, ulong v)
        {
            PutU32(b, off, (uint)(v >> 32));
            PutU32(b, off + 4, (uint)v);
        }

        public static byte[] Bytes32(uint v)
        {
            var b = new byte[4]; PutU32(b, 0, v); return b;
        }

        public static byte[] Bytes64(ulong v)
        {
            var b = new byte[8]; PutU64(b, 0, v); return b;
        }
    }
}
