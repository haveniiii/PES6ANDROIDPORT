#!/usr/bin/env python3
"""Checks a running lobby server from anywhere:  python3 server/probe.py host[:port]

Registers as a fake phone (HELLO -> WELCOME), lists the rooms (LIST -> ROOMS), says BYE, and
prints the round trip. Exit code 0 when the server answered.
"""

import socket
import sys
import time

MAGIC = b"P6L1"
HELLO, WELCOME, LIST, ROOMS, BYE = 1, 2, 3, 4, 15


def ask(s, kind, want, **kv):
    body = "\n".join("%s=%s" % i for i in kv.items()).encode()
    t0 = time.monotonic()
    s.send(MAGIC + bytes([kind]) + body)
    while True:
        d = s.recv(4096)
        if d[:4] == MAGIC and d[4] == want:
            return (time.monotonic() - t0) * 1000, d[5:].decode("utf-8", "replace").splitlines()


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    host, _, port = sys.argv[1].partition(":")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3.0)
    s.connect((host, int(port or 27700)))
    try:
        ms, lines = ask(s, HELLO, WELCOME, mac="02aabbccddee", name="probe",
                        proto="P6N3", app="probe", patch="original", local="0.0.0.0:0")
    except (socket.timeout, ConnectionRefusedError):
        sys.exit("no answer from %s: is the server running and UDP %s open in the firewall?"
                 % (sys.argv[1], port or 27700))
    kv = dict(l.split("=", 1) for l in lines if "=" in l)
    print("WELCOME in %.0f ms: the server sees this Mac at %s (server %s)" % (ms, kv.get("you"), kv.get("server")))
    ms, lines = ask(s, LIST, ROOMS, token=kv["token"])
    rooms = [l[5:].split("|")[1] for l in lines if l.startswith("room=")]
    print("ROOMS in %.0f ms: %s" % (ms, ", ".join(rooms) or "(none)"))
    s.send(MAGIC + bytes([BYE]) + ("token=" + kv["token"]).encode())


if __name__ == "__main__":
    main()
