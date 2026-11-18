#!/usr/bin/env python3
"""port_rom_store.py <esp32 project> <sdl project>

An esp32* project's main/rom_store.c and .h into the sdl* project's src/,
for the machine's roms/ folder: the flash partition's reading (from_partition())
goes, the SD card's (from_sd()) becomes emu_rom_read() (sdlemu), the
comments say so. The usual shape only: what doesn't fit is reported, and
then ported by hand.
"""
import pathlib
import re
import sys


def function_span(text, name):
    """(start, end) of 'static ... name(...)\\n{ ... \\n}\\n' with the comment before it"""
    m = re.search(r"(/\*(?:(?!\*/).)*\*/\n)?static [a-z_ ]+\b" + name + r"\([^)]*\)\n\{\n", text, re.S)
    if not m:
        return None
    end = text.find("\n}\n", m.end())
    return m.start(), end + 3


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src = pathlib.Path(sys.argv[1]) / "main"
    dst = pathlib.Path(sys.argv[2]) / "src"
    dst.mkdir(parents=True, exist_ok=True)

    h = (src / "rom_store.h").read_text()
    h2 = re.sub(r"from the \w+ flash partition \(roms/<name>\s*\*?\s*in the\s*\*?\s*project, written (there )?by idf\.py "
                r"flash( into [^)]*)?\), else from the\s*\*?\s*SD\s*\*?\s*card's \"?[\w/]*\"? folder( under the same "
                r"names)?\.", "files in the machine's roms/ folder (emu_rom_read()), named as on the boards.", h,
                flags=re.S)
    print(f"rom_store.h: comment {'done' if h2 != h else 'not found'}")
    (dst / "rom_store.h").write_text(h2)

    c = (src / "rom_store.c").read_text()
    for inc in ('#include "esp_log.h"\n', '#include "esp_partition.h"\n', '#include "emu/sdcard.h"\n'):
        c = c.replace(inc, "")
    c = c.replace('#include "rom_store.h"', '#include "emu/emu.h"\n#include "rom_store.h"', 1)
    c = re.sub(r'#define ROM_PARTITION_TYPE.*\n', "", c)
    c = re.sub(r'#define ROM_FOLDER.*\n', "", c)
    c = re.sub(r'static const char \*TAG = "rom";\n\n?', "", c)
    for name in ("from_partition", "from_sd"):
        span = function_span(c, name)
        print(f"{name}(): {'gone' if span else 'not found'}")
        if span:
            c = c[:span[0]] + c[span[1]:]
    c, n = re.subn(r"    if \(from_partition\([^\n]*\) && plausible\([^\n]*\)\) \{\n        return true;\n    \}\n",
                   "", c)
    print(f"the partition's branch: {n} gone")
    c = c.replace("from_sd(", "emu_rom_read(")
    c = re.sub(r"\n{3,}", "\n\n", c)
    left = [(i + 1, line.strip()) for i, line in enumerate(c.splitlines())
            if re.search(r"partition|ESP_|esp_|SLOT\b|SDCARD|sdcard", line)]
    for i, line in left:
        print(f"  look at {i}: {line[:110]}")
    if len(re.findall(r"\bSLOT\b", c)) == 1:
        c = re.sub(r"#define SLOT .*\n\n?", "", c)
    (dst / "rom_store.c").write_text(c)


if __name__ == "__main__":
    main()
