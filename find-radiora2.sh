#!/bin/bash

# Looks for a RadioRA2 Main Repeater by scanning the default network for
# an open port 51023/tcp. Once it has found candidate IP addresses, this
# script verifies the host by reading an HTML file that is unique to the
# RadioRA2 repeater.
#
# Only returns the first device found on the local network. Results are
# cached in ".radiora2-addr.txt". Protect this file from being written and/or
# deleted, if you want to hard-code a prefered response. If the cached
# IP address becomes invalid, a full scan is still performed, though.

export LC_ALL=C
cache=".radiora2-addr.txt"
port=51023

validate() {
  [ -n "$1" ] || return
# curl -m 2 -o- "http://$1/deviceIP" 2>/dev/null |
#   grep -sq 'name="PRODTYPE" value="Main Repeater"' && {
  curl -m 2 -o- "http://$1/" 2>/dev/null |
    grep -sq '<h1>LUTRON</h1>' && {
    echo -n "$1" >"${cache}"
    echo "$1"
    exit 0
  }
}
{ validate "$(<${cache})"; } 2>/dev/null
{ yes n | rm -i "${cache}"; } >&/dev/null

ip="$(ip route show to default 0.0.0.0/0 2>/dev/null |
      sed 's/.*src \([^ ]*\).*/\1/;t1;d;:1;q')"
subnet="${1:-$(ip address show to "${ip}" |
               sed 's,.*inet \([0-9.]*/[0-9]*\) .*,\1,;t1;d;:1;q')}"
for t in 1 3 10; do
  rra2="$(nmap -T5 -Pn --max-retries $((t/3)) --max-rtt-timeout $((t*150))ms \
               --min-hostgroup $((256/t)) --min-parallelism $((85/t)) \
               -n -p"${port}" -sT "${subnet}" -oG - 2>/dev/null |
          sed 's/Host: *\([^ ]*\).*open.*/\1/;t;d')"
  for r in ${rra2}; do
    validate "${r}"
  done
done

exit 1
