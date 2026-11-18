#!/bin/bash
# tools/capture.sh out.png
#
# The sdl* window on this computer's X display (one forwarded with ssh -Y:
# what the window shows, the setup screen too) as a PNG. xwd and Python's
# PIL; the console's `s` gives the machine's own picture instead.
here=$(dirname "$0")
out=${1:?out.png}
w=$(xwininfo -root -tree | grep -E '\("sdl[a-z0-9]+" ' | head -1 | awk '{print $1}')
[ -n "$w" ] || { echo "no sdl* window"; exit 1; }
xwd -id "$w" -silent > "$out.xwd" && python3 -I "$here/xwd2png.py" "$out.xwd" "$out"
rm -f "$out.xwd"
