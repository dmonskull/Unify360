// SPDX-License-Identifier: GPL-3.0-or-later
// drop-in compatibility surface. old tools are written against XDevkit's IXboxConsole
// (GetMemory/SetMemory/SendTextCommand) and the community JRPC / XRPC C# helpers. these
// thin shims expose those shapes over our plain-socket Xbox, so porting a tool is a
// one-line change (swap the namespace / console type). no Microsoft xbdm.dll needed.
using System;
using System.Globalization;

namespace X360.Compat
{
    // stand-in for XDevkit's IXboxConsole, the object every old tool is written against.
    public sealed class XboxConsole
    {
        public Xbox Box { get; private set; }
        public string IP { get; }

        public XboxConsole(string ip) { IP = ip; }

        public void Connect() { Box = new Xbox(IP).Connect(); }
        public void Disconnect() { Box?.Close(); Box = null; }

        public byte[] GetMemory(uint address, uint length) => Box.Mem.Read(address, (int)length);

        // XDevkit-style out-param overload: fills buffer, reports bytes read.
        public void GetMemory(uint address, uint length, byte[] buffer, out uint bytesRead)
        {
            var data = Box.Mem.Read(address, (int)length);
            Array.Copy(data, buffer, data.Length);
            bytesRead = (uint)data.Length;
        }

        public void SetMemory(uint address, byte[] data) => Box.Mem.Write(address, data);

        public void SetMemory(uint address, uint length, byte[] data, out uint bytesWritten)
        {
            bytesWritten = (uint)Box.Mem.Write(address, data);
        }

        public string SendTextCommand(string command) => Box.Tp.Command(command).Message;

        public uint ResolveFunction(string module, int ordinal) => Box.Resolve(module, ordinal);
    }

    // the community JRPC C# helper surface: JRPC.Call<T>(console, module, ord, args...).
    public static class JRPC
    {
        public static T Call<T>(XboxConsole console, string module, int ordinal, params object[] args)
            => Convert<T>(console.Box.Call(0, args, RetFor(typeof(T)), true, module, ordinal));

        public static T Call<T>(XboxConsole console, uint address, params object[] args)
            => Convert<T>(console.Box.Call(address, args, RetFor(typeof(T))));

        public static void CallVoid(XboxConsole console, string module, int ordinal, params object[] args)
            => console.Box.Call(0, args, "void", true, module, ordinal);

        public static void CallVoid(XboxConsole console, uint address, params object[] args)
            => console.Box.Call(address, args, "void");

        public static uint ResolveFunction(XboxConsole console, string module, int ordinal)
            => console.Box.Resolve(module, ordinal);

        public static uint GetTitleID(XboxConsole console) => console.Box.TitleId();

        public static void XNotify(XboxConsole console, string text, int type = 34)
            => console.Box.Notify(text, type);

        private static string RetFor(Type t)
        {
            if (t == typeof(void)) return "void";
            if (t == typeof(uint) || t == typeof(int)) return "int";
            if (t == typeof(ulong) || t == typeof(long)) return "uint64";
            if (t == typeof(float) || t == typeof(double)) return "float";
            if (t == typeof(string)) return "string";
            return "int";
        }

        private static T Convert<T>(object v)
        {
            if (v == null) return default;
            if (typeof(T) == typeof(string)) return (T)(object)v.ToString();
            if (typeof(T) == typeof(float)) return (T)(object)System.Convert.ToSingle(v, CultureInfo.InvariantCulture);
            if (typeof(T) == typeof(double)) return (T)(object)System.Convert.ToDouble(v, CultureInfo.InvariantCulture);
            if (typeof(T) == typeof(uint)) return (T)(object)System.Convert.ToUInt32(v);
            if (typeof(T) == typeof(int)) return (T)(object)unchecked((int)System.Convert.ToUInt32(v));
            if (typeof(T) == typeof(ulong)) return (T)(object)System.Convert.ToUInt64(v);
            if (typeof(T) == typeof(long)) return (T)(object)unchecked((long)System.Convert.ToUInt64(v));
            return (T)v;
        }
    }

    // the community XRPC shared-buffer helper surface.
    public static class XRPC
    {
        public static uint Call(XboxConsole console, uint address, params object[] args)
            => console.Box.Xrpc.Call(address, args);

        public static uint Resolve(XboxConsole console, string module, int ordinal)
            => console.Box.Xrpc.Resolve(module, ordinal);
    }
}
