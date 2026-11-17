#pragma once

/*
 * The emulator runtime on the Raspberry Pi 5 (64-bit Linux, SDL 3). A
 * machine (Spectrum, Amiga, ...) describes itself in an emu_machine_t, the
 * same interface as on the ESP32 boards (../esp32emu), and emu_main() does
 * the rest:
 *
 *   emulation thread: one machine frame at a time, paced by the clock; the
 *                     sound goes to the audio device after each frame (its
 *                     rate nudged to keep the queue short)
 *   main thread:      the window (an X11 or Wayland window through SDL), the
 *                     machine's picture scaled into it when it changed, the
 *                     keyboard and mouse
 *   stats thread:     once a second: speed, every_second(), the title
 *   console threads:  line commands from stdin and 127.0.0.1:<port>
 *
 * Where the boards have an SD card and NVS, the Pi has folders:
 *   emu_data_dir()    the "SD card": $RPI5EMU_HOME, else ~/rpi5emu (a
 *                     folder for each machine: its disks, its roms/)
 *   settings          ~/.config/rpi5emu/settings.ini (emu/settings.h)
 *
 * Start-up: settings, reserve_memory(), storage(), the remembered disks, the
 * images named on the command line, the window, sound, start(). When
 * start() fails, the window shows emu_set_message()'s text.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "emu/disks.h"
#include "emu/json.h"

typedef struct {
    int width;
    int height;
    int hz;                     /* frames per second of the emulated video */
    /* Optional (0: the machine's view_width at 600 lines, every column):
     * the picture's shape in the window (view_w x view_h: its aspect) and
     * the columns of it shown (src_x, src_w), e.g. the Amiga's super-hires
     * frames
     */
    int view_w, view_h;
    int src_x, src_w;
} emu_screen_t;

typedef enum {
    EMU_SCALE_NEAREST,          /* sharp pixels (colour pictures) */
    EMU_SCALE_SMOOTH,           /* filtered (black and white at odd factors) */
} emu_scale_t;

/* A row of choices on the setup screen: "System   [TOS 1.62] [TOS 2.06]".
 * Changing one while the machine runs restarts it, unless it's `live`:
 * set() changes the running machine itself (between frames). A row whose
 * buttons another choice decides: the machine's choice_names(). On the Pi:
 * the console's `c` and the command line's --choice for now.
 */
typedef struct {
    const char *label;
    const char *const *names;   /* button labels, ends with ""; "a\nb": b smaller
                                   under a ("640 x 480\nup to 256 colours") */
    int (*get)(void);
    void (*set)(int index);     /* also saves it */
    bool live;                  /* takes effect without a restart */
} emu_choice_t;

typedef struct {
    const char *name;           /* "Atari STE": the window's title */
    const char *short_name;     /* "Atari" */
    int view_width;             /* picture width at 600 lines high: its aspect
                                   (and the window's first size) */
    emu_scale_t scale;

    /* At start, before anything else */
    void (*reserve_memory)(void);
    /* Once the data folder is there (or isn't) */
    void (*storage)(bool sd_card);
    /* (Re)start with the current settings; false if it can't */
    bool (*start)(void);
    void (*reset)(void);        /* cold reset (emulation paused) */
    void (*run_frame)(void);
    bool (*halted)(void);       /* the CPU stopped until a reset */

    void (*screen_mode)(emu_screen_t *mode);
    uint32_t (*screen_signature)(void);     /* changes when the picture does */
    void (*render_line)(int y, uint16_t *dst);      /* RGB565, mode.width pixels */

    /* Sound, 16-bit stereo (render NULL: none) */
    int audio_hz;
    void (*audio_enable)(bool on);
    int (*audio_render)(int16_t *dst, int frames);
    int (*audio_pending)(void);

    /* Input, from other threads */
    void (*hid_key)(uint8_t usage, bool pressed);   /* HID usage, $E0-$E7 modifiers */
    void (*hid_mouse)(int dx, int dy, uint8_t buttons);
    /* The boards' touch screen (not used on the Pi yet) */
    void (*touch)(int x, int y, bool pressed);
    void (*touchpad)(int dx, int dy, uint8_t buttons);

    /* Setup choices */
    const emu_choice_t *choices;
    int choice_count;
    /* Rows whose buttons another choice decides (the MSX's memory by its
     * model): row i's buttons now, or NULL for its own names
     */
    const char *const *(*choice_names)(int i);

    /* Disk images in the data folder for the machine's drives (NULL: none) */
    const disks_config_t *disks;
    /* The boards' web page script (not used on the Pi yet) */
    const char *web_script, *web_script_end;

    /* Status (the console's `status`): EMU_STATUS_ROWS adds the machine's
     * rows (emu_json_row()), EMU_STATUS_DRIVES its drives (emu_json_drive())
     */
    void (*status)(emu_json_t *j, int section);
    void (*console_info)(char *buf, size_t len);
    /* Console lines the runtime doesn't know (e.g. 'R'); true if handled */
    bool (*console_command)(const char *line);
    /* Once a second from the stats thread (e.g. write a disk back) */
    void (*every_second)(void);

    /* The Pi's own: before the program ends, the emulation stopped (write
     * back what every_second() would have, later). NULL: nothing to do.
     */
    void (*shutdown)(void);
} emu_machine_t;

enum { EMU_STATUS_ROWS, EMU_STATUS_DRIVES };

/* Runs the machine until its window closes; main() returns its result:
 *
 *   int main(int argc, char **argv) { return emu_main(&machine, argc, argv); }
 *
 * Options: --help lists them (data folder, window size, full screen, sound,
 * console port, debugger port, choices, images to insert).
 */
int emu_main(const emu_machine_t *machine, int argc, char **argv);

const emu_machine_t *emu_machine(void);

/* The "SD card": a folder for each machine (its disks, its roms/), no
 * trailing slash
 */
const char *emu_data_dir(void);

/* This machine's ROMs: roms/ in its disk folder (<data>/zx/roms; the Macs
 * share mac/roms), made at start, no trailing slash
 */
const char *emu_rom_dir(void);

/* A ROM file from emu_rom_dir() (the name as given, else in capitals: as
 * copied from a FAT card), up to max bytes into buf: the bytes read, -1 if
 * there's none
 */
long emu_rom_read(const char *name, void *buf, size_t max);

typedef enum {
    EMU_SETUP,                  /* not started (start() failed) */
    EMU_RUNNING,
    EMU_PAUSED,                 /* held between frames */
    EMU_HALTED,                 /* the CPU stopped until a reset */
    EMU_STOPPED,                /* the debugger stopped it (emudbg.h) */
} emu_state_t;

typedef struct {
    emu_state_t state;
    float fps;                  /* frames emulated per second, last second */
    float pictures;             /* pictures drawn into the window per second */
    int speed;                  /* % of real speed */
    float load[2];              /* % busy: the main thread (window), the emulation */
    float temperature;          /* the SoC, degrees C, as last read; NAN if unknown */
    uint32_t session_s;         /* seconds since the machine was (re)started */
    bool sd_card;               /* the data folder is there */
} emu_info_t;

/* Updated about once a second; safe from any thread */
void emu_info(emu_info_t *info);

/* Have the temperature read (at most every 5 s; it is anyway) */
void emu_measure_temperature(void);

/* Cold reset, done by the emulation thread before its next frame */
void emu_request_reset(void);

/* Run fn with the emulation stopped between frames (or right away when it
 * isn't running). False if it couldn't be held.
 */
bool emu_paused(void (*fn)(void *arg), void *arg);

/* Start the machine again with its current settings (paused meanwhile) */
bool emu_restart(void);

/* Take the machine's offer for a drive's disk (disks_config_t.offer): its
 * settings change and a running machine restarts with them (with the old
 * ones again if it can't start). False if there is none.
 */
bool emu_take_offer(int drive);

/* Text for the status: what the data folder is used for */
void emu_set_drive_info(const char *text);

/* Why the machine can't start (e.g. "No ROM"), shown in the window; NULL
 * clears it
 */
void emu_set_message(const char *text);
