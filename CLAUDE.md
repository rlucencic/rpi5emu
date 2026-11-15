# rpi5emu: the shared runtime for the rpi5* emulators

The Raspberry Pi 5 (64-bit Ubuntu 26.04, SDL 3) side of the esp32emu
family: every `esp32*` emulator gets an `rpi5*` project (the user's plan,
2026-11-15), each a window of its own (X11 or Wayland). C, CMake. Users:
`../rpi5zx` (the Spectrum) so far.

**One machine, two platforms**: an rpi5* project compiles its esp32*
sibling's platform-independent core (`../esp32zx/components/zx`) and
esp32emu's CPU cores (`rpi5emu_add_cores()`, `../esp32emu/components`), and
its own `src/main.c`: the sibling's `main/main.c` ported (mostly the same
file: keymap, drives, status, console). A fix in a core is a fix for the
boards too; a core change must keep the boards' builds working (esp32emu's
CLAUDE.md: all projects built). rpi5emu's interface is esp32emu's
`emu_machine_t` (`include/emu/emu.h`; the Pi's own extra: `shutdown()`,
`emu_info_t.pictures`), `disks.h`, `json.h` (copied: keep them in step),
plus `settings.h` (NVS's place) and `log.h` (`EMU_LOGx` for `ESP_LOGx`).

## Layout

| Path | What |
|---|---|
| `include/emu/emu.h` | The machine interface and the runtime's API |
| `src/emu.c` | Options, the window and its picture (main thread), the emulation thread (paced by the clock; sound into an SDL stream after each frame, its rate nudged), holds (`emu_paused()`, the setup screen), input (Ctrl+Alt keys, HID usages = SDL scancodes, mouse capture), stats thread, status JSON (the boards' `/api/status` shape) |
| `src/ui.c` | The setup screen: SDL drawing on a 1024 x 600 canvas scaled to the window; SDL3_ttf text (Ubuntu or DejaVu font) or SDL's 8 x 8 debug font |
| `src/web.c` | The boards' web page (esp32emu's `web/index.html`, built in by `cmake/embed.cmake`) and its API, a thread a connection |
| `src/console.c` | Line commands from stdin and 127.0.0.1:6400 |
| `src/disks.c` | esp32emu's disks.c on the data folder; a name starting with `/` is a path anywhere |
| `src/settings.c` | `~/.config/rpi5emu/settings.ini`, sections as NVS namespaces |
| `src/png.c` | Screenshots (libpng) |
| `tools/pi.sh` | Sync to the Pi, build there, run with X forwarding |
| `tools/console.sh` | Console commands to the Pi's emulator |
| `tools/capture.sh`, `tools/xwd2png.py` | The forwarded window as a PNG |

## Developing (the user's setup)

- Edit on the development PC (`/home/robert/Development`, Zorin, x86-64:
  no SDL 3 packages there), build and run on the Pi: `robert@robert-pi5`
  over SSH (key auth; `~/Development` with the same layout; gh and GitHub
  over SSH there too). `tools/pi.sh rpi5zx build` rsyncs the working trees
  (rpi5emu, the project, esp32emu, the esp32 sibling: uncommitted changes
  too, no .git, no build folders) and builds; `tools/pi.sh rpi5zx run
  [options]` runs it with its window here (`ssh -C -Y`). The Pi's clones are
  git repos whose working trees rsync overwrites: after a push, `git fetch
  && git reset --hard origin/main` there.
- ROMs on the Pi: `~/rpi5emu/roms` (copied from the esp32 projects' `roms/`,
  not in git).
- Driving it: `tools/console.sh 'i' 'T 10 PRINT 1\n' @4 's /tmp/x.png'`
  (the machine's picture, then `scp`), `tools/capture.sh out.png` (the
  window as shown, the setup screen too; xwd here), `setup` and `t x y`
  (the setup screen; Start is `t 900 554`, Disks `t 112 554`, the first
  row's buttons at y 167), `curl http://robert-pi5.local:8080/api/status`.
- Debugging: gdb on the Pi, or `gdb-multiarch` here with `target remote |
  ssh robert@robert-pi5 gdbserver - <program>` (`set sysroot target:`); the
  sources are at the same paths on both. The machines' debugger:
  `--debug 6502` and an SSH tunnel.
- Missing tools that would help (the user may install them): on the Pi
  `xvfb xdotool imagemagick x11-apps valgrind` (a virtual display to test
  in without the user's screen, real keys and clicks), here `xdotool
  imagemagick`.

## Lessons

- **X forwarding carries every changed picture**: a 704 x 600 window over
  `ssh -Y` showed 8 pictures a second with the whole border changing each
  frame, 15 with `-C`; the emulation stays at 100% (it doesn't wait for the
  window). The window thread falls behind, so its copy of the picture is
  old: screenshots come from the machine itself, held between frames
  (`emu_screenshot()`), not from the presenter. The `pictures/s` of `i`
  says how the window keeps up.
- **The setup screen's buttons are where they were last drawn**: clicks
  that come before a redraw (the console's `t`, quick ones) found the old
  page's buttons; a click now lays the page out first when it changed.
- **esp32zx's typing**: `T` replaces what's still being typed and leaves
  the key held at that moment down (the board's too); and the 128 and +3
  editors take the first key after a report to clear it. Wait for one `T`
  to end (6 frames a character) before the next.
- Pipewire on the Pi without a monitor plays into `auto_null`: the sound
  runs (the stream is there) but nothing is heard.

## Working with the user

- Expects changes built and checked on the Pi, then a short report of what
  was verified and what wasn't.
- Commits and pushes only when asked (GitHub over SSH, `main`); the commit
  messages end with the session's `Co-Authored-By` line.
- Keep their private data (network names, addresses, disk images) out of
  anything published.
