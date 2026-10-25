"""find consoles on the lan. two ways: the xbdm NAP name protocol (udp 731, used by
xbox neighborhood) and a plain tcp sweep of port 730."""
import socket
import struct
from concurrent.futures import ThreadPoolExecutor

NAP_PORT = 731
XBDM_PORT = 730
NAP_WILDCARD = 3
NAP_REPLY = 2
NAP_LOOKUP = 1


def discover_nap(timeout=1.0):
    """broadcast a NAP wildcard; return [(ip, name), ...] of answering consoles."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 0))
    s.settimeout(timeout)
    found = {}
    try:
        s.sendto(struct.pack("BB", NAP_WILDCARD, 0), ("255.255.255.255", NAP_PORT))
        while True:
            try:
                data, addr = s.recvfrom(512)
            except socket.timeout:
                break
            if len(data) >= 2 and data[0] == NAP_REPLY:
                nlen = data[1]
                name = data[2:2 + nlen].decode("latin-1", "replace") if nlen else ""
                found[addr[0]] = name
    finally:
        s.close()
    return sorted(found.items())


def resolve_name(name, timeout=1.0):
    """forward NAP lookup: debug name -> ip (first responder)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind(("0.0.0.0", 0))
    s.settimeout(timeout)
    try:
        raw = name.encode("latin-1")
        s.sendto(struct.pack("BB", NAP_LOOKUP, len(raw)) + raw,
                 ("255.255.255.255", NAP_PORT))
        try:
            _, addr = s.recvfrom(512)
            return addr[0]
        except socket.timeout:
            return None
    finally:
        s.close()


def scan_subnet(prefix, timeout=0.4, workers=128):
    """tcp scan prefix.1..254 for an open xbdm port. prefix like '192.168.40'."""
    def probe(i):
        ip = f"{prefix}.{i}"
        try:
            with socket.create_connection((ip, XBDM_PORT), timeout) as c:
                c.settimeout(timeout)
                try:
                    banner = c.recv(32)
                except socket.timeout:
                    banner = b""
                return ip if banner.startswith(b"201") else None
        except OSError:
            return None
    with ThreadPoolExecutor(max_workers=workers) as pool:
        return [ip for ip in pool.map(probe, range(1, 255)) if ip]
