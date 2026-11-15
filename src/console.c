/*
 * The console: line commands from stdin and from clients of
 * 127.0.0.1:<port>, each line run by the thread that read it (see
 * emu/console.h).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "emu/console.h"
#include "emu/emu.h"
#include "emu/log.h"
#include "emu_internal.h"

static const char *TAG = "console";

static __thread int t_out = 1;          /* where this thread's command answers */
static __thread bool t_said;            /* the command answered something */

static void write_all(int fd, const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
        s += w;
        n -= w;
    }
}

void emu_console_printf(const char *fmt, ...)
{
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    write_all(t_out, buf, n);
    t_said = true;
}

static const char *const s_help =
    "i                  state, speed, the machine's info\n"
    "r                  cold reset\n"
    "F <drive> [name]   insert (a name in the disk folder, or a path), eject\n"
    "                   without a name, + for a new blank disk\n"
    "d                  the drives\n"
    "s [file.png]       screenshot (the machine's picture)\n"
    "window [file.png]  the window as it's drawn (the setup screen too)\n"
    "c [<row> <n>]      the setup choices, or choose one\n"
    "status             the status as JSON\n"
    "v [0-100|m]        volume, or mute / unmute\n"
    "setup              the setup screen open, or back to the machine\n"
    "t <x> <y>          a click on the setup screen (laid out 1024 x 600)\n"
    "q                  quit\n"
    "(anything else goes to the machine)\n";

typedef struct {
    int drive;
    const char *name;
} disk_cmd_t;

/* Insert `name`, "" ejects, "+" makes a new blank disk */
static void disk_command(void *arg)
{
    const disk_cmd_t *c = arg;
    const char *drive = disks_config()->drive_names[c->drive];
    char made[DISKS_NAME_LEN];
    if (c->name[0] == 0) {
        disks_eject(c->drive);
        emu_console_printf("%s empty\n", drive);
    } else if (strcmp(c->name, "+") == 0) {
        if (disks_create(c->drive, disks_default_size(c->drive), made, sizeof(made))) {
            emu_console_printf("%s%s new %s\n", drive, emu_drive_colon(drive), made);
        } else {
            emu_console_printf("can't make a blank disk\n");
        }
    } else if (disks_insert(c->drive, c->name)) {
        emu_console_printf("%s%s %s\n", drive, emu_drive_colon(drive), c->name);
    } else {
        emu_console_printf("%s doesn't go into %s\n", c->name, drive);
    }
}

static void print_info(void)
{
    static const char *const states[] = { "not started", "running", "paused", "halted", "stopped (debugger)" };
    emu_info_t info;
    emu_info(&info);
    emu_console_printf("%s: %s, %.1f fps, %d%%, %.1f pictures/s, window %.0f%% emulation %.0f%% of a core, "
                       "%.1f C, %lu s\n",
                       emu_machine()->name, states[info.state], info.fps, info.speed, info.pictures, info.load[0],
                       info.load[1], info.temperature, (unsigned long)info.session_s);
    if (emu_message()[0]) {
        emu_console_printf("message: %s\n", emu_message());
    }
    if (emu_started() && emu_machine()->console_info) {
        char buf[512];
        buf[0] = 0;
        emu_machine()->console_info(buf, sizeof(buf));
        emu_console_printf("%s\n", buf);
    }
}

static void print_drives(void)
{
    const disks_config_t *cfg = emu_machine()->disks ? disks_config() : NULL;
    if (cfg == NULL || !disks_ready()) {
        emu_console_printf("no drives\n");
        return;
    }
    emu_console_printf("folder %s\n", disks_folder());
    for (int d = 0; d < cfg->drives; d++) {
        const char *name = disks_name(d);
        emu_console_printf("%d %-10s %s\n", d, cfg->drive_names[d], name ? name : "-");
    }
}

static void print_choices(void)
{
    const emu_machine_t *m = emu_machine();
    for (int i = 0; i < m->choice_count; i++) {
        const emu_choice_t *ch = &m->choices[i];
        const char *const *names = m->choice_names ? m->choice_names(i) : NULL;
        names = names ? names : ch->names;
        int now = ch->get();
        emu_console_printf("%d %-12s", i, ch->label);
        for (int n = 0; names[n] && names[n][0]; n++) {
            /* a button's two lines ("80286\n6 MHz") as one */
            char name[128];
            snprintf(name, sizeof(name), "%s", names[n]);
            for (char *p = name; *p; p++) {
                *p = *p == '\n' ? ' ' : *p;
            }
            emu_console_printf(" %s%d %s%s", n == now ? "[" : "", n, name, n == now ? "]" : "");
        }
        emu_console_printf("%s\n", ch->live ? " (live)" : "");
    }
}

static void execute(char *line)
{
    line[strcspn(line, "\r\n")] = 0;
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (line[0] == 0) {
        return;
    }
    t_said = false;
    const emu_machine_t *m = emu_machine();
    const char *arg = line + 1 + strspn(line + 1, " ");
    if (strcmp(line, "status") == 0) {
        static char buf[32768];
        emu_status_json(buf, sizeof(buf));
        emu_console_printf("%s\n", buf);
    } else if (strncmp(line, "window", 6) == 0 && (line[6] == 0 || line[6] == ' ')) {
        char made[1200];
        const char *file = line + 6 + strspn(line + 6, " ");
        if (emu_window_shot(file, made, sizeof(made))) {
            emu_console_printf("window %s\n", made);
        } else {
            emu_console_printf("no picture of the window\n");
        }
    } else if (strcmp(line, "setup") == 0) {
        emu_ui_toggle();
    } else if (line[0] == 't' && line[1] == ' ') {
        int x, y;
        if (sscanf(line + 1, "%d %d", &x, &y) == 2) {
            emu_ui_click(x, y);
        } else {
            emu_console_printf("t <x> <y>: a click on the setup screen (1024 x 600)\n");
        }
    } else if (strcmp(line, "help") == 0 || (line[0] == 'h' && line[1] == 0)) {
        emu_console_printf("%s", s_help);
    } else if (line[1] != 0 && line[1] != ' ') {
        /* a word the runtime doesn't know: the machine's */
        if (m->console_command == NULL || !m->console_command(line)) {
            emu_console_printf("? %s\n", line);
        }
    } else {
        switch (line[0]) {
        case 'i':
            print_info();
            break;
        case 'r':
            emu_request_reset();
            break;
        case 'q':
            emu_console_printf("bye\n");
            emu_quit();
            break;
        case 'd':
            print_drives();
            break;
        case 'F': {
            int drive = -1, at = 0;
            if (m->disks && sscanf(line + 1, " %d %n", &drive, &at) >= 1 && drive >= 0 &&
                drive < disks_config()->drives) {
                disk_cmd_t c = { drive, line + 1 + at };
                emu_paused(disk_command, &c);
            } else {
                emu_console_printf("F <drive> [name | + | nothing: eject]\n");
            }
            break;
        }
        case 's': {
            char made[1200];
            if (emu_screenshot(arg, made, sizeof(made))) {
                emu_console_printf("screenshot %s\n", made);
            } else {
                emu_console_printf("no screenshot (no picture yet, or can't write it)\n");
            }
            break;
        }
        case 'c': {
            int row, n;
            if (sscanf(arg, "%d %d", &row, &n) == 2) {
                if (!emu_choose(row, n)) {
                    emu_console_printf("no such choice, or the machine doesn't start so\n");
                }
            }
            print_choices();
            break;
        }
        case 'v':
            if (*arg == 'm') {
                emu_set_muted(!emu_muted());
            } else if (*arg) {
                emu_set_volume(atoi(arg));
            }
            emu_console_printf("volume %d%s\n", emu_volume(), emu_muted() ? ", muted" : "");
            break;
        default:
            if (m->console_command == NULL || !m->console_command(line)) {
                emu_console_printf("? %s\n", line);
            }
            break;
        }
    }
    if (!t_said) {
        emu_console_printf("ok\n");
    }
}

static void *stdin_thread(void *arg)
{
    char line[4096];
    t_out = 1;
    while (fgets(line, sizeof(line), stdin) != NULL) {
        execute(line);
    }
    return NULL;
}

static void *client_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    FILE *in = fdopen(dup(fd), "r");
    char line[4096];
    t_out = fd;
    while (in != NULL && fgets(line, sizeof(line), in) != NULL) {
        execute(line);
    }
    if (in != NULL) {
        fclose(in);
    }
    close(fd);
    return NULL;
}

static void *listen_thread(void *arg)
{
    int listener = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        pthread_t t;
        if (pthread_create(&t, NULL, client_thread, (void *)(intptr_t)fd) == 0) {
            pthread_detach(t);
        } else {
            close(fd);
        }
    }
    return NULL;
}

void emu_console_start(int port, bool use_stdin)
{
    pthread_t t;
    if (use_stdin && pthread_create(&t, NULL, stdin_thread, NULL) == 0) {
        pthread_detach(t);
    }
    if (port <= 0) {
        return;
    }
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(listener, 4) < 0) {
        EMU_LOGW(TAG, "no console on 127.0.0.1:%d: %s", port, strerror(errno));
        if (listener >= 0) {
            close(listener);
        }
        return;
    }
    if (pthread_create(&t, NULL, listen_thread, (void *)(intptr_t)listener) == 0) {
        pthread_detach(t);
        EMU_LOGI(TAG, "console on 127.0.0.1:%d", port);
    }
}
