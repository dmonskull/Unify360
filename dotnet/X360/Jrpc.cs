// SPDX-License-Identifier: GPL-3.0-or-later
// jrpc / jrpc2 backend: the extended `consolefeatures` command. speaks exactly the
// wire format JRPC2.xex (and our plugin's consolefeatures hook) expect. the '\' in the
// params strings are LITERAL backslashes on the wire.
using System;
using System.Globalization;
using System.Text;

namespace X360
{
    public static class JrpcType
    {
        public const int Void = 0, Int = 1, String = 2, Float = 3, Byte = 4,
                         IntArray = 5, FloatArray = 6, ByteArray = 7, UInt64 = 8, UInt64Array = 9;
    }

    public sealed class Jrpc
    {
        private const int VERSION = 2;
        private const int OP_RESOLVE = 9, OP_CPUKEY = 10, OP_SHUTDOWN = 11, OP_XNOTIFY = 12,
                          OP_KERNELVERSION = 13, OP_SETLEDS = 14, OP_TEMPERATURE = 15,
                          OP_TITLEID = 16, OP_CONSOLETYPE = 17, OP_CONSTMEM = 18;

        private readonly Transport _tp;
        public Jrpc(Transport tp) { _tp = tp; }

        // -- argument encoding -------------------------------------------------
        private static string Encode(object arg)
        {
            switch (arg)
            {
                case bool b: return $"{JrpcType.Int}\\{(b ? 1 : 0)}\\";
                case float f: return $"{JrpcType.Float}\\{f.ToString(CultureInfo.InvariantCulture)}\\";
                case double d: return $"{JrpcType.Float}\\{d.ToString(CultureInfo.InvariantCulture)}\\";
                case string s:
                    {
                        var raw = Encoding.ASCII.GetBytes(s);
                        return $"{JrpcType.ByteArray}/{raw.Length}\\{Memory.ToHex(raw).ToUpperInvariant()}\\";
                    }
                case byte[] by:
                    return $"{JrpcType.ByteArray}/{by.Length}\\{Memory.ToHex(by).ToUpperInvariant()}\\";
                default:
                    {
                        // any integer kind -> 32-bit
                        ulong v = Convert.ToUInt64(Convert.ToInt64(arg) & 0xFFFFFFFFL);
                        return $"{JrpcType.Int}\\{v & 0xFFFFFFFF}\\";
                    }
            }
        }

        private string Send(string body)
        {
            var resp = _tp.Command(body);
            string msg = resp.Message;
            while (msg.Contains("buf_addr="))
            {
                string buf = msg.Split(new[] { "buf_addr=" }, StringSplitOptions.None)[1];
                buf = buf.Substring(0, Math.Min(8, buf.Length));
                resp = _tp.Command($"consolefeatures buf_addr=0x{buf}");
                msg = resp.Message;
            }
            if (msg.Contains("error="))
                throw new XbdmException(resp.Code, msg.Split(new[] { "error=" }, 2, StringSplitOptions.None)[1]);
            return msg.Trim();
        }

        // -- the generic call --------------------------------------------------
        public string Call(uint address = 0, object[] args = null, int ret = JrpcType.Int,
                           bool system = true, string module = null, int ordinal = 0, int arraySize = 0)
        {
            args = args ?? Array.Empty<object>();
            var sb = new StringBuilder();
            sb.Append($"consolefeatures ver={VERSION} type={ret}");
            if (system) sb.Append(" system");
            if (module != null) sb.Append($" module=\"{module}\" ord={ordinal}");
            sb.Append($" as={arraySize}");
            sb.Append($" params=\"A\\{address:X}\\A\\{args.Length}\\");
            foreach (var a in args) sb.Append(Encode(a));
            sb.Append("\"");
            return Send(sb.ToString());
        }

        public uint CallInt(uint address, object[] args = null, bool system = true, string module = null, int ordinal = 0)
            => ParseHex(Call(address, args, JrpcType.Int, system, module, ordinal));
        public ulong CallUInt64(uint address, object[] args = null, bool system = true, string module = null, int ordinal = 0)
            => ParseHex64(Call(address, args, JrpcType.UInt64, system, module, ordinal));

        // -- special operations ------------------------------------------------
        public uint Resolve(string module, int ordinal)
        {
            string hexmod = Memory.ToHex(Encoding.ASCII.GetBytes(module)).ToUpperInvariant();
            string body = $"consolefeatures ver={VERSION} type={OP_RESOLVE} params=\"A\\0\\A\\2\\"
                        + $"{JrpcType.ByteArray}/{module.Length}\\{hexmod}\\{JrpcType.Int}\\{ordinal}\\\"";
            return ParseHex(Send(body));
        }

        public string CpuKey()
            => Send($"consolefeatures ver={VERSION} type={OP_CPUKEY} params=\"A\\0\\A\\0\\\"");

        public string KernelVersion()
            => Send($"consolefeatures ver={VERSION} type={OP_KERNELVERSION} params=\"A\\0\\A\\0\\\"");

        public uint TitleId()
            => ParseHex(Send($"consolefeatures ver={VERSION} type={OP_TITLEID} params=\"A\\0\\A\\0\\\""));

        public string ConsoleType()
            => Send($"consolefeatures ver={VERSION} type={OP_CONSOLETYPE} params=\"A\\0\\A\\0\\\"");

        public uint Temperature(int kind = 0)
            => ParseHex(Send($"consolefeatures ver={VERSION} type={OP_TEMPERATURE} params=\"A\\0\\A\\1\\{JrpcType.Int}\\{kind}\\\""));

        public void SetLeds(int tl, int tr, int bl, int br)
            => Send($"consolefeatures ver={VERSION} type={OP_SETLEDS} params=\"A\\0\\A\\4\\"
                  + $"{JrpcType.Int}\\{tl}\\{JrpcType.Int}\\{tr}\\{JrpcType.Int}\\{bl}\\{JrpcType.Int}\\{br}\\\"");

        public void Notify(string text, int kind = 34)
        {
            string hex = Memory.ToHex(Encoding.ASCII.GetBytes(text)).ToUpperInvariant();
            Send($"consolefeatures ver={VERSION} type={OP_XNOTIFY} params=\"A\\0\\A\\2\\"
               + $"{JrpcType.ByteArray}/{text.Length}\\{hex}\\{JrpcType.Int}\\{kind}\\\"");
        }

        public void Shutdown()
        {
            try { Send($"consolefeatures ver={VERSION} type={OP_SHUTDOWN} params=\"A\\0\\A\\0\\\""); }
            catch (XbdmException) { }
        }

        // jrpc2's persistent poke: keep writing value at address while a title runs.
        public void ConstantMemory(uint address, uint value, uint? ifValue = null, uint? titleId = null)
        {
            int useIf = ifValue.HasValue ? 1 : 0, useTid = titleId.HasValue ? 1 : 0;
            Send($"consolefeatures ver={VERSION} type={OP_CONSTMEM} params=\"A\\{address:X}\\A\\5\\"
               + $"{JrpcType.Int}\\{value}\\{JrpcType.Int}\\{useIf}\\{JrpcType.Int}\\{ifValue ?? 0}\\"
               + $"{JrpcType.Int}\\{useTid}\\{JrpcType.Int}\\{titleId ?? 0}\\\"");
        }

        // -- probe -------------------------------------------------------------
        public bool Available()
        {
            try { TitleId(); return true; }
            catch (Exception) { return false; }
        }

        internal static uint ParseHex(string s) => uint.Parse(s.Trim(), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        internal static ulong ParseHex64(string s) => ulong.Parse(s.Trim(), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
    }
}
