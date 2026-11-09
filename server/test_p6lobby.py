#!/usr/bin/env python3
"""Tests of p6lobby.py over real UDP on localhost:  python3 server/test_p6lobby.py"""

import asyncio
import logging
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import p6lobby  # noqa: E402

PORT = 27799


class Phone:
    def __init__(self, mac, name, proto="P6N3", patch="original"):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(("127.0.0.1", 0))
        self.s.setblocking(False)
        self.mac, self.name, self.proto, self.patch = mac, name, proto, patch
        self.token = ""

    def send(self, kind, **kv):
        if self.token and "token" not in kv:
            kv["token"] = self.token
        body = "\n".join("%s=%s" % i for i in kv.items()).encode()
        self.s.sendto(p6lobby.MAGIC + bytes([kind]) + body, ("127.0.0.1", PORT))

    def raw(self, data):
        self.s.sendto(data, ("127.0.0.1", PORT))

    async def recv(self, want=None, timeout=1.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                d = self.s.recv(2048)
            except BlockingIOError:
                await asyncio.sleep(0.01)
                continue
            if d[:4] == p6lobby.MAGIC:
                kind = d[4]
                lines = d[5:].decode().split("\n")
                if want is None or kind == want:
                    return kind, lines
            elif want == "raw":
                return "raw", d
        return None, None

    async def hello(self):
        self.send(p6lobby.HELLO, mac=self.mac, name=self.name, proto=self.proto, app="t", patch=self.patch,
                  local="10.0.0.2:27600")
        k, lines = await self.recv(p6lobby.WELCOME)
        assert k == p6lobby.WELCOME, "no welcome"
        self.token = [x[6:] for x in lines if x.startswith("token=")][0]
        return lines


def a2_client(lobby, mac):
    return [c for c in lobby.by_addr.values() if c.mac == mac][0]


def kv(lines, key):
    return [x[len(key) + 1:] for x in lines if x.startswith(key + "=")]


async def run():
    lobby = p6lobby.Lobby(["Lobby 1"], "hola")
    loop = asyncio.get_running_loop()
    tr, _ = await loop.create_datagram_endpoint(lambda: lobby, local_addr=("127.0.0.1", PORT))
    ok = 0
    try:
        a, b, c = Phone("02503600000a", "ANA"), Phone("02503600000b", "BETO"), Phone("02503600000c", "CATA", "P6N6")
        w = await a.hello()
        assert kv(w, "motd") == ["hola"]
        await b.hello()
        await c.hello()
        ok += 1

        a.send(p6lobby.LIST)
        k, rooms = await a.recv(p6lobby.ROOMS)
        assert k and len(kv(rooms, "room")) == 1 and kv(rooms, "room")[0].split("|")[1] == "Lobby 1", rooms
        lobby_id = kv(rooms, "room")[0].split("|")[0]
        ok += 1

        # a forged request without the token gets nothing
        a.send(p6lobby.LIST, token="bad")
        k, _ = await a.recv(p6lobby.ROOMS, 0.2)
        assert k is None
        ok += 1

        a.send(p6lobby.JOIN, id=lobby_id)
        k, room = await a.recv(p6lobby.ROOM)
        assert kv(room, "id") == [lobby_id] and len(kv(room, "member")) == 1
        b.send(p6lobby.JOIN, id=lobby_id)
        k, room_b = await b.recv(p6lobby.ROOM)
        k, room_a = await a.recv(p6lobby.ROOM)
        mem = [m.split("|") for m in kv(room_a, "member")]
        assert [m[1] for m in mem] == ["ANA", "BETO"], mem
        assert mem[1][2] == "127.0.0.1:%d" % b.s.getsockname()[1] and mem[1][3] == "10.0.0.2:27600"
        assert mem[0][6] == "1" and mem[1][6] == "0"           # "you"
        ok += 1

        # a 60 Hz phone cannot join a 30 Hz room; its own list has its own lobby
        c.send(p6lobby.JOIN, id=lobby_id)
        k, err = await c.recv(p6lobby.ERROR)
        assert kv(err, "msg") == ["rate"], err
        c.send(p6lobby.LIST)
        k, rooms_c = await c.recv(p6lobby.ROOMS)
        assert kv(rooms_c, "room")[0].split("|")[0] != lobby_id and kv(rooms_c, "room")[0].split("|")[2] == "0"
        ok += 1

        # no relay until asked
        a.raw(b"P6N3\x01" + bytes.fromhex(a.mac) + b"GROUP000")
        k, _ = await b.recv("raw", 0.2)
        assert k is None
        a.send(p6lobby.RELAY, mac=b.mac)
        k, room_a = await a.recv(p6lobby.ROOM)
        assert [m.split("|")[4] for m in kv(room_a, "member")] == ["0", "1"], room_a
        await b.recv(p6lobby.ROOM)
        hello = b"P6N3\x01" + bytes.fromhex(a.mac) + b"GROUP000"
        a.raw(hello)
        k, d = await b.recv("raw")
        assert d == hello
        pdp = b"P6N3\x02" + bytes.fromhex(b.mac) + bytes.fromhex(a.mac) + b"\x00" * 8
        b.raw(pdp)
        k, d = await a.recv("raw")
        assert d == pdp
        pdp_to_c = b"P6N3\x02" + bytes.fromhex(b.mac) + bytes.fromhex(c.mac) + b"\x00" * 8
        b.raw(pdp_to_c)
        k, _ = await c.recv("raw", 0.2)
        assert k is None
        ok += 1

        # private room with a password
        c.send(p6lobby.CREATE, name="Mi sala", max="2", **{"pass": "x1"})
        k, room_c = await c.recv(p6lobby.ROOM)
        rid = kv(room_c, "id")[0]
        c2 = Phone("02503600000d", "DANI", "P6N6")
        await c2.hello()
        c2.send(p6lobby.JOIN, id=rid, **{"pass": "no"})
        k, err = await c2.recv(p6lobby.ERROR)
        assert kv(err, "msg") == ["password"]
        c2.send(p6lobby.JOIN, id=rid, **{"pass": "x1"})
        k, room = await c2.recv(p6lobby.ROOM)
        assert len(kv(room, "member")) == 2
        ok += 1

        # a player's room left empty stays EMPTY_ROOM_S, then goes; BYE drops the phone
        c.send(p6lobby.LEAVE)
        c2.send(p6lobby.BYE)
        await asyncio.sleep(0.1)
        assert rid in lobby.rooms and len(lobby.by_addr) == 3        # empty, kept for a while
        c.send(p6lobby.LIST)
        k, rooms = await c.recv(p6lobby.ROOMS)
        assert any(x.split("|")[0] == rid and x.split("|")[2] == "0" for x in kv(rooms, "room")), rooms
        lobby.expire_rooms(time.monotonic() + p6lobby.EMPTY_ROOM_S - 5)
        assert rid in lobby.rooms
        lobby.expire_rooms(time.monotonic() + p6lobby.EMPTY_ROOM_S + 1)
        assert rid not in lobby.rooms
        ok += 1

        # the same phone from a new address keeps its room (a 4G phone whose NAT moved)
        a2 = Phone(a.mac, "ANA")
        a2.token = a.token
        a2.send(p6lobby.HELLO, mac=a.mac, name="ANA", proto="P6N3", app="t", patch="original", local="", token=a.token)
        k, _ = await a2.recv(p6lobby.WELCOME)
        k, room = await b.recv(p6lobby.ROOM)
        mem = [m.split("|") for m in kv(room, "member")]
        assert mem[0][2] == "127.0.0.1:%d" % a2.s.getsockname()[1], mem
        ok += 1

        # HELLO answered at most HELLOS_PER_S a second: a forged source gets little back
        h = Phone("02503600001f", "FLOOD")
        await h.hello()
        for _ in range(10):
            h.send(p6lobby.HELLO, mac=h.mac, name="FLOOD", proto="P6N3", app="t", patch="original", local="")
        got = 0
        while (await h.recv(p6lobby.WELCOME, timeout=0.3))[0]:
            got += 1
        assert got <= p6lobby.HELLOS_PER_S, got
        ok += 1

        # at most MAX_PER_IP phones from one IP (all of these are 127.0.0.1)
        many = [Phone("0250360002%02x" % i, "P%d" % i) for i in range(p6lobby.MAX_PER_IP + 2)]
        answered = 0
        for p in many:
            p.send(p6lobby.HELLO, mac=p.mac, name=p.name, proto="P6N3", app="t", patch="original", local="")
            if (await p.recv(p6lobby.WELCOME, timeout=0.3))[0]:
                answered += 1
        assert answered < len(many) and lobby.per_ip["127.0.0.1"] == p6lobby.MAX_PER_IP, (answered, lobby.per_ip)
        many[0].send(p6lobby.BYE, token="x")                       # a BYE without the token does nothing
        await asyncio.sleep(0.1)
        assert lobby.per_ip["127.0.0.1"] == p6lobby.MAX_PER_IP
        ok += 1
        # a match: both in the game's match at once; logged with its path, and its end
        seen = []
        h = logging.Handler()
        h.emit = lambda rec: seen.append(rec.getMessage())
        p6lobby.log.addHandler(h)
        p6lobby.log.setLevel(logging.INFO)
        p6lobby.log.propagate = False
        a2.send(p6lobby.STATE, busy="2")
        b.send(p6lobby.STATE, busy="2")
        await asyncio.sleep(0.1)
        start = [x for x in seen if x.startswith("ev match_start")]
        assert len(start) == 1 and "path=relay" in start[0] and "an=ANA" in start[0], seen
        b.send(p6lobby.STATE, busy="0")
        await asyncio.sleep(0.1)
        end = [x for x in seen if x.startswith("ev match_end")]
        assert len(end) == 1 and "'lost the other phone'" in end[0], seen
        assert a2_client(lobby, a.mac).match is None
        p6lobby.log.removeHandler(h)
        p6lobby.log.propagate = True
        ok += 1
        print("p6lobby: %d checks passed" % ok)
    finally:
        tr.close()


if __name__ == "__main__":
    asyncio.run(run())
