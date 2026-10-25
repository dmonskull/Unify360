// SPDX-License-Identifier: GPL-3.0-or-later
// live smoke test of the X360 .NET library against a freeboot console running Unify360.
// runs on macOS via `dotnet run`. pass the console IP as arg1 (default 192.168.40.189).
using System;
using X360;
using X360.Compat;

class Program
{
    static int Main(string[] args)
    {
        string host = args.Length > 0 ? args[0] : "192.168.40.189";
        Console.WriteLine($"== X360 .NET library live test against {host} ==\n");
        try
        {
            using var xb = new Xbox(host, timeout: 15.0).Connect();

            Console.WriteLine("[xbdm]");
            Console.WriteLine($"  name        = {xb.Xbdm.Name()}");
            Console.WriteLine($"  consoletype = {xb.Xbdm.ConsoleType()}");
            bool hasUnify = false;
            foreach (var m in xb.Xbdm.Modules()) if (m.Name == "Unify360.xex") hasUnify = true;
            Console.WriteLine($"  Unify360 loaded = {hasUnify}");

            Console.WriteLine("\n[backends]");
            var backends = xb.Backends(refresh: true);
            Console.WriteLine($"  {string.Join(", ", backends)}");

            Console.WriteLine("\n[memory]");
            uint pbase = xb.Xbdm.ModuleBase("Unify360.xex");
            Console.WriteLine($"  Unify360 base = 0x{pbase:X8}");
            Console.WriteLine($"  read 2 bytes  = {BitConverter.ToString(xb.Mem.Read(pbase, 2))}  (expect 4D-5A 'MZ')");

            Console.WriteLine("\n[jrpc]");
            Console.WriteLine($"  resolve xboxkrnl!1 = 0x{xb.Jrpc.Resolve("xboxkrnl.exe", 1):X8}");
            Console.WriteLine($"  title id           = 0x{xb.Jrpc.TitleId():X8}");

            Console.WriteLine("\n[xdrpc]");
            Console.WriteLine($"  call xam!463 title = 0x{xb.Xdrpc.CallInt(0, null, true, "xam.xex", 463):X8}");

            Console.WriteLine("\n[xrpc]");
            uint buf = xb.UsePluginXrpc();
            Console.WriteLine($"  plugin buffer      = 0x{buf:X8} (via UNIFY001 status block)");
            Console.WriteLine($"  resolve xboxkrnl!1 = 0x{xb.Xrpc.Resolve("xboxkrnl.exe", 1):X8}");
            // stock path: fresh Xrpc using ONLY the classic pointer (served by our mem-hook)
            var stock = new Xrpc(xb.Mem);
            Console.WriteLine($"  classic-pointer available() = {stock.Available()}");
            Console.WriteLine($"  classic-pointer resolve     = 0x{stock.Resolve("xboxkrnl.exe", 1):X8}");

            Console.WriteLine("\n[unified Call()]");
            Console.WriteLine($"  Call(module xam!463) = 0x{Convert.ToUInt64(xb.Call(module: "xam.xex", ordinal: 463, ret: "int")):X8}");

            Console.WriteLine("\n[compat shims]");
            var con = new XboxConsole(host);
            con.Connect();
            Console.WriteLine($"  XboxConsole.GetMemory(base,2) = {BitConverter.ToString(con.GetMemory(pbase, 2))}");
            Console.WriteLine($"  JRPC.Call<uint>(xam,463)      = 0x{JRPC.Call<uint>(con, "xam.xex", 463):X8}");
            Console.WriteLine($"  JRPC.ResolveFunction(krnl,1)  = 0x{JRPC.ResolveFunction(con, "xboxkrnl.exe", 1):X8}");
            Console.WriteLine($"  XRPC.Resolve(krnl,1)          = 0x{XRPC.Resolve(con, "xboxkrnl.exe", 1):X8}");
            con.Disconnect();

            Console.WriteLine("\n== ALL CHECKS PASSED ==");
            return 0;
        }
        catch (Exception e)
        {
            Console.WriteLine($"\n!! FAILED: {e.GetType().Name}: {e.Message}");
            Console.WriteLine(e.StackTrace);
            return 1;
        }
    }
}
