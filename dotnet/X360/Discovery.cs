// SPDX-License-Identifier: GPL-3.0-or-later
// find consoles on the lan: the xbdm NAP name protocol (udp 731) and a tcp sweep of 730.
using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Threading.Tasks;

namespace X360
{
    public static class Discovery
    {
        private const int NapPort = 731, XbdmPort = 730;
        private const byte NapLookup = 1, NapReply = 2, NapWildcard = 3;

        // broadcast a NAP wildcard; return (ip, name) of answering consoles.
        public static List<(string ip, string name)> DiscoverNap(double timeout = 1.0)
        {
            var found = new Dictionary<string, string>();
            using (var s = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp))
            {
                s.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.Broadcast, true);
                s.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
                s.Bind(new IPEndPoint(IPAddress.Any, 0));
                s.ReceiveTimeout = (int)(timeout * 1000);
                s.SendTo(new byte[] { NapWildcard, 0 }, new IPEndPoint(IPAddress.Broadcast, NapPort));
                var buf = new byte[512];
                while (true)
                {
                    EndPoint ep = new IPEndPoint(IPAddress.Any, 0);
                    int n;
                    try { n = s.ReceiveFrom(buf, ref ep); }
                    catch (SocketException) { break; }
                    if (n >= 2 && buf[0] == NapReply)
                    {
                        int nlen = buf[1];
                        string name = nlen > 0 ? Transport.Latin1.GetString(buf, 2, Math.Min(nlen, n - 2)) : "";
                        found[((IPEndPoint)ep).Address.ToString()] = name;
                    }
                }
            }
            var outl = new List<(string, string)>();
            foreach (var kv in found) outl.Add((kv.Key, kv.Value));
            outl.Sort((a, b) => string.CompareOrdinal(a.Item1, b.Item1));
            return outl;
        }

        // forward NAP lookup: debug name -> ip (first responder), or null.
        public static string ResolveName(string name, double timeout = 1.0)
        {
            using (var s = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp))
            {
                s.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.Broadcast, true);
                s.Bind(new IPEndPoint(IPAddress.Any, 0));
                s.ReceiveTimeout = (int)(timeout * 1000);
                var raw = Transport.Latin1.GetBytes(name);
                var pkt = new byte[2 + raw.Length];
                pkt[0] = NapLookup; pkt[1] = (byte)raw.Length; Array.Copy(raw, 0, pkt, 2, raw.Length);
                s.SendTo(pkt, new IPEndPoint(IPAddress.Broadcast, NapPort));
                var buf = new byte[512];
                EndPoint ep = new IPEndPoint(IPAddress.Any, 0);
                try { s.ReceiveFrom(buf, ref ep); return ((IPEndPoint)ep).Address.ToString(); }
                catch (SocketException) { return null; }
            }
        }

        // tcp scan prefix.1..254 for an open xbdm port. prefix like "192.168.40".
        public static List<string> ScanSubnet(string prefix, double timeout = 0.4)
        {
            var tasks = new List<Task<string>>();
            for (int i = 1; i < 255; i++)
            {
                string ip = $"{prefix}.{i}";
                tasks.Add(Task.Run(() =>
                {
                    try
                    {
                        using (var c = new TcpClient())
                        {
                            if (!c.ConnectAsync(ip, XbdmPort).Wait((int)(timeout * 1000))) return null;
                            c.ReceiveTimeout = (int)(timeout * 1000);
                            var buf = new byte[32];
                            int n = c.GetStream().Read(buf, 0, 32);
                            string s = n > 0 ? Transport.Latin1.GetString(buf, 0, n) : "";
                            return s.StartsWith("201", StringComparison.Ordinal) ? ip : null;
                        }
                    }
                    catch (Exception) { return null; }
                }));
            }
            Task.WaitAll(tasks.ToArray());
            var outl = new List<string>();
            foreach (var t in tasks) if (t.Result != null) outl.Add(t.Result);
            return outl;
        }
    }
}
