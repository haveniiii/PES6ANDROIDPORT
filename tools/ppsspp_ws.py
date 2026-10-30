#!/usr/bin/env python3
"""Minimal PPSSPP WebSocket debugger client.

Usage:
    python3 tools/ppsspp_ws.py [--port N] read <hexaddr> <size>
    python3 tools/ppsspp_ws.py [--port N] raw '<json>'

Default port 53766 (detected via lsof; PPSSPP binds an ephemeral port for the
remote debugger). Protocol: JSON text frames, e.g.
    {"event":"memory.read","address":0x089ECF88,"size":8}
    -> {"event":"memory.read","address":...,"size":...,"hex":"..."}
"""
import base64
import json
import os
import re
import socket
import struct
import sys
import time


def connect(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    req = ("GET /debugger HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
           "Origin: http://127.0.0.1\r\n\r\n" % (port, key))
    s.sendall(req.encode())
    resp = s.recv(4096)
    if b"101" not in resp.split(b"\r\n")[0]:
        raise RuntimeError("handshake failed: %r" % resp[:120])
    return s


def send_text(s, text):
    data = text.encode()
    hdr = bytearray([0x81])
    n = len(data)
    if n < 126:
        hdr.append(0x80 | n)
    elif n < 65536:
        hdr.append(0x80 | 126)
        hdr += struct.pack(">H", n)
    else:
        hdr.append(0x80 | 127)
        hdr += struct.pack(">Q", n)
    mask = os.urandom(4)
    hdr += mask
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    s.sendall(bytes(hdr) + masked)


def recv_frame(s):
    b1 = s.recv(1)
    if not b1:
        return None, None, b""
    fin = bool(b1[0] & 0x80)
    opcode = b1[0] & 0x0F
    b2 = s.recv(1)[0]
    ln = b2 & 0x7F
    if ln == 126:
        ln = struct.unpack(">H", s.recv(2))[0]
    elif ln == 127:
        ln = struct.unpack(">Q", s.recv(8))[0]
    payload = b""
    while len(payload) < ln:
        payload += s.recv(ln - len(payload))
    return opcode, fin, payload


def recv_message(s):
    """Assemble a full WebSocket message across continuation frames."""
    opcode, fin, data = recv_frame(s)
    if opcode is None:
        return None
    buf = data
    while not fin:
        opcode2, fin, data = recv_frame(s)
        if opcode2 is None:
            break
        buf += data
    return buf.decode("utf-8", "replace")


def recv_text(s):
    return recv_message(s)


def read_bytes(s, addr, size):
    send_text(s, json.dumps({"event": "memory.read", "address": addr, "size": size}))
    for _ in range(60):
        s.settimeout(3)
        try:
            msg = recv_message(s)
        except (socket.timeout, TimeoutError, OSError):
            return None
        if msg is None:
            return None
        m = re.search(r'"base64"\s*:\s*"([^"]*)"', msg, re.DOTALL)
        if m:
            return base64.b64decode("".join(m.group(1).split()))
    return None


def main():
    args = sys.argv[1:]
    port = 53766
    if args and args[0] == "--port":
        port = int(args[1])
        args = args[2:]
    if not args:
        print(__doc__)
        return 1

    if args[0] == "watch":
        addr = int(args[1], 16) if len(args) > 1 else 0x089ECF88
        nwords = int(args[2]) if len(args) > 2 else 4
        s = connect(port)
        last = None
        print("watching %s (%d words) ... (Ctrl-C to stop)" % (hex(addr), nwords))
        try:
            while True:
                try:
                    raw = read_bytes(s, addr, nwords * 4)
                except Exception:
                    raw = None
                if raw is None:
                    time.sleep(0.5)
                    try:
                        s.close()
                    except Exception:
                        pass
                    try:
                        s = connect(port)
                    except Exception:
                        time.sleep(1.0)
                    continue
                words = [struct.unpack_from("<I", raw, i)[0] for i in range(0, nwords * 4, 4)]
                # estados = palabras pares (state[0], state[1], ...); impares = frames
                states = tuple(words[i] for i in range(0, len(words), 2))
                if states != last:
                    frames = [words[i] for i in range(1, len(words), 2)]
                    print("states=%s frames=%s" % ([hex(x) for x in states], frames))
                    last = states
                time.sleep(0.2)
        except KeyboardInterrupt:
            pass
        s.close()
        return 0

    payload = None
    if args[0] == "read":
        addr = int(args[1], 16)
        size = int(args[2]) if len(args) > 2 else 4
        payload = {"event": "memory.read", "address": addr, "size": size}
    elif args[0] == "raw":
        payload = json.loads(args[1])
    else:
        print(__doc__)
        return 1

    s = connect(port)
    send_text(s, json.dumps(payload))
    # print a few messages (responses may stream)
    for _ in range(4):
        s.settimeout(3)
        try:
            msg = recv_text(s)
        except socket.timeout:
            break
        if msg is None:
            break
        m = re.search(r'"base64"\s*:\s*"([^"]*)"', msg, re.DOTALL)
        if m:
            raw = base64.b64decode("".join(m.group(1).split()))
            words = [struct.unpack_from("<I", raw, i)[0] for i in range(0, len(raw) - 3, 4)]
            print("hex=%s  u32=%s" % (raw.hex(), [hex(w) for w in words]))
            continue
        try:
            obj = json.loads(msg)
            print(json.dumps(obj))
        except ValueError:
            print(msg)
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
