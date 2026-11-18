#!/bin/bash
# tools/pi.sh <project> [build | run [options] | sync]
#
# From the development computer to the Pi (RPI5_HOST, default robert-pi5),
# where the sdl* projects are built and run:
#   sync    the working trees (sdlemu, the project, esp32emu and the
#           project's esp32* sibling: uncommitted changes too) into the
#           Pi's ~/Development, no build folders, no .git
#   build   sync, then cmake --build there (configures the first time)
#   run     the program there, its window here (ssh -C -Y: X forwarding,
#           compressed), its log on this terminal; options go to it. From
#           a Mac: XQuartz (started when DISPLAY isn't set), SDL drawing
#           in software
#   headless [options]  the program there without a window (SDL's offscreen
#           display) and started at once, in the background: drive it with
#           console.sh (q ends it)
#   shot out.png        the machine's picture from a running one
#   log                 the headless one's log
set -e
project=${1:?project, e.g. sdlzx}
what=${2:-build}
shift 2 || true
host=${RPI5_HOST:-robert-pi5}
dev=$(cd "$(dirname "$0")/../.." && pwd)
sibling=esp32${project#sdl}

sync() {
    for repo in sdlemu "$project" esp32emu "$sibling"; do
        [ -d "$dev/$repo" ] || continue
        # a git clone there first when it's on GitHub (then rsync brings the changes)
        url=$(git -C "$dev/$repo" remote get-url origin 2>/dev/null || true)
        if [ -n "$url" ]; then
            ssh -o BatchMode=yes "$host" "[ -d Development/$repo/.git ] || git clone -q $url Development/$repo"
        fi
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
    # XQuartz has no GLX visual the Pi's Mesa can use (X_GLXCreateContext
    # BadValue ends the program): SDL draws in software there, its window
    # surface too (SDL 3 backs that with a GL texture on X11). And no
    # XInput2: SDL's relative mode reads XQuartz's Rel X/Y axes, which its
    # raw motion never carries (only Abs X/Y), so the grabbed mouse didn't
    # move; without it the runtime moves it by warps
    env=
    if [ "$(uname -s)" = Darwin ]; then
        if [ -z "$DISPLAY" ]; then
            export DISPLAY=:0
            [ -S /tmp/.X11-unix/X0 ] || open -a XQuartz
            for i in $(seq 40); do [ -S /tmp/.X11-unix/X0 ] && break; sleep 0.25; done
        fi
        env="SDL_RENDER_DRIVER=software SDL_FRAMEBUFFER_ACCELERATION=0 SDL_VIDEO_X11_XINPUT2=0 "
    fi
    ssh -C -Y "$host" "cd ~/Development/$project && ${env}exec build/$project $*" ;;
headless)
    # no window anywhere (SDL's offscreen display), started at once, the
    # log in /tmp/<project>.log there; ready when the console answers
    ssh -o BatchMode=yes "$host" "(cd ~/Development/$project && SDL_VIDEO_DRIVER=offscreen \
        exec nohup build/$project --no-stdin -S $*) > /tmp/$project.log 2>&1 < /dev/null &
        for i in \$(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/6400) 2>/dev/null && exit 0; sleep 0.2; done; \
        echo 'no console'; cat /tmp/$project.log; exit 1" ;;
shot)
    # the machine's picture (the console's s) into the file named here
    out=${1:?out.png}
    "$(dirname "$0")/console.sh" "s /tmp/$project-shot.png" > /dev/null
    scp -q "$host:/tmp/$project-shot.png" "$out" && echo "$out" ;;
log)
    ssh -o BatchMode=yes "$host" "cat /tmp/$project.log" ;;
*)
    echo "pi.sh <project> [sync | build | run [options] | headless [options] | shot out.png | log]"; exit 2 ;;
esac
