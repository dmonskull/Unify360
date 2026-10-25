// SPDX-License-Identifier: GPL-3.0-or-later
// xbdm line transport (tcp 730): crlf commands, 3-digit reply codes, 202 multiline,
// 203 binary, 204 send-binary. a faithful port of the python x360.transport.
using System;
using System.Collections.Generic;
using System.Net.Sockets;
using System.Text;
using System.Threading.Tasks;

namespace X360
{
    public class XbdmException : Exception
    {
        public int Code { get; }
        public XbdmException(int code, string message = "")
            : base(code != 0 ? $"xbdm {code}: {message}" : message)
        {
            Code = code;
        }
    }

    public sealed class Response
    {
        public int Code { get; }
        public string Message { get; }
        public Response(int code, string message) { Code = code; Message = message; }
        public bool Ok => Code >= 200 && Code < 300;
    }

    public sealed class Transport
    {
        private const int Recv = 4096;
        private const int MaxLine = 64 * 1024;

        // the xbdm error codes worth naming; everything else stays numeric.
        private static readonly Dictionary<int, string> ErrorNames = new Dictionary<int, string>
        {
            {400,"unexpected error"}, {402,"file not found"}, {403,"no such module"},
            {404,"memory not mapped"}, {405,"no such thread"}, {407,"unknown command"},
            {410,"file already exists"}, {411,"directory not empty"}, {416,"not debuggable"},
        };

        internal static readonly Encoding Latin1 = Encoding.GetEncoding(28591); // ISO-8859-1, 1:1 bytes

        public string Host { get; }
        public int Port { get; }
        public double Timeout { get; }

        private TcpClient _client;
        private NetworkStream _stream;
        private byte[] _buf = Array.Empty<byte>();
        private int _off;

        public Transport(string host, int port = 730, double timeout = 10.0)
        {
            Host = host; Port = port; Timeout = timeout;
        }

        // -- lifecycle ---------------------------------------------------------
        public Transport Connect()
        {
            Close();
            try
            {
                _client = new TcpClient();
                if (!_client.ConnectAsync(Host, Port).Wait((int)(Timeout * 1000)))
                    throw new XbdmException(0, $"connect to {Host}:{Port} timed out");
                _stream = _client.GetStream();
                int ms = (int)(Timeout * 1000);
                _stream.ReadTimeout = ms; _stream.WriteTimeout = ms;
            }
            catch (XbdmException) { throw; }
            catch (Exception e) { throw new XbdmException(0, $"connect to {Host}:{Port} failed: {e.Message}"); }
            _buf = Array.Empty<byte>(); _off = 0;
            Parse(RecvLine(), new[] { 201 });
            return this;
        }

        public void Close()
        {
            if (_client != null)
            {
                try { if (_stream != null) { var b = Latin1.GetBytes("bye\r\n"); _stream.Write(b, 0, b.Length); } }
                catch { }
                try { _stream?.Dispose(); } catch { }
                try { _client.Close(); } catch { }
                _client = null; _stream = null;
            }
            _buf = Array.Empty<byte>(); _off = 0;
        }

        // -- raw io ------------------------------------------------------------
        public void SendRaw(byte[] data)
        {
            if (_stream == null) throw new XbdmException(0, "not connected");
            _stream.Write(data, 0, data.Length);
        }

        public void SendRaw(string line) => SendRaw(Latin1.GetBytes(line));

        private void Fill()
        {
            var tmp = new byte[Recv];
            int n = _stream.Read(tmp, 0, Recv);
            if (n <= 0) throw new XbdmException(0, "connection closed");
            int avail = _buf.Length - _off;
            var nb = new byte[avail + n];
            Array.Copy(_buf, _off, nb, 0, avail);
            Array.Copy(tmp, 0, nb, avail, n);
            _buf = nb; _off = 0;
        }

        public byte[] RecvLine()
        {
            while (true)
            {
                for (int i = _off; i < _buf.Length; i++)
                {
                    if (_buf[i] == (byte)'\n')
                    {
                        int len = i + 1 - _off;
                        var line = new byte[len];
                        Array.Copy(_buf, _off, line, 0, len);
                        _off = i + 1;
                        return line;
                    }
                }
                if (_buf.Length - _off >= MaxLine) throw new XbdmException(0, "response line too long");
                Fill();
            }
        }

        public byte[] RecvExact(int n)
        {
            while (_buf.Length - _off < n) Fill();
            var data = new byte[n];
            Array.Copy(_buf, _off, data, 0, n);
            _off += n;
            return data;
        }

        // -- protocol ----------------------------------------------------------
        public static Response Parse(byte[] line, int[] allow = null)
        {
            string text = Latin1.GetString(line).TrimEnd('\r', '\n');
            if (text.Length < 4 || !char.IsDigit(text[0]) || !char.IsDigit(text[1])
                || !char.IsDigit(text[2]) || text[3] != '-')
                throw new XbdmException(0, $"malformed status line: {text}");
            int code = int.Parse(text.Substring(0, 3));
            string message = text.Substring(4);
            if (code >= 400)
                throw new XbdmException(code, message.Length > 0 ? message
                    : (ErrorNames.TryGetValue(code, out var nm) ? nm : ""));
            return new Response(code, message);
        }

        public Response Command(string line)
        {
            SendRaw(Latin1.GetBytes(line + "\r\n"));
            return Parse(RecvLine());
        }

        public (Response, List<string>) CommandLines(string line)
        {
            var resp = Command(line);
            var outl = new List<string>();
            if (resp.Code != 202) return (resp, outl);
            long total = 0;
            while (true)
            {
                var raw = RecvLine();
                total += raw.Length;
                if (total > 8L * 1024 * 1024 || outl.Count > 200000)
                    throw new XbdmException(0, "multiline response too large");
                string s = Latin1.GetString(raw).TrimEnd('\r', '\n');
                if (s == ".") break;
                outl.Add(s);
            }
            return (resp, outl);
        }
    }
}
