# rpi5emu: the shared runtime for the rpi5* emulators

The Raspberry Pi 5 (64-bit Ubuntu 26.04, SDL 3) side of the esp32emu
family: every `esp32*` emulator has an `rpi5*` project (the user's plan,
2026-11-15), each a window of its own (X11 or Wayland). C, CMake. Users:
`../rpi5zx`, `rpi5cpc`, `rpi5cbm`, `rpi5coco`, `rpi5gs`, `rpi5msx`,
`rpi5xl`, `rpi5ste`, `rpi5pc`, `rpi5mac`, `rpi5mac2`, `rpi5mac3`,
`rpi5arc`, `rpi5ami`: all 14 (the last 13 ported the same day). A change
here: build them all (`for p in ...; do tools/pi.sh $p build; done`).

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
| `include/emu/nvs.h` | ESP-IDF's NVS calls over the settings file: the esp32* `settings.c` files carry over as they are |
| `cmake/embed.cmake`, `rpi5emu_embed()` | A file built in (the web page; a machine's web script: `NAME` and `NAME_SIZE` for `.web_script`, `.web_script_end`) |
| `tools/port_glue.py` | An esp32* project's `main/*.c` into an rpi5* project's `src/`: the board APIs replaced (log, NVS, heap_caps, delays, `app_main`), `#if CONFIG_IDF_TARGET_ESP32P4` resolved to the Tab5's branch; then lists what's left |
| `tools/port_finish.py` | The usual hand edits after it (the header, the ROM messages, the SD card's wording, `shutdown()` from `flush()`) |
| `tools/port_rom_store.py` | A `rom_store.c` of the usual shape (a table of names, partitions and sizes) onto `emu_rom_read()` |
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

## Porting a machine (how the 13 went)

1. `git init` an `rpi5X` beside the others; `tools/port_glue.py esp32X
   rpi5X`, `tools/port_finish.py rpi5X/src/main.c esp32X`, and
   `tools/port_rom_store.py esp32X rpi5X` (or a `rom_store.c` by hand: the
   Macs, the Archimedes and the Amiga find their ROMs by contents).
2. By hand what's left: memory the boards size by PSRAM or restart into
   (`emu_reboot_into_machine()`: allocate at `start()` instead), FreeRTOS
   tasks (threads, semaphores, mutexes), `esp_timer` (`clock_gettime()`),
   web scripts (`rpi5emu_embed()`), Kconfig options (constants with the
   Kconfig defaults), the boards' opcode table copies (the ST's and Macs'
   sizes count 4-byte pointers: pass none).
3. `CMakeLists.txt` from the core's host Makefile: its sources, include
   folders, defines (`CONFIG_Z80_*`, `M68K_*`) and cores
   (`rpi5emu_add_cores()`), zlib where the host build links it.
4. The core's `#ifndef ESP_PLATFORM` parts are its host test build's: they
   are compiled here too and may want symbols the host harness defines
   (esp32gs's `gs_host_*` switches, esp32ami's `ami_host_watch_hit()`):
   defined in the glue as the boards behave.
5. ROMs to `~/rpi5emu/roms` on the Pi (`scp` from the esp32 project's
   `roms/`), `tools/pi.sh rpi5X build`, `headless`, `console.sh`, `shot`.

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
- **Headless runs need the whole command line redirected**: `cd dir && nohup
  prog > log &` keeps the SSH session open (the `&&` list's subshell holds
  its output); `(cd dir && exec nohup prog) > log 2>&1 < /dev/null &`
  doesn't (`tools/pi.sh headless`).
- **Key events in one frame**: the IIGS lost Control's key-up when it came
  in the same frame as Reset's (Control stayed down, letters became control
  characters): console keys a frame apart (`@0.3`).
- Pipewire on the Pi without a monitor plays into `auto_null`: the sound
  runs (the stream is there) but nothing is heard.

## Working with the user

- Expects changes built and checked on the Pi, then a short report of what
  was verified and what wasn't.
- Commits and pushes only when asked (GitHub over SSH, `main`); the commit
  messages end with the session's `Co-Authored-By` line.
- Keep their private data (network names, addresses, disk images) out of
  anything published.
