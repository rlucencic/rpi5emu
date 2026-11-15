/*
 * Settings in ~/.config/rpi5emu/settings.ini: "[section]" lines, then
 * "key=value" lines (the value is the rest of the line). Kept in memory,
 * the whole file written again at each change.
 *
 * Several of the family's programs may run at once and share the file: a
 * change takes a lock on the file's folder (flock), reads the file again,
 * makes the change and writes it through a temporary file of its own, so
 * no program's change undoes another's. Reads come from memory: what this
 * program read last, its own changes included.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "emu/log.h"
#include "emu/settings.h"

static const char *TAG = "settings";

typedef struct {
    char *ns, *key, *value;
} entry_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static entry_t *s_entries;
static int s_count, s_cap;
static char s_path[1024];
static bool s_loaded;

static void make_dirs(const char *path)
{
    char dir[sizeof(s_path)];
    snprintf(dir, sizeof(dir), "%s", path);
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(dir, 0755);
            *p = '/';
        }
    }
}

static entry_t *find(const char *ns, const char *key)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].ns, ns) == 0 && strcmp(s_entries[i].key, key) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

static void put(const char *ns, const char *key, const char *value)
{
    entry_t *e = find(ns, key);
    if (e != NULL) {
        free(e->value);
        e->value = strdup(value);
        return;
    }
    if (s_count == s_cap) {
        int cap = s_cap ? s_cap * 2 : 32;
        entry_t *n = realloc(s_entries, cap * sizeof(entry_t));
        if (n == NULL) {
            return;
        }
        s_entries = n;
        s_cap = cap;
    }
    s_entries[s_count++] = (entry_t){ strdup(ns), strdup(key), strdup(value) };
}

/* The file as it is now, in place of what was read before (none there yet:
 * what's in memory stays)
 */
static void read_file(void)
{
    FILE *f = fopen(s_path, "r");
    if (f == NULL) {
        return;
    }
    for (int i = 0; i < s_count; i++) {
        free(s_entries[i].ns);
        free(s_entries[i].key);
        free(s_entries[i].value);
    }
    s_count = 0;
    char line[2048], ns[2048] = "";
    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end != NULL) {
                *end = 0;
                snprintf(ns, sizeof(ns), "%s", line + 1);
            }
            continue;
        }
        char *eq = strchr(line, '=');
        if (eq == NULL || line[0] == '#' || line[0] == ';' || ns[0] == 0) {
            continue;
        }
        *eq = 0;
        put(ns, line, eq + 1);
    }
    fclose(f);
}

static void load(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && xdg[0] == '/') {
        snprintf(s_path, sizeof(s_path), "%s/rpi5emu/settings.ini", xdg);
    } else {
        const char *home = getenv("HOME");
        snprintf(s_path, sizeof(s_path), "%s/.config/rpi5emu/settings.ini", home ? home : ".");
    }
    read_file();
}

/* (under s_lock) The file read again under the lock on its folder, held
 * until end_change(): -1 if the folder can't be locked (then the change
 * goes ahead as before, unguarded)
 */
static int begin_change(void)
{
    load();
    make_dirs(s_path);
    char dir[sizeof(s_path)];
    snprintf(dir, sizeof(dir), "%s", s_path);
    char *slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = 0;
    }
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    while (fd >= 0 && flock(fd, LOCK_EX) != 0) {
        if (errno != EINTR) {
            EMU_LOGW(TAG, "can't lock %s: %s", dir, strerror(errno));
            close(fd);
            fd = -1;
        }
    }
    read_file();
    return fd;
}

static void end_change(int fd)
{
    if (fd >= 0) {
        close(fd); /* the lock goes with it */
    }
}

static int compare_entries(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    int c = strcmp(x->ns, y->ns);
    return c ? c : strcmp(x->key, y->key);
}

static void save(void)
{
    /* a temporary file of this program's own: settings.ini.XXXXXX */
    char tmp[sizeof(s_path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.XXXXXX", s_path);
    int fd = mkstemp(tmp);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (f == NULL) {
        EMU_LOGW(TAG, "can't write %s: %s", s_path, strerror(errno));
        if (fd >= 0) {
            close(fd);
            unlink(tmp);
        }
        return;
    }
    fchmod(fd, 0644);
    qsort(s_entries, s_count, sizeof(entry_t), compare_entries);
    const char *ns = NULL;
    for (int i = 0; i < s_count; i++) {
        if (ns == NULL || strcmp(ns, s_entries[i].ns) != 0) {
            ns = s_entries[i].ns;
            fprintf(f, "%s[%s]\n", i ? "\n" : "", ns);
        }
        fprintf(f, "%s=%s\n", s_entries[i].key, s_entries[i].value);
    }
    if (fclose(f) != 0 || rename(tmp, s_path) != 0) {
        EMU_LOGW(TAG, "can't write %s: %s", s_path, strerror(errno));
        unlink(tmp);
    }
}

bool emu_setting_get_str(const char *ns, const char *key, char *buf, size_t len)
{
    pthread_mutex_lock(&s_lock);
    load();
    entry_t *e = find(ns, key);
    if (e != NULL && len > 0) {
        snprintf(buf, len, "%s", e->value);
    }
    pthread_mutex_unlock(&s_lock);
    return e != NULL;
}

bool emu_setting_get_int(const char *ns, const char *key, int32_t *value)
{
    char text[32], *end;
    if (!emu_setting_get_str(ns, key, text, sizeof(text))) {
        return false;
    }
    long v = strtol(text, &end, 0);
    if (end == text) {
        return false;
    }
    *value = (int32_t)v;
    return true;
}

void emu_setting_set_str(const char *ns, const char *key, const char *value)
{
    pthread_mutex_lock(&s_lock);
    /* No line breaks in a value: the file is one per line */
    char clean[2048];
    snprintf(clean, sizeof(clean), "%s", value ? value : "");
    for (char *p = clean; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            *p = ' ';
        }
    }
    int lock = begin_change();
    entry_t *e = find(ns, key);
    if (e == NULL || strcmp(e->value, clean) != 0) {
        put(ns, key, clean);
        save();
    }
    end_change(lock);
    pthread_mutex_unlock(&s_lock);
}

void emu_setting_erase(const char *ns, const char *key)
{
    pthread_mutex_lock(&s_lock);
    int lock = begin_change();
    entry_t *e = find(ns, key);
    if (e != NULL) {
        free(e->ns);
        free(e->key);
        free(e->value);
        *e = s_entries[--s_count];
        save();
    }
    end_change(lock);
    pthread_mutex_unlock(&s_lock);
}

void emu_setting_set_int(const char *ns, const char *key, int32_t value)
{
    char text[16];
    snprintf(text, sizeof(text), "%ld", (long)value);
    emu_setting_set_str(ns, key, text);
}

const char *emu_settings_path(void)
{
    pthread_mutex_lock(&s_lock);
    load();
    pthread_mutex_unlock(&s_lock);
    return s_path;
}
