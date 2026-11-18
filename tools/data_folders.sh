#!/bin/bash
# tools/data_folders.sh [-n] [data folder]
#
# The data folder's old shared folders into each machine's own (since
# 2026-11-17: every machine its folder, its ROMs in that folder's roms/, the
# three Macs sharing mac/; the boards' SD cards the same, esp32emu's
# tools/sd_folders.ps1):
#   roms/      each ROM into its machine's <folder>/roms/, by its name
#   floppies/  .st .msa .hd into st/; .adl, and .adf or .img of the
#              Archimedes' floppy sizes (640 KB, 800 KB, 1.6 MB), into arc/;
#              the other .img into st/ (the ST's hard disks), the other .adf
#              and .rp9 into amiga/; .hdf with the Amiga's RDSK or DOS mark
#              into amiga/, else in whole 516096-byte cylinders into arc/
#   disks/     into mac/
#   atari/     into xl/
# Nothing is overwritten; what isn't known stays where it is, listed. An old
# folder left empty goes. -n: only say what it would do. The data folder:
# Uses the same environment variables and default paths as the runtime.
set -u
dry=
if [ "${1:-}" = -n ]; then dry=1; shift; fi
data=${1:-${SDLEMU_HOME:-${RPI5EMU_HOME:-}}}
if [ -z "$data" ]; then
    data=$HOME/sdlemu
    if [ ! -e "$data" ] && [ -d "$HOME/rpi5emu" ]; then
        data=$HOME/rpi5emu
    fi
fi
cd "$data" || exit 1
moved=0
kept=0

move() {    # move <file> <folder>
    local name=${1##*/}
    if [ -e "$2/$name" ]; then
        echo "kept $1: $2/$name is there already"
        kept=$((kept + 1))
        return
    fi
    echo "$1 -> $2/"
    if [ -z "$dry" ]; then
        mkdir -p "$2" && mv -n "$1" "$2/"
    fi
    moved=$((moved + 1))
}

keep() {
    echo "kept $1: $2"
    kept=$((kept + 1))
}

rom_machine() {     # the machine folder for a ROM by its name, none if unknown
    case "${1,,}" in
    128.rom | plus2.rom | plus3.rom | trdos.rom) echo zx ;;
    os464.rom | os664.rom | os6128.rom | osplus.rom | basic10.rom | basic11.rom | basic664.rom | \
        basicplus.rom | amsdos.rom | system.cpr) echo cpc ;;
    c64*.rom | c128*.rom | 1541.rom | 1571.rom) echo cbm ;;
    bas12.rom | bas13.rom | extbas11.rom | disk10.rom | disk11.rom | coco3.rom) echo coco ;;
    gs_rom*.rom | chargen.rom) echo apple2gs ;;
    msx.rom | msx2.rom | msx2ext.rom | disk.rom | nextor.rom) echo msx ;;
    atarixl.rom | ataribas.rom) echo xl ;;
    tos*.img) echo st ;;
    plus.rom | se.rom | classic.rom | ii.rom | iifdhd.rom | lc.rom | lc2.rom | lc3.rom | mdc824.rom) echo mac ;;
    arthur*.rom | riscos*.rom) echo arc ;;
    amiga-os-*.rom | aros-*.rom | kick*.rom) echo amiga ;;
    esac
}

floppy_machine() {  # st, arc or amiga for an image in floppies/, none if unknown
    local size
    size=$(stat -c %s "$1")
    case "${1,,}" in
    *.st | *.msa | *.hd) echo st ;;
    *.adl) echo arc ;;
    *.rp9) echo amiga ;;
    *.img) case $size in 655360 | 819200 | 1638400) echo arc ;; *) echo st ;; esac ;;
    *.adf) case $size in 655360 | 819200 | 1638400) echo arc ;; *) echo amiga ;; esac ;;
    *.hdf)
        if head -c 8192 "$1" | grep -qa RDSK || [ "$(head -c 3 "$1" | tr -d '\0')" = DOS ]; then
            echo amiga
        elif [ $((size % 516096)) -eq 0 ]; then
            echo arc
        fi ;;
    esac
}

for f in roms/*; do
    [ -f "$f" ] || continue
    m=$(rom_machine "${f##*/}")
    if [ -n "$m" ]; then move "$f" "$m/roms"; else keep "$f" "whose ROM is it?"; fi
done
for f in floppies/*; do
    [ -f "$f" ] || continue
    m=$(floppy_machine "$f")
    if [ -n "$m" ]; then move "$f" "$m"; else keep "$f" "the ST's, the Archimedes' or the Amiga's?"; fi
done
for f in disks/*; do
    [ -f "$f" ] && move "$f" mac
done
for f in atari/*; do
    [ -f "$f" ] && move "$f" xl
done
if [ -z "$dry" ]; then
    for d in roms floppies disks atari; do
        [ -d "$d" ] && rmdir "$d" 2> /dev/null && echo "$d/ gone (empty)"
    done
fi
echo "${dry:+would have }moved $moved, kept $kept"
