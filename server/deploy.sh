#!/bin/sh
# Installs (or updates) the lobby server on a fresh Debian/Ubuntu VPS and checks it from here.
#   server/deploy.sh root@<ip>          (needs SSH access with a key; see server/README.md)
#   SSH_OPTS="-i ~/.ssh/key" for another key
#   DUCKDNS_DOMAIN=name DUCKDNS_TOKEN=... also point name.duckdns.org at this server, now and every
#   15 minutes (a systemd timer). The token is kept on the server only (/etc/p6lobby/duckdns, root, 0600).
set -eu
[ $# -eq 1 ] || { echo "usage: $0 user@host" >&2; exit 2; }
target=$1
host=${target#*@}
dir=$(cd "$(dirname "$0")" && pwd)

scp -q ${SSH_OPTS:-} "$dir/p6lobby.py" "$dir/p6lobby.service" "$target:/tmp/"
ssh ${SSH_OPTS:-} "$target" 'set -e
  command -v python3 >/dev/null || { apt-get update -q && apt-get install -yq python3; }
  id p6lobby >/dev/null 2>&1 || useradd --system --no-create-home p6lobby
  mkdir -p /opt/p6lobby
  install -m 644 /tmp/p6lobby.py /opt/p6lobby/p6lobby.py
  install -m 644 /tmp/p6lobby.service /etc/systemd/system/p6lobby.service
  systemctl daemon-reload
  systemctl enable p6lobby >/dev/null 2>&1
  systemctl restart p6lobby
  if command -v ufw >/dev/null && ufw status | grep -q active; then ufw allow 27700/udp >/dev/null; fi
  sleep 1
  systemctl is-active p6lobby'

if [ -n "${DUCKDNS_DOMAIN:-}" ] && [ -n "${DUCKDNS_TOKEN:-}" ]; then
  # The token goes through stdin, never on a command line (ps) or in the repo.
  printf 'DOMAIN=%s\nTOKEN=%s\n' "$DUCKDNS_DOMAIN" "$DUCKDNS_TOKEN" | ssh ${SSH_OPTS:-} "$target" 'set -e
    umask 077; mkdir -p /etc/p6lobby; cat > /etc/p6lobby/duckdns
    cat > /etc/p6lobby/duckdns-update <<"SCRIPT"
#!/bin/sh
# The URL (with the token) goes to curl on stdin, not on its command line.
. /etc/p6lobby/duckdns
printf "url = \"https://www.duckdns.org/update?domains=%s&token=%s&ip=\"\n" "$DOMAIN" "$TOKEN" |
  curl -fsS --max-time 20 -K - | grep -qx OK
SCRIPT
    chmod 700 /etc/p6lobby/duckdns-update
    cat > /etc/systemd/system/p6lobby-dns.service <<UNIT
[Unit]
Description=Point the lobby name (DuckDNS) at this server
After=network-online.target
Wants=network-online.target
[Service]
Type=oneshot
ExecStart=/etc/p6lobby/duckdns-update
UNIT
    cat > /etc/systemd/system/p6lobby-dns.timer <<UNIT
[Unit]
Description=Point the lobby name (DuckDNS) at this server, every 15 minutes
[Timer]
OnBootSec=1min
OnUnitActiveSec=15min
[Install]
WantedBy=timers.target
UNIT
    systemctl daemon-reload
    systemctl enable --now p6lobby-dns.timer >/dev/null 2>&1
    systemctl start p6lobby-dns.service
    echo "DuckDNS: $(systemctl show -p Result --value p6lobby-dns.service)"'
fi
python3 "$dir/probe.py" "$host:27700"
