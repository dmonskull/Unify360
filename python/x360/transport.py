"""xbdm line transport for the xbox 360 debug monitor (tcp port 730).

this is the one socket everything else rides on. it speaks the raw xbdm protocol:
crlf terminated commands, 3 digit reply codes, multiline (202), binary (203) and
send-binary (204) responses. no third party code, written from the protocol.
"""
import socket


class XbdmError(Exception):
    """a command came back with a 4xx code, or the stream broke."""

    def __init__(self, code, message=""):
        self.code = code
        self.message = message
        super().__init__(f"xbdm {code}: {message}" if code else message)


class Response:
    __slots__ = ("code", "message")

    def __init__(self, code, message):
        self.code = code
        self.message = message

    @property
    def ok(self):
        return 200 <= self.code < 300


# the handful of xbdm error codes worth naming; everything else stays numeric.
ERROR_NAMES = {
    400: "unexpected error",
    402: "file not found",
    403: "no such module",
    404: "memory not mapped",
    405: "no such thread",
    407: "unknown command",
    410: "file already exists",
    411: "directory not empty",
    416: "not debuggable",
}


class Transport:
    """a single xbdm connection. thread hostile on purpose; wrap with a lock if
    you share it. the facade (console.Xbox) does that for you."""

    RECV = 4096
    MAX_LINE = 64 * 1024

    def __init__(self, host, port=730, timeout=10.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None
        self._buf = b""

    # -- lifecycle ---------------------------------------------------------
    def connect(self):
        self.close()
        try:
            self.sock = socket.create_connection((self.host, self.port), self.timeout)
        except OSError as e:
            raise XbdmError(0, f"connect to {self.host}:{self.port} failed: {e}")
        self._buf = b""
        banner = self.recv_line()
        self.parse(banner, allow=(201,))
        return self

    def close(self):
        if self.sock is not None:
            try:
                self.sock.sendall(b"bye\r\n")
            except OSError:
                pass
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        self._buf = b""

    def __enter__(self):
        return self.connect()

    def __exit__(self, *exc):
        self.close()

    @property
    def connected(self):
        if self.sock is None:
            return False
        try:
            self.sock.setblocking(False)
            try:
                if self.sock.recv(1, socket.MSG_PEEK) == b"":
                    return False
            except BlockingIOError:
                pass
            except OSError:
                return False
            finally:
                self.sock.setblocking(True)
                self.sock.settimeout(self.timeout)
            return True
        except OSError:
            return False

    # -- raw io ------------------------------------------------------------
    def send_raw(self, data):
        if self.sock is None:
            raise XbdmError(0, "not connected")
        self.sock.sendall(data)

    def _fill(self):
        chunk = self.sock.recv(self.RECV)
        if not chunk:
            raise XbdmError(0, "connection closed")
        self._buf += chunk

    def recv_line(self):
        while True:
            nl = self._buf.find(b"\n")
            if nl >= 0:
                line, self._buf = self._buf[: nl + 1], self._buf[nl + 1 :]
                return line
            if len(self._buf) >= self.MAX_LINE:
                raise XbdmError(0, "response line too long")
            self._fill()

    def recv_exact(self, n):
        while len(self._buf) < n:
            self._fill()
        data, self._buf = self._buf[:n], self._buf[n:]
        return data

    # -- protocol ----------------------------------------------------------
    @staticmethod
    def parse(line, allow=None):
        text = line.rstrip(b"\r\n").decode("latin-1")
        if len(text) < 4 or not text[:3].isdigit() or text[3] != "-":
            raise XbdmError(0, f"malformed status line: {text!r}")
        code, message = int(text[:3]), text[4:]
        if code >= 400:
            raise XbdmError(code, message or ERROR_NAMES.get(code, ""))
        if allow is not None and code not in allow:
            # not an error, just not what the caller expected; let them see it
            pass
        return Response(code, message)

    def command(self, line):
        """send one command, return the single status Response. raises on 4xx."""
        self.send_raw(line.encode("latin-1") + b"\r\n")
        return self.parse(self.recv_line())

    def command_lines(self, line):
        """send a command expecting a 202 multiline body; return the body lines."""
        resp = self.command(line)
        if resp.code != 202:
            return resp, []
        out = []
        total = 0
        while True:
            raw = self.recv_line()
            total += len(raw)
            if total > 8 * 1024 * 1024 or len(out) > 200000:
                raise XbdmError(0, "multiline response too large")
            s = raw.rstrip(b"\r\n").decode("latin-1")
            if s == ".":
                break
            out.append(s)
        return resp, out
