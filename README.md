# rpi5emu

The shared Linux runtime for the `rpi5*` retro-computer emulators on the
Raspberry Pi 5: the [esp32emu](https://github.com/rlucencic/esp32emu)
family's machines, each in a window of its own (X11 or Wayland, through
SDL 3).

The machines aren't written again: each `rpi5*` project builds its
`esp32*` sibling's platform-independent machine core (the Spectrum of
[esp32zx](https://github.com/rlucencic/esp32zx), ...) and esp32emu's CPU
cores (Z80, 68000, 6502, ...), and plugs them into this runtime through the
same `emu_machine_t` interface they have on the ESP32 boards. One
implementation of each machine for the boards and the Pi.

## What it gives a machine

- **The window**: the machine's picture scaled to the window with its
  aspect kept (sharp pixels, or smooth for 1-bit pictures), resizable, full
  screen; drawn again only when the picture changes.
- **The setup screen**, as on the boards: the machine's choices (model,
  memory, ...), the sound's volume, the drives and their images (insert,
  eject, new blank disks), Start / Back / Restart, Reset. Shown at start,
  and with Ctrl+Alt+M.
- **The web page**, the boards' own: the machine's status, the drives, the
  disk images (upload with drag and drop, download, delete, insert, new
  blank disks), on `http://<the Pi>:8080/`.
- **Sound** through SDL (PipeWire, PulseAudio, ALSA), the machine paced by
  the clock and the sound card's rate followed.
- **Keyboard and mouse**: keys by position (HID usages, as the boards'
  Bluetooth keyboards), the mouse captured with a click in the window.
- **A console** of line commands on the terminal and on `127.0.0.1:6400`:
  keys, typing, disks, screenshots, choices, the status.
- **The debugger** of the esp32emu family (`--debug <port>`, the VS Code
  extension [esp32emu-vscode](https://github.com/rlucencic/esp32emu-vscode)).
- **Files where the boards have an SD card and NVS**: a data folder
  (`~/rpi5emu`: `roms/` and the machines' disk folders) and a settings file
  (`~/.config/rpi5emu/settings.ini`).

## Building

On a Raspberry Pi 5 with 64-bit Ubuntu 26.04 (any Linux with SDL 3.2 or
later should do):

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
                 libsdl3-dev libsdl3-ttf-dev libpng-dev
```

`libsdl3-ttf-dev` is optional: without it the setup screen uses SDL's own
8 x 8 font. rpi5emu isn't built on its own: build a machine, with its
siblings cloned side by side:

```sh
git clone https://github.com/rlucencic/esp32emu
git clone https://github.com/rlucencic/rpi5emu
git clone https://github.com/rlucencic/esp32zx
git clone https://github.com/rlucencic/rpi5zx
cd rpi5zx && cmake -S . -B build -G Ninja && cmake --build build
build/rpi5zx --help
```

## Running

```
rpi5zx [options] [images ...]

  images               disks, tapes, snapshots ...: each into the first drive
                       that takes it
  -S, --start          start the machine at once (else the setup screen first)
  -d, --data DIR       the data folder: roms/, the disk folders
                       (default $RPI5EMU_HOME, else ~/rpi5emu)
  -f, --fullscreen     start full screen (Ctrl+Alt+F switches)
  -s, --scale N        the window's size: N times the machine's own (default 1)
  -c, --choice L=NAME  a setup choice, e.g. --choice Model=+3
      --no-sound
  -p, --console PORT   line commands on 127.0.0.1:PORT (default 6400, 0: none)
  -w, --web PORT       the web page on PORT (default 8080, 0: none)
      --debug PORT     the debugger on 127.0.0.1:PORT
```

Keys of the window's own (the machine gets all the others): Ctrl+Alt+M the
setup screen, Ctrl+Alt+F full screen, Ctrl+Alt+G frees the mouse,
Ctrl+Alt+W warp speed, Ctrl+Alt+S screenshot, Ctrl+Alt+R reset,
Ctrl+Alt+Q quit. Disk images can be dropped on the window.

The window can be shown on another computer's X display with
`ssh -C -Y <the Pi> rpi5zx`: good for the setup screen and still pictures,
but every changed picture crosses the network (8 to 15 a second on a home
network): for games, a monitor on the Pi.

The web page is open to anyone on the network, as the boards' is: `--web 0`
turns it off.

## The machines

| Project | Machine | From |
|---|---|---|
| [rpi5zx](https://github.com/rlucencic/rpi5zx) | ZX Spectrum 128K, +2, +3 | esp32zx |
| [rpi5cpc](https://github.com/rlucencic/rpi5cpc) | Amstrad CPC 464, 664, 6128, 464 Plus, 6128 Plus | esp32cpc |
| [rpi5cbm](https://github.com/rlucencic/rpi5cbm) | Commodore 64 and 128 | esp32cbm |
| [rpi5coco](https://github.com/rlucencic/rpi5coco) | Tandy Color Computer 2 and 3 | esp32coco |
| [rpi5gs](https://github.com/rlucencic/rpi5gs) | Apple IIGS | esp32gs |
| [rpi5msx](https://github.com/rlucencic/rpi5msx) | MSX1 and MSX2 | esp32msx |
| [rpi5xl](https://github.com/rlucencic/rpi5xl) | Atari 800XL and 130XE | esp32xl |
| [rpi5ste](https://github.com/rlucencic/rpi5ste) | Atari STE | esp32ste |
| [rpi5pc](https://github.com/rlucencic/rpi5pc) | IBM PC/XT and PC/AT | esp32pc |
| [rpi5mac](https://github.com/rlucencic/rpi5mac) | Macintosh Plus, SE, Classic | esp32mac |
| [rpi5mac2](https://github.com/rlucencic/rpi5mac2) | Macintosh LC and II | esp32mac2 |
| [rpi5mac3](https://github.com/rlucencic/rpi5mac3) | Macintosh LC II and LC III | esp32mac3 |
| [rpi5arc](https://github.com/rlucencic/rpi5arc) | Acorn Archimedes | esp32arc |
| [rpi5ami](https://github.com/rlucencic/rpi5ami) | Amiga 500, 2000, 600, 1200 | esp32ami |

## Licence

MIT.
