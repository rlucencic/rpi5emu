#pragma once

/*
 * Line commands, from the terminal the machine was started in (stdin) and
 * from 127.0.0.1:<port> (--console, default 6400: `nc 127.0.0.1 6400`, or
 * bash's /dev/tcp/127.0.0.1/6400), as the boards' serial console:
 *
 *   i                  the machine's state, speed and console_info()
 *   r                  cold reset
 *   F <drive> [name]   insert <name> (in the disk folder, or a path) into
 *                      drive 0, 1, ...; eject without a name, "+" for a new
 *                      blank disk
 *   d                  the drives and what's in them
 *   s [file.png]       the machine's picture, at its own size (default
 *                      <data>/screenshots/<machine>-<n>.png)
 *   c                  the setup choices; c <row> <n>: choose (a running
 *                      machine restarts, unless the row is live)
 *   status             the status as JSON (the boards' /api/status rows)
 *   v [0-100|m]        the sound's volume, set it, or m: mute or unmute
 *   setup              the setup screen open, or back to the machine
 *   t <x> <y>          a click on the setup screen, laid out 1024 x 600 (as
 *                      the MaTouch's): Start is t 900 554
 *   q                  quit
 *   h                  this list
 *
 * Anything else goes to the machine (emu_machine_t.console_command), e.g.
 * keys and typing. Every command answers at least one line; "ok" if it
 * says nothing else.
 */

/* Printf to the console the command came from (for the machine's console
 * commands); stdout outside a command
 */
void emu_console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
