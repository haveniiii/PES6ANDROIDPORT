# PES6PORT online lobby server

`p6lobby.py` keeps the online rooms, introduces the phones of a room to each other, and relays
the pairs that can't open a direct path through their NATs. The match itself goes from phone to
phone. The design is in `docs/41-online.md`.

- **Requirements:** Python 3.8 or newer with the standard library only. One UDP port (27700).
- **Load:** an idle phone sends ~1 small datagram every 10 s.
- **Relay traffic:** a relayed match carries 100–250 kbit/s each way, which is about 0.1–0.2 GB
  per hour of play. It is capped at ~640 kbit/s per phone.

## Running it

```sh
python3 server/p6lobby.py --port 27700 --lobby "Chile 1" --lobby "Chile 2"
python3 server/test_p6lobby.py        # the server's own tests (localhost)
```

The `--lobby` rooms always exist, once for each game version and match rate. Players can create
rooms of their own too, optionally with a password. A player's room that is left empty stays 3 minutes
(`EMPTY_ROOM_S`), so its maker can come back and a friend can still find it.

## On a small VPS (Debian / Ubuntu)

From the Mac, with SSH access to the VPS by key:

```sh
server/deploy.sh root@<ip>            # installs or updates, starts it, then runs probe.py
python3 server/probe.py <ip>:27700    # checks it from anywhere: WELCOME and the room list
```

**A name instead of the IP.** The APK holds the server's address, so give it a name: then a new server only needs
the name pointed at it, and the APKs already out keep working. With a free DuckDNS name (duckdns.org), `deploy.sh`
points it at the server now and keeps it there every 15 minutes. The token stays on the server
(`/etc/p6lobby/duckdns`, root only), never in the repo:

```sh
DUCKDNS_DOMAIN=<name> DUCKDNS_TOKEN=<token> server/deploy.sh root@<ip>
```

**What happened.** The server logs each phone that comes in, the rooms it joins, every match (two members in the game's
match at once: direct or relayed, how long, how it ended) and what it refused:

```sh
server/stats.sh root@<ip>                 # a summary of today
server/stats.sh root@<ip> "2 hours ago"   # or since then
server/stats.sh root@<ip> -f              # follow the events live
```

The same by hand, on the VPS:

```sh
sudo useradd --system --no-create-home p6lobby
sudo mkdir -p /opt/p6lobby && sudo cp p6lobby.py /opt/p6lobby/
sudo cp p6lobby.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now p6lobby
sudo ufw allow 27700/udp              # and in the provider's firewall too
journalctl -u p6lobby -f              # one status line every 5 minutes
```

## Security

- **SSH by key only.** A new VPS lets root log in with a password, and bots try passwords within hours. Turn that off:
  ```sh
  printf 'PasswordAuthentication no\nKbdInteractiveAuthentication no\nPermitRootLogin prohibit-password\n' \
      > /etc/ssh/sshd_config.d/00-keys-only.conf && sshd -t && systemctl reload ssh
  ```
  The file starts with `00`, so it wins over the provider's `50-cloud-init.conf`.
- **Firewall:** only 22/tcp and 27700/udp (`ufw`). Keep `unattended-upgrades` on (Ubuntu has it on by default).
- **The service** runs as its own user with no capabilities, no writable disk and only UDP/IP sockets. Check it
  with `systemd-analyze security p6lobby`; it scores 1.3 ("OK").
- **What the server limits:**
  - Every request after the first HELLO needs the random token, so a forged source never gets past HELLO.
  - HELLOs get at most 2 answers a second per phone.
  - A public IP can hold at most 16 phones, and the whole server takes at most 50 new phones a second, so a flood
    can't fill it.
  - Lobby requests are capped at 20 a second per phone.
  - Relay runs only between members of the same room who asked for it, at ~640 kbit/s a phone.
- **What it can't hide:** the phones of a room learn each other's public and local address. That's how a direct
  P2P path opens, and the same is true of any P2P game. The server keeps nothing on disk; its log
  (`journalctl`) holds addresses and player names.

## The app

Build the app with the server's address built in, either a DNS name or an IP:

```sh
PES6_LOBBY_SERVER=lobby.example.com:27700 tools/android_build.sh iso
```

To test with an app built without it, use either of these:

- a `lobby=host:port` line in `settings.txt`;
- `PES6_LOBBY=host:port` in `env.txt`.
