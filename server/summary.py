#!/usr/bin/env python3
"""A summary of what happened on the lobby server, from its log (the "ev" lines of p6lobby.py).

    journalctl -u p6lobby --since today -o cat | python3 server/summary.py
    server/stats.sh root@<server> [since]       does the same from the Mac
"""

import collections
import re
import shlex
import statistics
import sys

LINE = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\S* \w+ (.*)$")
STATUS = re.compile(r"^(\d+) phones, (\d+) rooms")


def mmss(s):
    s = int(s)
    return "%d:%02d" % (s // 60, s % 60)


def main():
    phones = {}                                  # mac -> {names, kind, ip, first}
    joins = collections.Counter()
    refused = collections.Counter()
    drops = collections.Counter()
    matches = {}                                 # id -> dict
    moved = collections.Counter()
    peak = 0
    online = set()
    first = last = None
    for raw in sys.stdin:
        m = LINE.match(raw.rstrip("\n"))
        if not m:
            continue
        when, text = m.groups()
        first = first or when
        last = when
        st = STATUS.match(text)
        if st:
            peak = max(peak, int(st.group(1)))
            continue
        if not text.startswith("ev "):
            continue
        try:
            parts = shlex.split(text[3:])
        except ValueError:
            continue
        kind, kv = parts[0], dict(p.split("=", 1) for p in parts[1:] if "=" in p)
        mac = kv.get("mac", "")
        if kind == "hello":
            online.add(mac)
            peak = max(peak, len(online))
            p = phones.setdefault(mac, {"first": when, "names": [], "kind": "", "ip": set()})
            p["kind"] = "%s %s" % ({"P6N6": "60 fps"}.get(kv.get("proto"), "30 fps"), kv.get("patch", ""))
            p["ip"].add(kv.get("ip", "").rsplit(":", 1)[0])
            if kv.get("name") and kv["name"] not in p["names"]:
                p["names"].append(kv["name"])
        elif kind == "name" and mac in phones:
            if kv.get("name") not in phones[mac]["names"]:
                phones[mac]["names"].append(kv.get("name"))
        elif kind == "moved":
            moved[mac] += 1
        elif kind == "join":
            joins[kv.get("roomname", "?")] += 1
        elif kind == "refuse":
            refused[kv.get("why", "?")] += 1
        elif kind == "drop":
            online.discard(mac)
            drops[kv.get("why", "?")] += 1
        elif kind == "match_start":
            matches[kv["id"]] = dict(kv, when=when)
        elif kind == "match_end" and kv.get("id") in matches:
            matches[kv["id"]].update(dur=int(kv.get("dur", 0)), why=kv.get("why", ""), path=kv.get("path", ""),
                                     relayed_kb=int(kv.get("relayed_kb", 0)))

    if first is None:
        print("Nothing in the log.")
        return
    print("From %s to %s" % (first, last))
    print()
    print("Phones: %d (at most %d at once)" % (len(phones), peak))
    for mac, p in sorted(phones.items(), key=lambda i: i[1]["first"]):
        print("  %-24s %-20s %s%s" % (" / ".join(p["names"]) or "?", p["kind"], ", ".join(sorted(p["ip"])),
                                         "  (changed address %d×)" % moved[mac] if moved[mac] else ""))
    if joins:
        print()
        print("Rooms joined: " + ", ".join("%s %d" % i for i in joins.most_common()))

    print()
    done = [x for x in matches.values() if "dur" in x]
    print("Matches: %d (%d still going)" % (len(matches), len(matches) - len(done)))
    if matches:
        paths = collections.Counter(x.get("path", "?") for x in matches.values())
        print("  " + ", ".join("%s %d" % i for i in paths.most_common()))
    if done:
        print("  length: median %s, longest %s" % (mmss(statistics.median(x["dur"] for x in done)),
                                                   mmss(max(x["dur"] for x in done))))
        ends = collections.Counter(x["why"] for x in done)
        print("  ended: " + ", ".join("%s %d" % i for i in ends.most_common()))
        kb = sum(x["relayed_kb"] for x in done)
        if kb:
            print("  relayed by the server: %.1f MB" % (kb / 1000))
    for x in matches.values():
        print("  %s %s  %s vs %s  (%s, %s)  %s%s" % (
            x["when"][5:16], x["id"], x.get("an", "?"), x.get("bn", "?"), x.get("room", "?"), x.get("path", "?"),
            mmss(x["dur"]) if "dur" in x else "still going",
            ", " + x["why"] if x.get("why") else ""))

    if refused or drops:
        print()
    if refused:
        print("Refused: " + ", ".join("%s %d" % i for i in refused.most_common()))
    if drops:
        print("Left: " + ", ".join("%s %d" % i for i in drops.most_common()))


if __name__ == "__main__":
    main()
