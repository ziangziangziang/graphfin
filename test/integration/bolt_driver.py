"""Minimal Bolt v3/v4 client for live wire assertions.

No Bolt driver package is available in this environment, so this module
implements just enough of the protocol to HELLO/RUN/PULL against the test
server and unpack the responses generically:

  handshake  0x6060B017 + 4 big-endian versions, server picks one
  HELLO      struct 0x01 {user_agent, scheme, principal, credentials}
  RUN        struct 0x10 [statement, params, extra]
  PULL       struct 0x3F [{n}]
  framing    uint16 length chunks, zero-length terminator

PackStream values decode to Python None/bool/int/float/str/list/dict, and
structs (temporal values, graph entities) to ("struct", tag, [fields]).
Only what the tests need is encoded client-side: null/int/str/list/map and
message structs.
"""

import socket
import struct

NULL = 0xC0
FALSE = 0xC2
TRUE = 0xC3
FLOAT = 0xC1
INT8 = 0xC8
INT16 = 0xC9
INT32 = 0xCA
INT64 = 0xCB
STR8 = 0xD0
STR16 = 0xD1
STR32 = 0xD2
LIST8 = 0xD4
LIST16 = 0xD5
LIST32 = 0xD6
MAP8 = 0xD8
MAP16 = 0xD9
MAP32 = 0xDA
STRUCT8 = 0xDC
STRUCT16 = 0xDD

RUN = 0x10
PULL = 0x3F
HELLO = 0x01
RESET = 0x0F
RECORD = 0x71
SUCCESS = 0x70
FAILURE = 0x7F
IGNORED = 0x7E


class BoltError(RuntimeError):
    pass


def _pack_int(n, out):
    if -16 <= n <= 127:
        out.append(n & 0xFF)
    elif -128 <= n <= 127:
        out += bytes([INT8, n & 0xFF])
    elif -32768 <= n <= 32767:
        out += bytes([INT16]) + struct.pack(">h", n)
    elif -2147483648 <= n <= 2147483647:
        out += bytes([INT32]) + struct.pack(">i", n)
    else:
        out += bytes([INT64]) + struct.pack(">q", n)


def _pack_str(s, out):
    b = s.encode("utf-8")
    if len(b) <= 15:
        out.append(0x80 | len(b))
    elif len(b) <= 255:
        out.append(STR8)
        out.append(len(b))
    elif len(b) <= 65535:
        out += bytes([STR16]) + struct.pack(">H", len(b))
    else:
        out += bytes([STR32]) + struct.pack(">I", len(b))
    out += b


def pack_value(v, out):
    if v is None:
        out.append(NULL)
    elif isinstance(v, bool):
        out.append(TRUE if v else FALSE)
    elif isinstance(v, int):
        _pack_int(v, out)
    elif isinstance(v, float):
        out.append(FLOAT)
        out += struct.pack(">d", v)
    elif isinstance(v, str):
        _pack_str(v, out)
    elif isinstance(v, (list, tuple)):
        if len(v) <= 15:
            out.append(0x90 | len(v))
        elif len(v) <= 255:
            out += bytes([LIST8, len(v)])
        elif len(v) <= 65535:
            out += bytes([LIST16]) + struct.pack(">H", len(v))
        else:
            out += bytes([LIST32]) + struct.pack(">I", len(v))
        for item in v:
            pack_value(item, out)
    elif isinstance(v, dict):
        if len(v) <= 15:
            out.append(0xA0 | len(v))
        elif len(v) <= 255:
            out += bytes([MAP8, len(v)])
        elif len(v) <= 65535:
            out += bytes([MAP16]) + struct.pack(">H", len(v))
        else:
            out += bytes([MAP32]) + struct.pack(">I", len(v))
        for k, item in v.items():
            _pack_str(k, out)
            pack_value(item, out)
    else:
        raise BoltError("cannot pack %r" % type(v))


def pack_struct(tag, fields):
    out = bytearray()
    if len(fields) <= 15:
        out.append(0xB0 | len(fields))
    elif len(fields) <= 255:
        out += bytes([STRUCT8, len(fields)])
    else:
        out += bytes([STRUCT16]) + struct.pack(">H", len(fields))
    out.append(tag)
    for f in fields:
        pack_value(f, out)
    return bytes(out)


class Unpacker:
    def __init__(self, buf):
        self.buf = buf
        self.pos = 0

    def take(self, n):
        if self.pos + n > len(self.buf):
            raise BoltError("truncated packstream")
        out = self.buf[self.pos:self.pos + n]
        self.pos += n
        return out

    def value(self):
        marker = self.take(1)[0]
        if marker <= 0x7F:
            return marker if marker < 0x80 else marker - 0x100
        if marker >= 0xF0:
            return marker - 0x100
        if 0x80 <= marker <= 0x8F:
            return self.take(marker & 0x0F).decode("utf-8")
        if 0x90 <= marker <= 0x9F:
            return [self.value() for _ in range(marker & 0x0F)]
        if 0xA0 <= marker <= 0xAF:
            # Decode key then value into locals: dict-display evaluation
            # order is not a sequencing guarantee to rely on here.
            out = {}
            for _ in range(marker & 0x0F):
                k = self.value()
                out[k] = self.value()
            return out
        if 0xB0 <= marker <= 0xBF:
            n = marker & 0x0F
            tag = self.take(1)[0]
            return ("struct", tag, [self.value() for _ in range(n)])
        if marker == NULL:
            return None
        if marker == FALSE:
            return False
        if marker == TRUE:
            return True
        if marker == FLOAT:
            return struct.unpack(">d", self.take(8))[0]
        if marker == INT8:
            return struct.unpack(">b", self.take(1))[0]
        if marker == INT16:
            return struct.unpack(">h", self.take(2))[0]
        if marker == INT32:
            return struct.unpack(">i", self.take(4))[0]
        if marker == INT64:
            return struct.unpack(">q", self.take(8))[0]
        if marker in (STR8, STR16, STR32):
            n = {STR8: 1, STR16: 2, STR32: 4}[marker]
            ln = int.from_bytes(self.take(n), "big")
            return self.take(ln).decode("utf-8")
        if marker in (LIST8, LIST16, LIST32):
            n = {LIST8: 1, LIST16: 2, LIST32: 4}[marker]
            ln = int.from_bytes(self.take(n), "big")
            return [self.value() for _ in range(ln)]
        if marker in (MAP8, MAP16, MAP32):
            n = {MAP8: 1, MAP16: 2, MAP32: 4}[marker]
            ln = int.from_bytes(self.take(n), "big")
            out = {}
            for _ in range(ln):
                k = self.value()
                out[k] = self.value()
            return out
        if marker in (STRUCT8, STRUCT16):
            n = {STRUCT8: 1, STRUCT16: 2}[marker]
            ln = int.from_bytes(self.take(n), "big")
            tag = self.take(1)[0]
            return ("struct", tag, [self.value() for _ in range(ln)])
        raise BoltError("unknown marker 0x%02x" % marker)


class BoltClient:
    """Blocking Bolt connection: handshake, HELLO, RUN/PULL per query."""

    def __init__(self, host, port, user, password, timeout=60):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.sock.sendall(b"\x60\x60\xb0\x17" + struct.pack(">IIII", 4, 3, 0, 0))
        picked = struct.unpack(">I", self._recvn(4))[0]
        if picked == 0:
            raise BoltError("server offered no Bolt version")
        hello = pack_struct(HELLO, [{
            "user_agent": "series-it/1.0", "scheme": "basic",
            "principal": user, "credentials": password}])
        self._send_message(hello)
        tag, fields = self._read_message()
        if tag != SUCCESS:
            raise BoltError("HELLO rejected: %r" % ([(tag, fields)],))

    def _recvn(self, n):
        out = bytearray()
        while len(out) < n:
            chunk = self.sock.recv(n - len(out))
            if not chunk:
                raise BoltError("connection closed")
            out += chunk
        return bytes(out)

    def _read_message(self):
        payload = bytearray()
        while True:
            ln = struct.unpack(">H", self._recvn(2))[0]
            if ln == 0:
                break
            payload += self._recvn(ln)
        u = Unpacker(bytes(payload))
        marker = u.take(1)[0]
        if 0xB0 <= marker <= 0xBF:
            nfields = marker & 0x0F
        elif marker == STRUCT8:
            nfields = u.take(1)[0]
        elif marker == STRUCT16:
            nfields = int.from_bytes(u.take(2), "big")
        else:
            raise BoltError("expected struct, got 0x%02x" % marker)
        tag = u.take(1)[0]
        return tag, [u.value() for _ in range(nfields)]

    def _send_message(self, msg):
        for i in range(0, len(msg), 0x7FFF):
            chunk = msg[i:i + 0x7FFF]
            self.sock.sendall(struct.pack(">H", len(chunk)) + chunk)
        self.sock.sendall(b"\x00\x00")

    def run(self, cypher, db="default", pull_n=1000):
        """RUN + PULL. Returns (fields, records, summary); RECORD rows are
        value lists."""
        self._send_message(pack_struct(RUN, [cypher, {}, {"db": db}]))
        self._send_message(pack_struct(PULL, [{"n": pull_n}]))
        fields, records, summary = None, [], None
        n_success = 0
        while True:
            tag, f = self._read_message()
            if tag == SUCCESS:
                n_success += 1
                if n_success == 1 and f and isinstance(f[0], dict):
                    fields = f[0].get("fields")
                else:
                    summary = f[0] if f else {}
                if n_success == 2:
                    break
            elif tag == RECORD:
                records.append(f[0])
            elif tag == FAILURE:
                raise BoltError("Bolt FAILURE: %r" % (f,))
            elif tag == IGNORED:
                break
        return fields, records, summary

    def reset(self):
        """RESET a FAILED session back to READY (required after any error)."""
        self._send_message(pack_struct(RESET, []))
        while True:
            tag, f = self._read_message()
            if tag == SUCCESS:
                return
            if tag == FAILURE:
                raise BoltError("Bolt RESET failed: %r" % (f,))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
