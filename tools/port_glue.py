#!/usr/bin/env python3
"""port_glue.py <esp32 project> <rpi5 project> [files ...]

Copies an esp32* emulator's glue (main/*.c, *.h: default all but rom_store,
which reads flash partitions and is written anew) into an rpi5* project's
src/, with the mechanical changes for rpi5emu:

  FreeRTOS, esp_heap_caps, esp_timer includes   gone (<unistd.h> instead)
  esp_log.h, ESP_LOGx                           emu/log.h, EMU_LOGx
  nvs.h, nvs_flash.h                            emu/nvs.h (NVS over the settings file)
  vTaskDelay(pdMS_TO_TICKS(n))                  usleep((n) * 1000)
  heap_caps_malloc/calloc/realloc/free (caps)   malloc/calloc/realloc/free
  app_main() { emu_run(&machine); }             main() { return emu_main(...); }

then lists the lines still naming the boards (esp_, xTask, SDCARD, ...):
those are ported by hand.
"""
import pathlib
import re
import sys

BOARD_WORDS = re.compile(r"esp_|ESP_|xTask|xSemaphore|portMAX|portTICK|pdMS|MALLOC_CAP|heap_caps|SDCARD|sdcard|"
                         r"IRAM_ATTR|DRAM_ATTR|EXT_RAM|CONFIG_|_binary_|emu_reboot_into_machine|flash")


def strip_call(text, name, keep_args):
    """name(a, b, caps) -> replacement(first keep_args args), parentheses balanced"""
    out, i = [], 0
    while True:
        j = text.find(name + "(", i)
        if j < 0:
            out.append(text[i:])
            return "".join(out)
        out.append(text[i:j])
        k, depth, args, start = j + len(name) + 1, 1, [], j + len(name) + 1
        while depth:
            c = text[k]
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            elif c == "," and depth == 1:
                args.append(text[start:k].strip())
                start = k + 1
            k += 1
        args.append(text[start:k - 1].strip())
        out.append(keep_args(args))
        i = k


def resolve_board(text):
    """#if CONFIG_IDF_TARGET_ESP32P4 ... [#else ...] #endif: the Tab5's branch
    (the Pi has its room and speed), and the opposite for !...; nested
    conditionals inside are kept as they are"""
    lines = text.split("\n")
    out, stack = [], []          # stack entries: [ours, keeping] or None for other #ifs
    for line in lines:
        s = line.strip()
        m = re.match(r"#if\s+(!?)\s*(?:defined\()?CONFIG_IDF_TARGET_ESP32(P4|S3)\)?\s*$", s)
        if m:
            p4 = (m.group(2) == "P4") != (m.group(1) == "!")
            stack.append([True, p4])
            continue
        if re.match(r"#if", s):
            stack.append(None)
        elif s.startswith("#else") and stack and stack[-1]:
            stack[-1][1] = not stack[-1][1]
            continue
        elif s.startswith("#endif") and stack:
            if stack.pop():
                continue
        if all(e is None or e[1] for e in stack):
            out.append(line)
    return "\n".join(out)


def port(text):
    text = resolve_board(text)
    text = text.replace('#include "sdkconfig.h"\n', "")
    for inc in ("freertos/FreeRTOS.h", "freertos/task.h", "freertos/semphr.h", "freertos/queue.h",
                "esp_heap_caps.h", "esp_timer.h", "esp_system.h", "esp_attr.h", "esp_psram.h"):
        text = text.replace(f'#include "{inc}"\n', "")
    text = text.replace('#include "esp_log.h"', '#include "emu/log.h"')
    text = re.sub(r'#include "nvs(_flash)?\.h"\n', "", text)
    if "nvs_open(" in text:
        text = text.replace('#include "emu/log.h"', '#include "emu/log.h"\n#include "emu/nvs.h"', 1)
    text = re.sub(r"\bESP_LOG([EWIDV])\(", r"EMU_LOG\1(", text)
    text = re.sub(r"vTaskDelay\(pdMS_TO_TICKS\(([^()]*(?:\([^()]*\))?[^()]*)\)\)", r"usleep((\1) * 1000)", text)
    text = strip_call(text, "heap_caps_malloc", lambda a: f"malloc({a[0]})")
    text = strip_call(text, "heap_caps_calloc", lambda a: f"calloc({a[0]}, {a[1]})")
    text = strip_call(text, "heap_caps_realloc", lambda a: f"realloc({a[0]}, {a[1]})")
    text = text.replace("heap_caps_free(", "free(")
    text = re.sub(r"\b(IRAM_ATTR|DRAM_ATTR|EXT_RAM_BSS_ATTR) ", "", text)
    text = text.replace("void app_main(void)", "int main(int argc, char **argv)")
    text = text.replace("emu_run(&machine);", "return emu_main(&machine, argc, argv);")
    if "usleep(" in text and "<unistd.h>" not in text:
        text = re.sub(r"(#include <[a-z/]+\.h>\n)(?!#include <)", r"\1#include <unistd.h>\n", text, count=1)
    return text


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src = pathlib.Path(sys.argv[1]) / "main"
    dst = pathlib.Path(sys.argv[2]) / "src"
    dst.mkdir(parents=True, exist_ok=True)
    files = sys.argv[3:] or [p.name for p in sorted(src.iterdir())
                             if p.suffix in (".c", ".h") and not p.name.startswith("rom_store")]
    for name in files:
        text = port((src / name).read_text())
        (dst / name).write_text(text)
        left = [(n + 1, line.strip()) for n, line in enumerate(text.splitlines()) if BOARD_WORDS.search(line)]
        print(f"{dst / name}: {len(left)} lines to look at")
        for n, line in left:
            print(f"  {n}: {line[:110]}")


if __name__ == "__main__":
    main()
