#!/usr/bin/env python3
"""PES6PORT -- the online lobby server (docs/41).

The phones play peer to peer: this server only keeps the rooms, tells the members of a room
where the others are (the address it sees them at, and the one they see themselves at), so they
can open a direct path through their NATs, and -- when two of them cannot -- passes their game
datagrams on itself (relay).

Everything is UDP on one port, the same socket the game uses on the phone (port 27600 there), so
that the address this server sees is the one the other phone has to send to.

    "P6L1" type(1) then UTF-8 "key=value" lines

  phone -> server
    1 HELLO   mac name proto app patch local [token]   every 10 s: register / keep alive
    3 LIST    token                                    -> 4 ROOMS
    5 CREATE  token name [max] [pass]                  -> 6 ROOM or 8 ERROR
    7 JOIN    token id [pass]                          -> 6 ROOM or 8 ERROR
    9 LEAVE   token                                    -> 6 ROOM (empty)
   11 RELAY   token mac                                the direct path to that member failed
   13 STATE   token busy                               1 in the game's WIRELESS lobby, 2 in a match
   15 BYE     token                                    leaving the lobby
  server -> phone
    2 WELCOME you token server motd
    4 ROOMS   room=id|name|members|max|locked ...      the rooms this phone can join
    6 ROOM    id name max, then member=mac|name|public|local|relay|busy|you ...   on every change
    8 ERROR   msg (the phone shows it as it comes)

  Anything starting with "P6N" (the phones' own protocol, runtime/common/net.c) from a member is
  relayed: to the member its destination MAC names (types 2 PDP and 4 REPORT) or to every member
  it is relayed with (the others), only between pairs that asked for it.

The token in WELCOME is random and must come back in every other request: a forged source
address never sees it, so the server cannot be used to bounce traffic at someone else.

Rooms are for one game data set (the patch, "original" by default) and one match rate (P6N3 at
30 Hz, P6N6 at 60): the phones of a room must be able to play each other. The permanent lobbies
(--lobby) exist for every such combination; players can make their own rooms too.

    python3 p6lobby.py [--port 27700] [--lobby "Lobby 1" --lobby "Lobby 2"] [--motd text]

What happens is logged as "ev <kind> key=value ..." lines (shell quoting): hello, name, moved, room, join,
leave, refuse, state, relay, match_start, match_end, drop. A match is two members of a room in the game's
match at once (STATE busy=2); it ends when one of them leaves it, loses the other (busy 0) or goes away.
server/summary.py reads them back from journalctl.
"""

import argparse
import asyncio
import logging
import os
import secrets
import shlex
import time
import zlib

MAGIC = b"P6L1"
HELLO, WELCOME, LIST, ROOMS, CREATE, ROOM, JOIN, ERROR, LEAVE, RELAY, STATE, BYE = 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 13, 15
SERVER_VERSION = "1"

CLIENT_TIMEOUT = 35.0          # no HELLO for this long: gone
EMPTY_ROOM_S = 180.0           # a player's room left empty stays this long: its maker can come back, a friend find it
ROOM_MAX = 8                   # members of a room (net.c has room for 16 peers)
MAX_CLIENTS = 4000
MAX_ROOMS = 500
RELAY_BYTES_PER_S = 80_000     # per client, ~640 kbit/s: a match sends ~230 at most (docs/15 §16c)
REQUESTS_PER_S = 20            # lobby requests per client (HELLO excluded)
HELLOS_PER_S = 2               # HELLOs answered per client: a forged source gets little back
MAX_PER_IP = 16                # phones behind one public IP (a house, a 4G CGNAT): a flood from one IP stops here
NEW_CLIENTS_PER_S = 50         # new registrations a second, server-wide: forged sources cannot fill it at once
MAX_DATAGRAM = 1400

log = logging.getLogger("p6lobby")


def parse(body):
    out = {}
    for line in body.decode("utf-8", "replace").split("\n"):
        k, sep, v = line.partition("=")
        if sep:
            out[k.strip()] = v.strip()
    return out


def clean(s, n):
    s = "".join(c for c in s if c.isprintable() and c not in "|=\n\r")
    return s.strip()[:n]


def ep(addr):
    return "%s:%d" % addr


def ev(kind, **kv):
    log.info("ev %s %s", kind, " ".join("%s=%s" % (k, shlex.quote(str(v))) for k, v in kv.items()))


class Client:
    def __init__(self, addr, mac):
        self.addr, self.mac = addr, mac
        self.token = secrets.token_hex(8)
        self.name, self.proto, self.app, self.patch, self.local = "", "", "", "original", ""
        self.room = None
        self.busy = 0
        self.seen = time.monotonic()
        self.relay_with = set()            # macs
        self.bucket, self.bucket_at = RELAY_BYTES_PER_S, time.monotonic()
        self.req, self.req_at = REQUESTS_PER_S, time.monotonic()
        self.hello, self.hello_at = HELLOS_PER_S, time.monotonic()
        self.relayed_bytes = 0
        self.match = None

    def allow_request(self, now):
        self.req = min(REQUESTS_PER_S, self.req + (now - self.req_at) * REQUESTS_PER_S)
        self.req_at = now
        if self.req < 1:
            return False
        self.req -= 1
        return True

    def allow_hello(self, now):
        self.hello = min(HELLOS_PER_S, self.hello + (now - self.hello_at) * HELLOS_PER_S)
        self.hello_at = now
        if self.hello < 1:
            return False
        self.hello -= 1
        return True

    def allow_relay(self, n, now):
        self.bucket = min(RELAY_BYTES_PER_S, self.bucket + (now - self.bucket_at) * RELAY_BYTES_PER_S)
        self.bucket_at = now
        if self.bucket < n:
            return False
        self.bucket -= n
        self.relayed_bytes += n
        return True


class Room:
    def __init__(self, rid, name, proto, patch, maxn, password, permanent):
        self.id, self.name, self.proto, self.patch = rid, name, proto, patch
        self.max, self.password, self.permanent = maxn, password, permanent
        self.members = []                  # Clients, in order of arrival
        self.empty_at = None               # when its last member left (players' rooms only)


class Match:
    def __init__(self, mid, a, b, room, path):
        self.id, self.a, self.b, self.room, self.path = mid, a, b, room, path
        self.start = time.monotonic()
        self.relay0 = a.relayed_bytes + b.relayed_bytes


class Lobby(asyncio.DatagramProtocol):
    def __init__(self, lobbies, motd):
        self.lobbies, self.motd = lobbies, motd
        self.by_addr = {}                  # addr -> Client
        self.rooms = {}                    # id -> Room
        self.next_room = 1
        self.transport = None
        self.stats = {"relayed": 0, "dropped": 0, "refused": 0}
        self.next_match = 1
        self.per_ip = {}                   # ip -> phones registered from it
        self.new_bucket, self.new_at = NEW_CLIENTS_PER_S, time.monotonic()

    # ---- plumbing --------------------------------------------------------------------------

    def connection_made(self, transport):
        self.transport = transport

    def send(self, addr, kind, lines):
        body = "\n".join(lines).encode("utf-8")
        self.transport.sendto(MAGIC + bytes([kind]) + body[:MAX_DATAGRAM], addr)

    def error(self, c, msg):
        ev("refuse", mac=c.mac, why=msg)
        self.send(c.addr, ERROR, ["msg=" + msg])

    def datagram_received(self, data, addr):
        try:
            if data[:3] == b"P6N" and len(data) >= 11:
                self.relay(data, addr)
            elif data[:4] == MAGIC and len(data) >= 5:
                self.request(data[4], parse(data[5:]), addr)
        except Exception:                  # one bad datagram never stops the server
            log.exception("datagram from %s", ep(addr))

    # ---- the lobby ---------------------------------------------------------------------------

    def request(self, kind, kv, addr):
        now = time.monotonic()
        c = self.by_addr.get(addr)
        if kind == HELLO:
            new = False
            mac = kv.get("mac", "").lower()
            if len(mac) != 12 or any(ch not in "0123456789abcdef" for ch in mac):
                return
            if c is None or c.mac != mac:
                if c is not None:
                    self.drop(c, "replaced")
                c = None
                for other in list(self.by_addr.values()):   # the same phone from a new address (4G)
                    if other.mac == mac and kv.get("token") == other.token:
                        ev("moved", mac=mac, ip=ep(addr))
                        del self.by_addr[other.addr]
                        self.forget_ip(other.addr)
                        self.per_ip[addr[0]] = self.per_ip.get(addr[0], 0) + 1
                        other.addr = addr
                        self.by_addr[addr] = c = other
                        if c.room:
                            self.broadcast_room(c.room)
                if c is None:
                    if not self.allow_new(addr, now):
                        self.stats["refused"] += 1
                        return
                    c = Client(addr, mac)
                    self.by_addr[addr] = c
                    self.per_ip[addr[0]] = self.per_ip.get(addr[0], 0) + 1
                    new = True
            elif kv.get("token") not in (None, "", c.token):
                return
            if not c.allow_hello(now):
                return
            c.seen = now
            changed = (c.name, c.proto, c.patch, c.local)
            c.name = clean(kv.get("name", ""), 24) or "PES6"
            c.proto = clean(kv.get("proto", ""), 8)
            c.app = clean(kv.get("app", ""), 24)
            c.patch = clean(kv.get("patch", ""), 64) or "original"
            c.local = clean(kv.get("local", ""), 64)
            if new:
                ev("hello", mac=c.mac, name=c.name, app=c.app, proto=c.proto, patch=c.patch, ip=ep(addr))
            elif changed[0] != c.name:
                ev("name", mac=c.mac, name=c.name)
            self.send(addr, WELCOME, ["you=" + ep(addr), "token=" + c.token, "server=" + SERVER_VERSION,
                                      "motd=" + self.motd])
            if c.room and changed != (c.name, c.proto, c.patch, c.local):
                self.broadcast_room(c.room)
            return
        if c is None or kv.get("token") != c.token or not c.allow_request(now):
            return
        c.seen = now
        if kind == LIST:
            self.send(addr, ROOMS, self.room_lines(c))
        elif kind == CREATE:
            self.create(c, kv)
        elif kind == JOIN:
            self.join(c, kv.get("id", ""), kv.get("pass", ""))
        elif kind == LEAVE:
            self.leave(c)
            self.send(addr, ROOM, ["id="])
        elif kind == RELAY:
            self.want_relay(c, kv.get("mac", "").lower())
        elif kind == STATE:
            busy = {"1": 1, "2": 2}.get(kv.get("busy"), 0)      # 1 in WIRELESS, 2 in a match
            if busy != c.busy:
                c.busy = busy
                ev("state", mac=c.mac, busy=busy)
                self.match_state(c)
                if c.room:
                    self.broadcast_room(c.room)
        elif kind == BYE:
            self.drop(c, "bye")

    def allow_new(self, addr, now):
        if len(self.by_addr) >= MAX_CLIENTS or self.per_ip.get(addr[0], 0) >= MAX_PER_IP:
            return False
        self.new_bucket = min(NEW_CLIENTS_PER_S, self.new_bucket + (now - self.new_at) * NEW_CLIENTS_PER_S)
        self.new_at = now
        if self.new_bucket < 1:
            return False
        self.new_bucket -= 1
        return True

    def room_lines(self, c):
        lines = []
        for name in self.lobbies:                # the permanent ones first, for this phone's kind
            r = self.rooms.get(self.lobby_id(name, c))
            lines.append("room=%s|%s|%d|%d|0" % (self.lobby_id(name, c), name, len(r.members) if r else 0,
                                                 ROOM_MAX))
        for r in self.rooms.values():
            if r.permanent or r.proto != c.proto or r.patch != c.patch:
                continue
            lines.append("room=%s|%s|%d|%d|%d" % (r.id, r.name, len(r.members), r.max, 1 if r.password else 0))
            if len(lines) >= 40:
                break
        return lines

    @staticmethod
    def lobby_id(name, c):
        return "L%d-%s-%s" % (zlib.crc32(name.encode()) % 100000, c.proto, c.patch)

    def create(self, c, kv):
        name = clean(kv.get("name", ""), 24)
        if not name:
            return self.error(c, "name")
        if len(self.rooms) >= MAX_ROOMS:
            return self.error(c, "full")
        try:
            maxn = max(2, min(ROOM_MAX, int(kv.get("max", ROOM_MAX))))
        except ValueError:
            maxn = ROOM_MAX
        rid = "R%d" % self.next_room
        self.next_room += 1
        self.rooms[rid] = Room(rid, name, c.proto, c.patch, maxn, clean(kv.get("pass", ""), 24), False)
        ev("room", id=rid, name=name, by=c.mac, locked=1 if self.rooms[rid].password else 0)
        self.join(c, rid, kv.get("pass", ""))

    def join(self, c, rid, password):
        r = self.rooms.get(rid)
        if r is None:
            for name in self.lobbies:            # a permanent lobby is made when first joined
                if rid == self.lobby_id(name, c):
                    r = self.rooms[rid] = Room(rid, name, c.proto, c.patch, ROOM_MAX, "", True)
                    break
        if r is None:
            return self.error(c, "gone")
        if c.room is r:
            return self.broadcast_room(r)
        if r.proto != c.proto:
            return self.error(c, "rate")
        if r.patch != c.patch:
            return self.error(c, "patch")
        if r.password and r.password != clean(password, 24):
            return self.error(c, "password")
        if len(r.members) >= r.max:
            return self.error(c, "full")
        self.leave(c)
        c.room = r
        r.members.append(c)
        ev("join", mac=c.mac, name=c.name, room=r.id, roomname=r.name)
        self.broadcast_room(r)

    def leave(self, c):
        r = c.room
        if r is None:
            return
        if c.match:
            self.end_match(c.match, "left the room")
        ev("leave", mac=c.mac, room=r.id)
        c.room = None
        r.members.remove(c)
        for m in r.members:
            m.relay_with.discard(c.mac)
        c.relay_with.clear()
        if not r.members:
            if r.permanent:
                del self.rooms[r.id]             # made again when joined
            else:
                r.empty_at = time.monotonic()    # kept EMPTY_ROOM_S (expire_rooms)
        else:
            self.broadcast_room(r)

    def want_relay(self, c, mac):
        if c.room is None:
            return
        for m in c.room.members:
            if m.mac == mac and m is not c:
                if mac not in c.relay_with:
                    mt = c.match if c.match and m in (c.match.a, c.match.b) else None
                    if mt and mt.path == "direct":
                        mt.path = "direct, then relay"
                    ev("relay", a=c.mac, b=m.mac, room=c.room.id, match=mt.id if mt else "")
                c.relay_with.add(m.mac)
                m.relay_with.add(c.mac)
                self.broadcast_room(c.room)
                return

    def broadcast_room(self, r):
        for m in r.members:
            lines = ["id=" + r.id, "name=" + r.name, "max=%d" % r.max]
            for o in r.members:
                lines.append("member=%s|%s|%s|%s|%d|%d|%d" % (o.mac, o.name, ep(o.addr), o.local,
                                                              1 if o.mac in m.relay_with else 0, o.busy,
                                                              1 if o is m else 0))
            self.send(m.addr, ROOM, lines)

    # ---- matches -----------------------------------------------------------------------------

    def match_state(self, c):
        if c.busy != 2:
            if c.match:
                self.end_match(c.match, "finished" if c.busy == 1 else "lost the other phone")
            return
        if c.match or c.room is None:
            return
        for m in c.room.members:
            if m is not c and m.busy == 2 and m.match is None:
                path = "relay" if m.mac in c.relay_with else "direct"
                mt = Match("M%d" % self.next_match, m, c, c.room, path)
                self.next_match += 1
                m.match = c.match = mt
                ev("match_start", id=mt.id, a=m.mac, b=c.mac, an=m.name, bn=c.name, room=c.room.name,
                   path=path, proto=c.proto, patch=c.patch)
                return

    def end_match(self, mt, why):
        relayed = (mt.a.relayed_bytes + mt.b.relayed_bytes - mt.relay0) // 1000
        ev("match_end", id=mt.id, dur=int(time.monotonic() - mt.start), why=why, path=mt.path, relayed_kb=relayed)
        mt.a.match = mt.b.match = None

    def drop(self, c, why):
        ev("drop", mac=c.mac, why=why)
        if c.match:
            self.end_match(c.match, "a phone went away (%s)" % why)
        self.leave(c)
        if self.by_addr.pop(c.addr, None) is not None:
            self.forget_ip(c.addr)

    def forget_ip(self, addr):
        n = self.per_ip.get(addr[0], 0) - 1
        if n > 0:
            self.per_ip[addr[0]] = n
        else:
            self.per_ip.pop(addr[0], None)

    # ---- relay -------------------------------------------------------------------------------

    def relay(self, data, addr):
        c = self.by_addr.get(addr)
        if c is None or c.room is None or not c.relay_with:
            return
        now = time.monotonic()
        if not c.allow_relay(len(data) + 28, now):
            self.stats["dropped"] += 1
            return
        kind = data[4]
        if kind in (2, 4) and len(data) >= 17:
            dst = data[11:17].hex()
            targets = [m for m in c.room.members if m.mac == dst and dst in c.relay_with]
        else:
            targets = [m for m in c.room.members if m.mac in c.relay_with]
        for m in targets:
            self.transport.sendto(data, m.addr)
            self.stats["relayed"] += 1

    # ---- housekeeping ------------------------------------------------------------------------

    async def sweep(self):
        while True:
            await asyncio.sleep(5)
            now = time.monotonic()
            for c in list(self.by_addr.values()):
                if now - c.seen > CLIENT_TIMEOUT:
                    self.drop(c, "timeout")
            self.expire_rooms(now)

    def expire_rooms(self, now):
        for r in list(self.rooms.values()):
            if not r.members and not r.permanent and r.empty_at is not None and now - r.empty_at >= EMPTY_ROOM_S:
                del self.rooms[r.id]
                ev("room_gone", id=r.id, name=r.name)

    async def report(self, every):
        while True:
            await asyncio.sleep(every)
            relays = sum(1 for c in self.by_addr.values() if c.relay_with) // 2
            matches = sum(1 for c in self.by_addr.values() if c.match) // 2
            log.info("%d phones, %d rooms, %d matches, %d relayed pairs, %d datagrams relayed, %d over the limit, "
                     "%d registrations refused", len(self.by_addr), len(self.rooms), matches, relays,
                     self.stats["relayed"], self.stats["dropped"], self.stats["refused"])


async def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default=os.environ.get("P6LOBBY_HOST", "0.0.0.0"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("P6LOBBY_PORT", "27700")))
    ap.add_argument("--lobby", action="append", help="a permanent lobby (repeat); default: Lobby 1..3")
    ap.add_argument("--motd", default=os.environ.get("P6LOBBY_MOTD", ""))
    ap.add_argument("--report", type=float, default=300.0, help="seconds between status lines")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    lobby = Lobby(a.lobby or ["Lobby 1", "Lobby 2", "Lobby 3"], clean(a.motd, 120))
    loop = asyncio.get_running_loop()
    transport, _ = await loop.create_datagram_endpoint(lambda: lobby, local_addr=(a.host, a.port))
    log.info("listening on udp %s:%d, lobbies %s", a.host, a.port, ", ".join(lobby.lobbies))
    try:
        await asyncio.gather(lobby.sweep(), lobby.report(a.report))
    finally:
        transport.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
