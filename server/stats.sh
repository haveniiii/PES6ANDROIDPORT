#!/bin/sh
# What happened on the lobby server, from the Mac.
#   server/stats.sh root@<ip> [since]     a summary (since: "today" by default, or "2 hours ago", "2026-11-09")
#   server/stats.sh root@<ip> -f          follow the events as they happen
#   SSH_OPTS="-i ~/.ssh/key" for another key
set -eu
[ $# -ge 1 ] || { echo "usage: $0 user@host [since | -f]" >&2; exit 2; }
target=$1
dir=$(cd "$(dirname "$0")" && pwd)
if [ "${2:-}" = "-f" ]; then
  exec ssh ${SSH_OPTS:-} "$target" "journalctl -u p6lobby -f -n 30 -o cat | grep --line-buffered -E ' ev | phones, '"
fi
ssh ${SSH_OPTS:-} "$target" "journalctl -u p6lobby --since '${2:-today}' -o cat --no-pager" | python3 "$dir/summary.py"
