#!/usr/bin/env python3
"""port_finish.py <sdl project>/src/main.c <esp32 sibling name>

The usual hand edits after port_glue.py, where the esp32* glue follows its
usual shape (each one reported, or "not found": then by hand):

  the header comment's board and runtime        the Pi's
  missing()'s "flash it, or the SD card" text   its folder's roms/ (emu_rom_dir())
  "on the SD card", "No SD card"                the data folder
  flush(): written back between frames          also shutdown(), at the end
"""
import pathlib
import re
import sys
import textwrap


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    path = pathlib.Path(sys.argv[1])
    sibling = sys.argv[2]
    t = path.read_text()

    def edit(what, pattern, repl, flags=0):
        nonlocal t
        t2, n = re.subn(pattern, repl, t, flags=flags)
        print(f"{what}: {'done' if n else 'not found'}")
        t = t2

    board = r"(Makerfabs[\s*]+MaTouch[\s*]+ESP32-S3[\s*]+7\"|M5Stack[\s*]+Tab5)"
    edit("header: the board", rf"[\s*]+on[\s*]+the[\s*]+{board}([\s*]+(and|or)[\s*]+the[\s*]+{board})?\.",
         " with SDL 3.", re.S)
    words = "The board side (.*?) is the shared runtime in ../esp32emu; this file plugs the (.*?) into it:"
    pattern = (re.escape(words).replace(r"\(\.\*\?\)", "(.*?)").replace(r"\ ", r"[\s*]+")
               + r"(.*?)(?=\n \*\s*\n|\n \*/)")

    def runtime(m):
        machine = re.sub(r"[\s*]+", " ", m.group(2))
        rest = re.sub(r"[\s*]+", " ", m.group(3)).strip()
        text = ("The Pi's side (window, sound, keyboard, setup screen, web page, console, settings, the data "
                f"folder) is the shared runtime in ../sdlemu, the machine {sibling}'s; this file plugs the "
                f"{machine} into it, as {sibling}'s main/main.c does on the boards (and mostly is that file): "
                f"{rest}")
        return textwrap.fill(text, 72, subsequent_indent=" * ")

    edit("header: the runtime", pattern, runtime, re.S)
    edit("missing(): the ROM message",
         r'static char message\[\d+\];\s*snprintf\(message, sizeof\(message\),\s*"No %s ROM: put %s into roms/ and '
         r'flash with idf\.py flash, or copy it into the "\s*"\\"(?:\w+/)?roms\\" folder on the SD card\.", ([^;]*)\);',
         r'static char message[1400];\n    snprintf(message, sizeof(message), "No %s ROM: copy %s into %s.", '
         r'\1, emu_rom_dir());', re.S)
    edit("the SD card", r"on the SD card", "in the data folder")
    edit("no SD card", r"No SD card", "No data folder")

    def fixed_message(m):
        indent, what, files = m.group(1), m.group(2), m.group(3)
        return (f"{indent}static char message[1400];\n"
                f'{indent}snprintf(message, sizeof(message), "{what}: copy {files} into %s.", emu_rom_dir());\n'
                f"{indent}emu_set_message(message);")

    edit("fixed ROM messages",
         r'( *)emu_set_message\("(No [^"]*?ROMs?): put ([^"]*?) into roms/ and flash with idf\.py ?"?\s*"?flash, "?'
         r'\s*"?or copy (?:it|them) into the \\"(?:\w+/)?roms\\" folder (?:on the SD card|in the data folder)\."\);',
         fixed_message, re.S)

    m = re.search(r"/\* Disk writes go to the SD card from here \(core 0\), between frames, once\n"
                  r" \* nothing has been written for a second\n \*/\n", t)
    if m:
        t = t.replace(m.group(0), "/* Disk writes go back to their files from here (the stats thread), between\n"
                      " * frames, once nothing has been written for a second; and at the end\n */\n")
    m = re.search(r"static void flush\(void \*arg\)\n\{\n(.*?)\n\}\n", t, re.S)
    if m and "static void shutdown(void)" not in t:
        body = "\n".join(line for line in m.group(1).split("\n") if "(void)arg;" not in line)
        t = t.replace(m.group(0), m.group(0) + f"\nstatic void shutdown(void)\n{{\n{body}\n}}\n", 1)
        t = t.replace("    .every_second = every_second,\n", "    .every_second = every_second,\n"
                      "    .shutdown = shutdown,\n", 1)
        print("shutdown(): done (as flush(); check it needs no arg)")
    else:
        print("shutdown(): not found")
    path.write_text(t)
    left = [(n + 1, line.strip()) for n, line in enumerate(t.splitlines())
            if re.search(r"SD card|MaTouch|Tab5|esp32emu|flash|ESP", line)]
    for n, line in left:
        print(f"  look at {n}: {line[:110]}")


if __name__ == "__main__":
    main()
