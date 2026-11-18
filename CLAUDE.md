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
| `tools/pi.ps1`, `tools/console.ps1` | The same from Windows (PowerShell 7.4+): the trees synced through git, not rsync |
| `tools/capture.sh`, `tools/xwd2png.py` | The forwarded window as a PNG |
| `tools/data_folders.sh` | An old data folder's shared `roms/`, `floppies/`, `disks/`, `atari/` into the machines' own folders (run on the Pi; `-n` only says) |

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
- Or from the Mac (`~/Development` there too): `~/.ssh/config` gives
  `robert-pi5` the user `robert` (the Mac's login is another) and
  `XAuthLocation /opt/X11/bin/xauth` (macOS's ssh looks in `/usr/X11R6`);
  XQuartz is the X server (`brew install --cask xquartz`), `pi.sh run`
  starts it and has SDL draw in software. `capture.sh` wants
  `PATH=/opt/X11/bin:$PATH` and Python's PIL there.
- Or from the Windows PC (`C:\Development`, PowerShell 7, Windows' OpenSSH,
  Git for Windows; its login isn't `robert`: `~/.ssh/config` gives
  `robert-pi5` the user): `tools\pi.ps1 rpi5zx build`, `run`, `headless`,
  `shot`, `log`, and `tools\console.ps1 i 'T 10 PRINT 1\n' '@4'` (`@n`
  quoted: PowerShell's splatting). No rsync there, and git checks text
  files out with CRLF (`core.autocrlf`): `pi.ps1` makes each working tree
  a commit (a temporary index, new and uncommitted files too, not the
  ignored ones), pushes it to the Pi's clone (`refs/pi-sync/head`) and
  checks it out there with an index of its own (`.git/pi-sync.index`; the
  Pi's HEAD and index untouched): LF again, the executable bits kept, only
  the changed files rewritten (ninja then has nothing to do), a file
  deleted here deleted there. `run` uses MobaXterm's X server (or VcXsrv,
  started when nothing answers on `localhost:0`) and draws as on the Mac
  (software, no XInput2): 16 pictures a second with the whole border
  changing.
- The data folder (`~/rpi5emu` on the Pi; the boards' SD cards the same):
  a folder for each machine, the disk config's `.folder` (`zx cpc cbm coco
  apple2gs msx xl st pc arc amiga`, the three Macs sharing `mac`), its
  images in it and its ROMs in its `roms/` (`emu_rom_dir()`, made at start;
  copied from the esp32 projects' `roms/`, not in git). Shared folders until
  2026-11-17 (`roms/`, `floppies/` for the ST, Archimedes and Amiga,
  `disks/` for the Macs, the XL's `atari/`): `tools/data_folders.sh [-n]`
  moved them (ROMs by name, images by type, size and the Amiga's `RDSK`
  mark), esp32emu's `tools/sd_folders.ps1 [-DryRun] E:\` does an SD card.
- Driving it: `tools/console.sh 'i' 'T 10 PRINT 1\n' @4 's /tmp/x.png'`
  (the machine's picture, then `scp`), `window /tmp/w.png` (the window as
  drawn, the setup screen too: headless works), `tools/capture.sh
  out.png` (a forwarded window as shown here; xwd), `setup` and `t x y`
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
5. ROMs to the machine's `~/rpi5emu/<folder>/roms` on the Pi (`scp` from
   the esp32 project's `roms/`), `tools/pi.sh rpi5X build`, `headless`,
   `console.sh`, `shot`.

## Lessons

- **X forwarding carries every changed picture**: a 704 x 600 window over
  `ssh -Y` showed 8 pictures a second with the whole border changing each
  frame, 15 with `-C`; the emulation stays at 100% (it doesn't wait for the
  window). The window thread falls behind, so its copy of the picture is
  old: screenshots come from the machine itself, held between frames
  (`emu_screenshot()`), not from the presenter. The `pictures/s` of `i`
  says how the window keeps up (0 on a still screen: only a changed
  picture is drawn).
- **XQuartz has no GLX for the Pi's Mesa**: forwarded to a Mac, SDL's
  default (OpenGL) renderer ended the program at once (`No matching
  fbConfigs`, then `X_GLXCreateContext` BadValue). `SDL_RENDER_DRIVER=software`
  alone wasn't enough: SDL 3 backs a software window surface with a GL
  texture on X11 too; `SDL_FRAMEBUFFER_ACCELERATION=0` stops that.
  `pi.sh run` sets both when it runs on a Mac.
- **XQuartz's refresh rates are numbers, not rates**: RandR lists its
  modes at 1, 2 ... Hz, and the software renderer's vsync waits one refresh
  a present: a picture a second, and the input read between them came a
  second late (the grabbed mouse "offset from the hand"). No vsync when the
  display's rate is under 20 Hz: 7 pictures a second with the mouse moving.
- **SDL's relative mouse on XQuartz didn't move**: SDL 3.4's XInput2 raw
  motion takes the device's `Rel X`/`Rel Y` axes, which XQuartz's pointer
  declares but whose raw events only carry `Abs X`/`Abs Y`: every delta 0.
  `pi.sh run` sets `SDL_VIDEO_X11_XINPUT2=0`, relative mode is then refused,
  and the runtime grabs by warps (the pointer hidden, put back in the
  window's middle). XQuartz moves and hides the Mac's pointer only while
  it's the active application, and over ssh motion from before a warp
  arrives after the warp's own: motion still nearer where the pointer was
  is skipped, and the warp tried again after 200 ms.
- **MobaXterm shows its arrow for a blank cursor**: forwarded to Windows,
  the grabbed mouse showed two pointers, the machine's and MobaXterm's.
  Its X server ("Moba/X") draws its arrow for a fully blank cursor
  (`SDL_HideCursor()`'s, `XFixesHideCursor()`'s), but takes a 16 x 16
  colour cursor with one pixel at 1/255 opacity and draws nothing: the
  runtime hides the pointer so while it grabs by warps (measured with
  `GetCursorInfo()` over the window: 199 pixels of arrow, then 0).
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
- **`pi.sh` needs the Pi's copies to be clones once a repo has a remote**:
  it clones a sibling that's missing, and a plain folder in its place (an
  earlier rsync) made the clone fail and `set -e` stop the script before
  the build: the old program ran and looked like a missing feature. Turn
  such a folder into a clone (`git init`, `remote add`, `fetch`, `reset
  --hard origin/main`: the build folder stays) and check `pi.sh`'s exit
  status, not only its output.
- **README pictures without private paths**: the setup screen and the web
  page show the home folder as `~` (`emu_short_path()`); screenshots go
  from the machine (`s`) or the window (`window`), headless.
- **Key events in one frame**: the IIGS lost Control's key-up when it came
  in the same frame as Reset's (Control stayed down, letters became control
  characters): console keys a frame apart (`@0.3`).
- **Machines running at once share one settings file**: each wrote the
  whole file from its own copy through the same `settings.ini.tmp`, so one
  rename took another's file away ("can't write settings.ini") and the
  last to save undid the others' changes (8 processes setting 200 keys
  each: 2793 of 3200 values lost). A change now reads the file again under
  a lock on its folder (`flock`) and writes through a temporary file of
  its own (`mkstemp`): none lost. Reads come from memory.
- **glibc's `exit()` takes every stdio stream's lock** (2.43, the Pi's):
  the console's stdin thread, blocked in `fgets(stdin)`, held stdin's, so
  a program run with its stdin open (`pi.sh run`: the ssh session's) said
  "bye" and hung there until its ssh ended, the console and web ports
  still held; an idle console client the same. The console reads its
  lines with `read()` now: a thread's blocking read, never through stdio.
  "bye" in the log isn't the end: check the process (`pgrep`).
- Pipewire on the Pi without a monitor plays into `auto_null`: the sound
  runs (the stream is there) but nothing is heard.

## To do

- **NEXT PLANNED IMPLEMENTATION — Sinclair QL (1984) emulator (`rpi5ql`)**

  Develop for Raspberry Pi 5 alongside `esp32ql` (ESP32-S3). Compile its
  portable QL machine core and use `emu_machine_t` for SDL 3 display, audio,
  and keyboard input. Share compatibility tests with the ESP32-S3 project.

  - Motorola 68008 at 7.5 MHz, with the shared Musashi core as the starting
    point for the instruction set and QL-specific timing for the 8-bit bus.
  - 128 KB RAM, 48 KB ROM, and ZX8301 video and memory control. Support
    512×256 four-colour and 256×256 eight-colour display modes.
  - ZX8302 peripheral control, Intel 8049 keyboard and sound behaviour,
    and both built-in Microdrives with cartridge image support.
  - Boot QDOS into SuperBASIC and test original software, including loading
    and saving a document in Psion Quill.

- **Acorn Electron (1983) emulator (`rpi5electron`)** follows the Sinclair QL.

  Develop for Raspberry Pi 5 alongside `esp32electron` (ESP32-S3). Compile
  its portable machine core and shared `m6502` core, and use `emu_machine_t`
  to connect with the SDL 3 display, audio, and keyboard. Share
  compatibility tests with the ESP32-S3 project.

  - 6502A with ULA-controlled timing, including 2 MHz ROM accesses, 1 MHz
    RAM accesses, and video contention in modes 0–3.
  - 32 KB RAM, 32 KB ROM, Acorn MOS, and BBC BASIC.
  - Screen modes 0–6, single-channel sound, and keyboard emulation.
  - Cassette loading from UEF images and compatibility tests with original
    Electron software. Consider Plus 1 and Plus 3 support after the base
    machine.

- **Oric Atmos (1984) emulator (`rpi5oric`)** follows the Acorn Electron.

  Develop alongside `esp32oric` (ESP32-S3). Follow the existing sibling-project
  layout by compiling the portable `esp32oric` machine core and integrating
  with `rpi5emu` through `emu_machine_t`. Optimize for Raspberry Pi 5 with
  native display and audio output through SDL 3, USB and Bluetooth keyboard
  support, and accurate timing for high software compatibility. Share
  emulator design concepts, compatibility tests, and technical
  documentation with `esp32oric` where practical.

  - MOS 6502A CPU at 1 MHz, 48 KB RAM, 16 KB ROM, and memory banking.
  - Oric ULA with accurate video attributes, 40×28 text, and 240×200
    high-resolution graphics.
  - AY-3-8912 three-channel sound and MOS 6522 VIA with timers and interrupts.
  - Oric keyboard emulation and cassette support for TAP files. Microdisc
    floppy disk emulation is optional.
  - Compatibility with Oric Atmos BASIC 1.1 and testing with original Oric
    software.

## Working with the user

- Expects changes built and checked on the Pi, then a short report of what
  was verified and what wasn't.
- Commits and pushes only when asked (GitHub over SSH, `main`); the commit
  messages end with the session's `Co-Authored-By` line.
- Keep their private data (network names, addresses, disk images) out of
  anything published.
