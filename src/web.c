/*
 * The boards' web page and its API (esp32emu's web/index.html, built in),
 * on http://<the Pi>:<port>/ (--web, default 8080; every address, as the
 * boards' page is on the network):
 *
 *   GET    /                    the page
 *   GET    /machine.js          the machine's script for the page, if it has one
 *   GET    /api/info            machine name, drives, image kinds, blank disk sizes
 *   GET    /api/disks           {"ready", "free", "disks": [{"name", "size", "drive"[, "folder"]}]}
 *   GET    /api/status          machine, drives, offers, the computer
 *   PUT    /api/drive/<n>       insert the image named in the body into drive n
 *   DELETE /api/drive/<n>       eject drive n
 *   POST   /api/drive/<n>/offer take what the machine offers for drive n's disk
 *   POST   /api/new?size=<bytes>  make a blank disk image
 *   POST   /api/reset           cold-reset the machine
 *   PUT    /disks/<name>        upload (raw body; also POST), replaces an image
 *                               of that name unless it's in a drive
 *   GET    /disks/<name>        download
 *   DELETE /disks/<name>        delete, unless it's in a drive
 *
 * /floppies/<name> works as /disks/<name>. A thread a connection, one
 * request each (Connection: close). Uploads go to <name>.tmp first.
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
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "emu/disks.h"
#include "emu/emu.h"
#include "emu/json.h"
#include "emu/log.h"
#include "emu_internal.h"

static const char *TAG = "web";

extern const unsigned char rpi5emu_index_html[];
extern const size_t rpi5emu_index_html_size;

#define HEADER_MAX  16384
#define CHUNK       65536

typedef struct {
    int fd;
    char method[8];
    char path[2048];            /* decoded later, per route */
    char query[512];
    long long length;           /* Content-Length, -1 none */
    char *body;                 /* what came with the header */
    size_t body_have;
} req_t;

/* ---- Answers ---- */

static bool send_all(int fd, const void *p, size_t n)
{
    const char *s = p;
    while (n > 0) {
        ssize_t w = send(fd, s, n, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        s += w;
        n -= w;
    }
    return true;
}

static void respond(req_t *r, const char *status, const char *type, const void *body, size_t len,
                    const char *extra)
{
    char head[1024];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n%s\r\n",
                     status, type, len, extra ? extra : "");
    send_all(r->fd, head, n);
    if (len && strcmp(r->method, "HEAD") != 0) {
        send_all(r->fd, body, len);
    }
}

static void text(req_t *r, const char *status, const char *s)
{
    respond(r, status, "text/plain; charset=utf-8", s, strlen(s), NULL);
}

static void json(req_t *r, emu_json_t *j)
{
    respond(r, "200 OK", "application/json", j->buf, j->len, NULL);
}

/* ---- The request ---- */

static bool url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        if (*s == '%') {
            unsigned v;
            if (!s[1] || !s[2] || sscanf(s + 1, "%2x", &v) != 1) {
                return false;
            }
            *out++ = v;
            s += 2;
        } else {
            *out++ = *s;
        }
    }
    *out = 0;
    return true;
}

/* The body (Content-Length bytes) into buf; false if it isn't all there */
static bool body(req_t *r, char *buf, size_t len)
{
    if (r->length < 0 || (size_t)r->length != len) {
        return false;
    }
    size_t got = r->body_have < len ? r->body_have : len;
    memcpy(buf, r->body, got);
    while (got < len) {
        ssize_t n = recv(r->fd, buf + got, len - got, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        got += n;
    }
    return true;
}

static bool read_request(req_t *r, char *buf)
{
    size_t have = 0;
    char *end = NULL;
    while (end == NULL && have < HEADER_MAX - 1) {
        ssize_t n = recv(r->fd, buf + have, HEADER_MAX - 1 - have, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        have += n;
        buf[have] = 0;
        end = strstr(buf, "\r\n\r\n");
    }
    if (end == NULL) {
        return false;
    }
    *end = 0;
    r->body = end + 4;
    r->body_have = buf + have - r->body;
    char target[sizeof(r->path)];
    if (sscanf(buf, "%7s %2047s", r->method, target) != 2) {
        return false;
    }
    char *q = strchr(target, '?');
    if (q) {
        *q = 0;
        snprintf(r->query, sizeof(r->query), "%s", q + 1);
    }
    snprintf(r->path, sizeof(r->path), "%s", target);
    r->length = -1;
    for (char *line = strstr(buf, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        if (strncasecmp(line + 2, "Content-Length:", 15) == 0) {
            r->length = atoll(line + 17);
        }
    }
    return true;
}

/* ---- What the machine is ---- */

static void info_get(req_t *r)
{
    static char buf[8192];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    emu_json_t j;
    emu_json_begin(&j, buf, sizeof(buf));
    const emu_machine_t *m = emu_machine();
    char hostname[96] = "";
    gethostname(hostname, sizeof(hostname) - 1);
    emu_json_str(&j, "name", m->name);
    emu_json_str(&j, "short_name", m->short_name);
    emu_json_str(&j, "hostname", hostname);
    emu_json_str(&j, "board", "Raspberry Pi");
    emu_json_str(&j, "host", "Pi");
    emu_json_str(&j, "storage", "data folder");
    emu_json_str(&j, "storage_title", "In the data folder");
    char about[1400], shown[1200];
    snprintf(about, sizeof(about), "The emulator on the Raspberry Pi (%s), and the disk images in its data folder, %s/%s.",
             hostname, emu_short_path(emu_data_dir(), shown, sizeof(shown)), m->disks ? disks_config()->folder : "");
    emu_json_str(&j, "about", about);
    emu_json_bool(&j, "script", m->web_script != NULL);
    emu_json_object(&j, "disks");
    if (m->disks) {
        const disks_config_t *d = disks_config();
        emu_json_str(&j, "title", d->title);
        emu_json_str(&j, "description", d->description);
        emu_json_str(&j, "folder", d->folder);
        emu_json_int(&j, "max_upload", (long long)d->max_upload);
        emu_json_bool(&j, "can_create", d->create != NULL);
        emu_json_array(&j, "drives");
        for (int i = 0; i < d->drives; i++) {
            emu_json_str(&j, NULL, d->drive_names[i]);
        }
        emu_json_end_array(&j);
        emu_json_array(&j, "extensions");
        for (const char *const *e = d->extensions; e && *e; e++) {
            emu_json_str(&j, NULL, *e);
        }
        emu_json_end_array(&j);
        emu_json_array(&j, "sizes");
        for (int i = 0; i < d->size_count; i++) {
            emu_json_object(&j, NULL);
            emu_json_int(&j, "bytes", d->sizes[i].bytes);
            emu_json_str(&j, "label", d->sizes[i].label);
            emu_json_end_object(&j);
        }
        emu_json_end_array(&j);
    } else {
        emu_json_array(&j, "drives");
        emu_json_end_array(&j);
        emu_json_array(&j, "extensions");
        emu_json_end_array(&j);
        emu_json_array(&j, "sizes");
        emu_json_end_array(&j);
    }
    emu_json_end_object(&j);
    emu_json_end_object(&j);
    json(r, &j);
    pthread_mutex_unlock(&lock);
}

static void list_get(req_t *r)
{
    char **names = NULL;
    int n = emu_machine()->disks ? disks_list(&names) : 0;
    struct statvfs vfs;
    unsigned long long free_bytes = 0;
    if (emu_machine()->disks && disks_ready() && statvfs(disks_folder(), &vfs) == 0) {
        free_bytes = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
    }
    size_t size = 256 + (size_t)n * (DISKS_NAME_LEN + 128);
    char *buf = malloc(size);
    if (buf == NULL) {
        disks_free_list(names, n);
        text(r, "500 Internal Server Error", "Out of memory");
        return;
    }
    emu_json_t j;
    emu_json_begin(&j, buf, size);
    emu_json_bool(&j, "ready", emu_machine()->disks && disks_ready());
    emu_json_int(&j, "free", (long long)free_bytes);
    emu_json_array(&j, "disks");
    for (int i = 0; i < n; i++) {
        char path[DISKS_NAME_LEN + 1100];
        struct stat st;
        bool folder = names[i][0] && names[i][strlen(names[i]) - 1] == '/';
        snprintf(path, sizeof(path), "%s/%s", disks_folder(), names[i]);
        long long bytes = !folder && stat(path, &st) == 0 ? (long long)st.st_size : 0;
        const char *drive = "";
        for (int d = 0; d < disks_config()->drives; d++) {
            if (disks_name(d) != NULL && strcmp(disks_name(d), names[i]) == 0) {
                drive = disks_config()->drive_names[d];
            }
        }
        emu_json_object(&j, NULL);
        emu_json_str(&j, "name", names[i]);
        emu_json_int(&j, "size", bytes);
        emu_json_str(&j, "drive", drive);
        if (folder) {
            emu_json_bool(&j, "folder", true);
        }
        emu_json_end_object(&j);
    }
    emu_json_end_array(&j);
    emu_json_end_object(&j);
    json(r, &j);
    free(buf);
    disks_free_list(names, n);
}

static void status_get(req_t *r)
{
    char *buf = malloc(32768);
    if (buf == NULL) {
        text(r, "500 Internal Server Error", "Out of memory");
        return;
    }
    emu_status_json(buf, 32768);
    respond(r, "200 OK", "application/json", buf, strlen(buf), NULL);
    free(buf);
}

/* ---- Drives ---- */

typedef struct {
    int drive;
    const char *name;           /* NULL: eject */
    bool ok;
} drive_op_t;

static void drive_op(void *arg)
{
    drive_op_t *op = arg;
    if (op->name == NULL) {
        disks_eject(op->drive);
        op->ok = true;
    } else {
        op->ok = disks_insert(op->drive, op->name);
    }
}

static int drive_number(req_t *r)
{
    int n = atoi(r->path + strlen("/api/drive/"));
    return emu_machine()->disks && n >= 1 && n <= disks_config()->drives ? n - 1 : -1;
}

static void drive_put(req_t *r)
{
    int d = drive_number(r);
    char name[DISKS_NAME_LEN];
    if (d < 0 || r->length <= 0 || r->length >= (long long)sizeof(name) || !body(r, name, r->length)) {
        text(r, "400 Bad Request", "PUT /api/drive/<number> with a disk image name");
        return;
    }
    name[r->length] = 0;
    if (!disks_can_insert(name)) {
        text(r, "400 Bad Request", "Not a disk image name");
        return;
    }
    drive_op_t op = { d, name, false };
    emu_paused(drive_op, &op);
    if (!op.ok) {
        text(r, "422 Unprocessable Entity", "Not a disk image the machine can use");
        return;
    }
    EMU_LOGI(TAG, "%s into %s", name, disks_config()->drive_names[d]);
    ui_touch();
    text(r, "200 OK", "OK");
}

static void drive_delete(req_t *r)
{
    int d = drive_number(r);
    if (d < 0) {
        text(r, "400 Bad Request", "No such drive");
        return;
    }
    drive_op_t op = { d, NULL, false };
    emu_paused(drive_op, &op);
    ui_touch();
    text(r, "200 OK", "OK");
}

static void drive_post(req_t *r)
{
    int d = drive_number(r);
    if (d < 0 || strstr(r->path, "/offer") == NULL) {
        text(r, "400 Bad Request", "POST /api/drive/<number>/offer");
        return;
    }
    if (disks_offer(d) == NULL) {
        text(r, "404 Not Found", "Nothing offered for that drive's disk");
        return;
    }
    if (!emu_take_offer(d)) {
        text(r, "409 Conflict", "The machine couldn't take it");
        return;
    }
    EMU_LOGI(TAG, "%s: its disk's offer taken", disks_config()->drive_names[d]);
    text(r, "200 OK", "OK");
}

static void reset_post(req_t *r)
{
    if (!emu_started()) {
        text(r, "409 Conflict", "The machine has not started yet");
        return;
    }
    emu_request_reset();
    EMU_LOGI(TAG, "reset");
    text(r, "200 OK", "OK");
}

static void new_post(req_t *r)
{
    if (emu_machine()->disks == NULL || !disks_ready()) {
        text(r, "400 Bad Request", "No disk folder");
        return;
    }
    uint32_t bytes = disks_default_size(-1);
    const char *size = strstr(r->query, "size=");
    if (size) {
        bytes = strtoul(size + 5, NULL, 10);
    }
    bool known = false;
    for (int i = 0; i < disks_config()->size_count; i++) {
        known |= disks_config()->sizes[i].bytes == bytes;
    }
    if (!known) {
        text(r, "400 Bad Request", "No such disk size");
        return;
    }
    char name[DISKS_NAME_LEN];
    if (!disks_create(-1, bytes, name, sizeof(name))) {
        text(r, "500 Internal Server Error", "Couldn't make the disk image");
        return;
    }
    text(r, "201 Created", name);
}

/* ---- Disk image files ---- */

/* The name after /disks/ (or /floppies/), checked; false sends the error */
static bool disk_name(req_t *r, char *name, size_t len)
{
    const char *p = strchr(r->path + 1, '/');
    snprintf(name, len, "%s", p ? p + 1 : "");
    if (!url_decode(name) || !disks_valid_name(name)) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Not a disk image name: %s", disks_config()->description);
        text(r, "400 Bad Request", msg);
        return false;
    }
    return true;
}

static void disk_put(req_t *r)
{
    char name[DISKS_NAME_LEN], path[DISKS_NAME_LEN + 1100], tmp[DISKS_NAME_LEN + 1110];
    if (!disk_name(r, name, sizeof(name))) {
        return;
    }
    if (disks_in_use(name)) {
        snprintf(path, sizeof(path), "%.100s is in a drive: eject it first", name);
        text(r, "409 Conflict", path);
        return;
    }
    if (r->length <= 0 || (disks_config()->max_upload && (uint64_t)r->length > disks_config()->max_upload)) {
        text(r, "400 Bad Request", "Empty, or too big for a disk image");
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", disks_folder(), name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        text(r, "500 Internal Server Error", "Can't write to the data folder");
        return;
    }
    char *buf = malloc(CHUNK);
    long long left = r->length;
    size_t first = r->body_have < (size_t)left ? r->body_have : (size_t)left;
    bool ok = buf != NULL && fwrite(r->body, 1, first, f) == first;
    left -= first;
    while (ok && left > 0) {
        ssize_t n = recv(r->fd, buf, left < CHUNK ? left : CHUNK, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        ok = n > 0 && fwrite(buf, 1, n, f) == (size_t)n;
        left -= n > 0 ? n : 0;
    }
    free(buf);
    ok = fclose(f) == 0 && ok;
    if (ok) {
        disks_removed(name);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        EMU_LOGW(TAG, "upload of %s failed", name);
        text(r, "500 Internal Server Error", "Upload failed");
        return;
    }
    EMU_LOGI(TAG, "received %s, %lld bytes", name, r->length);
    ui_touch();
    text(r, "201 Created", "OK");
}

static void disk_get(req_t *r)
{
    char name[DISKS_NAME_LEN], path[DISKS_NAME_LEN + 1100];
    if (!disk_name(r, name, sizeof(name))) {
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", disks_folder(), name);
    FILE *f = fopen(path, "rb");
    struct stat st;
    if (f == NULL || fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        if (f) {
            fclose(f);
        }
        text(r, "404 Not Found", "No such disk");
        return;
    }
    char head[DISKS_NAME_LEN + 300];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lld\r\n"
                     "Content-Disposition: attachment; filename=\"%.200s\"\r\nConnection: close\r\n\r\n",
                     (long long)st.st_size, name);
    char *buf = malloc(CHUNK);
    bool ok = buf && send_all(r->fd, head, n);
    size_t got;
    while (ok && (got = fread(buf, 1, CHUNK, f)) > 0) {
        ok = send_all(r->fd, buf, got);
    }
    free(buf);
    fclose(f);
}

static void disk_delete(req_t *r)
{
    char name[DISKS_NAME_LEN], path[DISKS_NAME_LEN + 1100];
    if (!disk_name(r, name, sizeof(name))) {
        return;
    }
    if (disks_in_use(name)) {
        snprintf(path, sizeof(path), "%.100s is in a drive: eject it first", name);
        text(r, "409 Conflict", path);
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", disks_folder(), name);
    disks_removed(name);
    if (remove(path) != 0) {
        text(r, "404 Not Found", "No such disk");
        return;
    }
    EMU_LOGI(TAG, "deleted %s", name);
    ui_touch();
    text(r, "200 OK", "OK");
}

/* ---- The server ---- */

static bool prefix(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

static void route(req_t *r)
{
    const char *m = r->method, *p = r->path;
    bool get = strcmp(m, "GET") == 0 || strcmp(m, "HEAD") == 0, put = strcmp(m, "PUT") == 0,
         post = strcmp(m, "POST") == 0, del = strcmp(m, "DELETE") == 0;
    bool disks = emu_machine()->disks != NULL && disks_ready();
    if (get && (strcmp(p, "/") == 0 || strcmp(p, "/index.html") == 0)) {
        respond(r, "200 OK", "text/html; charset=utf-8", rpi5emu_index_html, rpi5emu_index_html_size, NULL);
    } else if (get && strcmp(p, "/machine.js") == 0) {
        const emu_machine_t *mc = emu_machine();
        if (mc->web_script) {
            respond(r, "200 OK", "text/javascript; charset=utf-8", mc->web_script,
                    mc->web_script_end - mc->web_script, NULL);
        } else {
            text(r, "404 Not Found", "No script");
        }
    } else if (get && strcmp(p, "/api/info") == 0) {
        info_get(r);
    } else if (get && (strcmp(p, "/api/disks") == 0 || strcmp(p, "/api/floppies") == 0)) {
        list_get(r);
    } else if (get && strcmp(p, "/api/status") == 0) {
        status_get(r);
    } else if (prefix(p, "/api/drive/") && (put || del || post)) {
        put ? drive_put(r) : del ? drive_delete(r) : drive_post(r);
    } else if (post && strcmp(p, "/api/new") == 0) {
        new_post(r);
    } else if (post && strcmp(p, "/api/reset") == 0) {
        reset_post(r);
    } else if ((prefix(p, "/disks/") || prefix(p, "/floppies/")) && (put || post || get || del)) {
        if (!disks) {
            text(r, "500 Internal Server Error", "No disk folder");
        } else {
            put || post ? disk_put(r) : get ? disk_get(r) : disk_delete(r);
        }
    } else if (get && strcmp(p, "/favicon.ico") == 0) {
        respond(r, "204 No Content", "text/plain", "", 0, NULL);
    } else {
        text(r, "404 Not Found", "Not here");
    }
}

static void *client_thread(void *arg)
{
    req_t *r = arg;
    char *buf = malloc(HEADER_MAX);
    struct timeval tv = { 30, 0 };
    setsockopt(r->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (buf && read_request(r, buf)) {
        route(r);
    }
    free(buf);
    shutdown(r->fd, SHUT_WR);
    close(r->fd);
    free(r);
    return NULL;
}

static void *listen_thread(void *arg)
{
    int listener = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            break;
        }
        req_t *r = calloc(1, sizeof(req_t));
        pthread_t t;
        if (r == NULL) {
            close(fd);
            continue;
        }
        r->fd = fd;
        if (pthread_create(&t, NULL, client_thread, r) == 0) {
            pthread_detach(t);
        } else {
            close(fd);
            free(r);
        }
    }
    return NULL;
}

void emu_web_start(int port)
{
    if (port <= 0) {
        return;
    }
    int listener = socket(AF_INET6, SOCK_STREAM, 0);
    bool v6 = listener >= 0;
    if (!v6) {
        listener = socket(AF_INET, SOCK_STREAM, 0);
    }
    int one = 1, zero = 0;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int bound;
    if (v6) {
        setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));      /* IPv4 too */
        struct sockaddr_in6 addr = { .sin6_family = AF_INET6, .sin6_port = htons((uint16_t)port),
                                     .sin6_addr = in6addr_any };
        bound = bind(listener, (struct sockaddr *)&addr, sizeof(addr));
    } else {
        struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                    .sin_addr.s_addr = htonl(INADDR_ANY) };
        bound = bind(listener, (struct sockaddr *)&addr, sizeof(addr));
    }
    if (listener < 0 || bound < 0 || listen(listener, 16) < 0) {
        EMU_LOGW(TAG, "no web page on port %d: %s", port, strerror(errno));
        if (listener >= 0) {
            close(listener);
        }
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, listen_thread, (void *)(intptr_t)listener) == 0) {
        pthread_detach(t);
        char hostname[96] = "";
        gethostname(hostname, sizeof(hostname) - 1);
        EMU_LOGI(TAG, "web page on http://%s.local:%d/", hostname, port);
    }
}
