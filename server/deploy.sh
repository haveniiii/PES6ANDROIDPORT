#!/bin/sh
# Installs (or updates) the lobby server on a fresh Debian/Ubuntu VPS and checks it from here.
#   server/deploy.sh root@<ip>          (needs SSH access with a key; see server/README.md)
#   SSH_OPTS="-i ~/.ssh/key" for another key
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
python3 "$dir/probe.py" "$host:27700"
