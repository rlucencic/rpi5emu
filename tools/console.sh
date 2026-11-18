#!/bin/bash
# tools/console.sh [-p port] 'cmd' ['@secs' | 'cmd' ...]
#
# Line commands to an sdl* emulator's console on the Pi (127.0.0.1:6400 there,
# through ssh; RPI5_HOST, default robert-pi5): '@n' waits n seconds between
# them; the answers come back at the end. E.g.
#   tools/console.sh 'T 10 PRINT "HI"\n' @4 'T RUN\n' @2 's /tmp/a.png' i
port=6400
if [ "$1" = "-p" ]; then port=$2; shift 2; fi
script="exec 3<>/dev/tcp/127.0.0.1/$port || exit 1; "
for c in "$@"; do
    if [[ "$c" == @* ]]; then
        script+="sleep ${c#@}; "
    else
        script+="printf '%s\n' $(printf '%q' "$c") >&3; "
    fi
done
script+="timeout 1 cat <&3; true"
ssh -o BatchMode=yes "${RPI5_HOST:-robert-pi5}" "$script"
