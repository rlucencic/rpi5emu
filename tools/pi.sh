#!/bin/bash
# tools/pi.sh <project> [build | run [options] | sync]
#
# From the development computer to the Pi (RPI5_HOST, default robert-pi5),
# where the rpi5* projects are built and run:
#   sync    the working trees (rpi5emu, the project, esp32emu and the
#           project's esp32* sibling: uncommitted changes too) into the
#           Pi's ~/Development, no build folders, no .git
#   build   sync, then cmake --build there (configures the first time)
#   run     the program there, its window here (ssh -C -Y: X forwarding,
#           compressed), its log on this terminal; options go to it
set -e
project=${1:?project, e.g. rpi5zx}
what=${2:-build}
shift 2 || true
host=${RPI5_HOST:-robert-pi5}
dev=$(cd "$(dirname "$0")/../.." && pwd)
sibling=esp32${project#rpi5}

sync() {
    for repo in rpi5emu "$project" esp32emu "$sibling"; do
        [ -d "$dev/$repo" ] || continue
        rsync -a --exclude .git --exclude 'build*/' --exclude managed_components \
            --exclude 'roms/*.rom' "$dev/$repo" "$host:Development/"
    done
}

case "$what" in
sync)
    sync ;;
build)
    sync
    ssh -o BatchMode=yes "$host" "cd ~/Development/$project && \
        { [ -f build/build.ninja ] || cmake -S . -B build -G Ninja; } && cmake --build build" ;;
run)
    ssh -C -Y "$host" "cd ~/Development/$project && exec build/$project $*" ;;
*)
    echo "pi.sh <project> [sync | build | run [options]]"; exit 2 ;;
esac
