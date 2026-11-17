/*
 * The runtime on the Pi: options, the window and its picture (the main
 * thread: SDL wants its video there), the emulation thread, sound, input,
 * the stats thread. See emu/emu.h.
 */
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/statvfs.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "emu/console.h"
#include "emu/emu.h"
#include "emu/log.h"
#include "emu/settings.h"
#include "emu_internal.h"
#include "emudbg.h"

static const char *TAG = "emu";

static const emu_machine_t *M;

/* ---- Options ---- */

#define MAX_LIST    16

static struct {
    const char *data_dir;
    double scale;               /* the window: view_width x 600 times this */
    bool fullscreen;
    bool no_sound;
    bool no_stdin;
    bool start;                 /* straight in, no setup screen first */
    int console_port;           /* 0: none */
    int debug_port;             /* 0: none */
    int web_port;               /* 0: none */
    const char *choices[MAX_LIST];
    int choice_count;
    const char *images[MAX_LIST];
    int image_count;
} opt = { .scale = 1.0, .console_port = 6400, .web_port = 8080 };

/* ---- Log ---- */

bool emu_log_verbose;

void emu_log(char level, const char *tag, const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    fprintf(stderr, "%c (%s) %s\n", level, tag, text);
}

/* ---- Time ---- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void sleep_until(uint64_t ns)
{
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

static double thread_cpu_s(pthread_t thread)
{
    clockid_t cid;
    struct timespec ts;
    if (pthread_getcpuclockid(thread, &cid) != 0 || clock_gettime(cid, &ts) != 0) {
        return 0;
    }
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- State ---- */

static char s_data_dir[1024];
static atomic_bool s_started;           /* start() succeeded */
static atomic_bool s_quit;
static atomic_bool s_reset_requested;
static atomic_bool s_warp;              /* run as fast as it goes (Ctrl+Alt+W) */
static atomic_uint s_frames;            /* frames emulated */
static uint64_t s_session_start;

/* The emulation thread and its pauses */
static pthread_t s_emu_thread;
static bool s_thread_running;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static bool s_pause_request, s_paused;
static pthread_mutex_t s_pause_lock;    /* recursive: one emu_paused() at a time */
static int s_pause_depth;
static __thread bool t_emulation_thread;

/* The presenter: render_line() only while this says so (not during a start) */
static pthread_mutex_t s_present_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_present_on;

static pthread_mutex_t s_info_lock = PTHREAD_MUTEX_INITIALIZER;
static emu_info_t s_info = { .state = EMU_SETUP, .temperature = NAN, .sd_card = true };
static char s_message[512], s_drive_info[256];
static atomic_uint s_stats_seq;         /* s_info renewed */

const emu_machine_t *emu_machine(void)
{
    return M;
}

const char *emu_data_dir(void)
{
    return s_data_dir;
}

long emu_rom_read(const char *name, void *buf, size_t max)
{
    char path[1400];
    snprintf(path, sizeof(path), "%s/roms/%s", s_data_dir, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        for (char *p = path + strlen(path) - strlen(name); *p; p++) {
            *p = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p;
        }
        f = fopen(path, "rb");
    }
    if (f == NULL) {
        return -1;
    }
    long n = (long)fread(buf, 1, max, f);
    fclose(f);
    if (n > 0) {
        EMU_LOGI(TAG, "ROM %s", path);
    }
    return n;
}

bool emu_started(void)
{
    return s_started;
}

void emu_quit(void)
{
    s_quit = true;
    SDL_Event e = { .type = SDL_EVENT_QUIT };
    SDL_PushEvent(&e);
}

void emu_set_drive_info(const char *text)
{
    pthread_mutex_lock(&s_info_lock);
    snprintf(s_drive_info, sizeof(s_drive_info), "%s", text ? text : "");
    pthread_mutex_unlock(&s_info_lock);
}

const char *emu_drive_info(void)
{
    return s_drive_info;
}

static atomic_bool s_redraw;            /* the window needs drawing again */

void emu_set_message(const char *text)
{
    pthread_mutex_lock(&s_info_lock);
    snprintf(s_message, sizeof(s_message), "%s", text ? text : "");
    pthread_mutex_unlock(&s_info_lock);
    if (text && text[0]) {
        EMU_LOGW(TAG, "%s", text);
    }
    s_redraw = true;
}

const char *emu_message(void)
{
    return s_message;
}

void emu_request_reset(void)
{
    s_reset_requested = true;
}

void emu_measure_temperature(void)
{
}

void emu_info(emu_info_t *info)
{
    pthread_mutex_lock(&s_info_lock);
    *info = s_info;
    pthread_mutex_unlock(&s_info_lock);
    pthread_mutex_lock(&s_lock);
    bool paused = s_paused && s_pause_request;
    pthread_mutex_unlock(&s_lock);
    info->state = !s_started ? EMU_SETUP : paused ? EMU_PAUSED : EMU_RUNNING;
    if (info->state == EMU_RUNNING && M->halted && M->halted()) {
        info->state = EMU_HALTED;
    }
    if (info->state == EMU_RUNNING && emudbg_stopped()) {
        info->state = EMU_STOPPED;
    }
    info->session_s = s_started ? (uint32_t)((now_ns() - s_session_start) / 1000000000ull) : 0;
}

/* ---- Sound ----
 *
 * The machine's samples go into an SDL audio stream after each frame; the
 * clock paces the frames, and the stream's rate is nudged (half a percent
 * at most) to keep about MAX_QUEUE / 4 queued: the sound card's clock and
 * ours drift apart.
 */

static SDL_AudioStream *s_audio;
static int s_volume = 77;               /* 0-100; the samples scaled by its square (0.6 by default, as the boards) */
static bool s_muted;
static float s_queue_avg;

static void audio_gain(void)
{
    if (s_audio != NULL) {
        float v = s_volume / 100.0f;
        SDL_SetAudioStreamGain(s_audio, s_muted ? 0.0f : v * v);
    }
}

int emu_volume(void)
{
    return s_volume;
}

bool emu_muted(void)
{
    return s_muted;
}

void emu_set_volume(int volume)
{
    s_volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    s_muted = false;
    audio_gain();
    emu_setting_set_int("speaker", "volume", s_volume);
    emu_setting_set_int("speaker", "muted", 0);
}

void emu_set_muted(bool muted)
{
    s_muted = muted;
    audio_gain();
    emu_setting_set_int("speaker", "muted", muted);
}

static void audio_open(void)
{
    int32_t v;
    if (emu_setting_get_int("speaker", "volume", &v)) {
        s_volume = v < 0 ? 0 : v > 100 ? 100 : v;
    }
    if (emu_setting_get_int("speaker", "muted", &v)) {
        s_muted = v != 0;
    }
    if (M->audio_render == NULL || M->audio_hz <= 0 || opt.no_sound) {
        return;
    }
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, M->audio_hz };
    s_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (s_audio == NULL) {
        EMU_LOGW(TAG, "no sound: %s", SDL_GetError());
        return;
    }
    audio_gain();
    SDL_ResumeAudioStreamDevice(s_audio);
    if (M->audio_enable) {
        M->audio_enable(true);
    }
    EMU_LOGI(TAG, "sound: %d Hz through %s", M->audio_hz, SDL_GetCurrentAudioDriver());
}

static void audio_pump(void)
{
    if (M->audio_render == NULL || s_audio == NULL) {
        return;
    }
    static int16_t buf[2 * 2048];
    for (int i = 0; i < 64; i++) {
        int n = M->audio_render(buf, 2048);
        if (n <= 0) {
            break;
        }
        if (!s_warp) {
            SDL_PutAudioStreamData(s_audio, buf, n * 4);
        }
        if (n < 2048) {
            break;
        }
    }
    int queued = SDL_GetAudioStreamQueued(s_audio) / 4;
    int target = M->audio_hz / 16;      /* ~60 ms */
    if (queued > target * 8) {
        /* The device isn't taking it (none there, or held): start over */
        SDL_ClearAudioStream(s_audio);
        s_queue_avg = 0;
        return;
    }
    s_queue_avg += (queued - s_queue_avg) * 0.05f;
    float ratio = 1.0f + 0.005f * (s_queue_avg - target) / target;
    ratio = ratio < 0.995f ? 0.995f : ratio > 1.005f ? 1.005f : ratio;
    SDL_SetAudioStreamFrequencyRatio(s_audio, ratio);
}

/* ---- The emulation thread ---- */

static Uint32 s_frame_event;            /* wakes the main thread */
static atomic_bool s_frame_event_pending;

static void frame_done(void)
{
    if (!atomic_exchange(&s_frame_event_pending, true)) {
        SDL_Event e = { .type = s_frame_event };
        SDL_PushEvent(&e);
    }
}

static void *emulation_thread(void *arg)
{
    t_emulation_thread = true;
    uint64_t next = now_ns();
    while (!s_quit) {
        pthread_mutex_lock(&s_lock);
        if (s_pause_request || !s_started) {
            s_paused = true;
            pthread_cond_broadcast(&s_cond);
            while ((s_pause_request || !s_started) && !s_quit) {
                pthread_cond_wait(&s_cond, &s_lock);
            }
            s_paused = false;
            next = now_ns();
        }
        pthread_mutex_unlock(&s_lock);
        if (s_quit) {
            break;
        }
        if (atomic_exchange(&s_reset_requested, false)) {
            M->reset();
            emudbg_rearm();
        }
        M->run_frame();
        s_frames++;
        emudbg_frame_end();
        audio_pump();
        frame_done();

        emu_screen_t mode = { 0 };
        M->screen_mode(&mode);
        int hz = mode.hz > 0 ? mode.hz : 50;
        next += 1000000000ull / hz;
        uint64_t now = now_ns();
        if (s_warp) {
            next = now;
        } else if (next > now) {
            sleep_until(next);
        } else if (now - next > 100000000ull) {
            next = now;                 /* too far behind: don't try to catch up */
        }
    }
    pthread_mutex_lock(&s_lock);
    s_paused = true;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

static void launch_emulation(void)
{
    if (s_thread_running) {
        return;
    }
    if (pthread_create(&s_emu_thread, NULL, emulation_thread, NULL) != 0) {
        EMU_LOGE(TAG, "can't start the emulation thread");
        return;
    }
    s_thread_running = true;
}

/* Holds on the emulation (emu_paused(), the setup screen): the first stops
 * it between frames, and the presenter's drawing; the last lets it go on
 */
static pthread_mutex_t s_hold_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_holds;

static void hold_emulation(void)
{
    pthread_mutex_lock(&s_hold_lock);
    if (s_holds++ == 0) {
        pthread_mutex_lock(&s_present_lock);
        s_present_on = false;
        pthread_mutex_unlock(&s_present_lock);
        pthread_mutex_lock(&s_lock);
        s_pause_request = true;
        pthread_mutex_unlock(&s_lock);
        emudbg_suspend();       /* out of a debugger stop: on to the frame's end */
        pthread_mutex_lock(&s_lock);
        while (s_thread_running && !s_paused) {
            pthread_cond_wait(&s_cond, &s_lock);
        }
        pthread_mutex_unlock(&s_lock);
    }
    pthread_mutex_unlock(&s_hold_lock);
}

static void release_emulation(void)
{
    pthread_mutex_lock(&s_hold_lock);
    if (s_holds > 0 && --s_holds == 0) {
        pthread_mutex_lock(&s_lock);
        s_pause_request = false;
        pthread_cond_broadcast(&s_cond);
        pthread_mutex_unlock(&s_lock);
        pthread_mutex_lock(&s_present_lock);
        s_present_on = s_started;
        pthread_mutex_unlock(&s_present_lock);
        emudbg_resume();
        s_redraw = true;
    }
    pthread_mutex_unlock(&s_hold_lock);
}

bool emu_paused(void (*fn)(void *arg), void *arg)
{
    if (t_emulation_thread) {
        fn(arg);
        return true;
    }
    /* one at a time (recursive: a fn may call it again) */
    pthread_mutex_lock(&s_pause_lock);
    s_pause_depth++;
    hold_emulation();
    fn(arg);
    release_emulation();
    s_pause_depth--;
    pthread_mutex_unlock(&s_pause_lock);
    return true;
}

/* (inside emu_paused()) */
static void start_fn(void *arg)
{
    bool *ok = arg;
    *ok = M->start();
    s_started = *ok;
    if (*ok) {
        s_session_start = now_ns();
        emu_set_message(NULL);
    }
}

bool emu_restart(void)
{
    bool ok = false;
    if (!emu_paused(start_fn, &ok)) {
        return false;
    }
    if (ok) {
        launch_emulation();
    }
    return ok;
}

void emu_hold(void)
{
    hold_emulation();
}

void emu_release(void)
{
    release_emulation();
}

typedef struct {
    const int *before;
    bool ok;
} revert_t;

static void revert_fn(void *arg)
{
    revert_t *r = arg;
    start_fn(&r->ok);
    if (!r->ok) {
        EMU_LOGW(TAG, "can't start so: the choices as they were");
        for (int i = 0; i < M->choice_count && i < MAX_LIST; i++) {
            M->choices[i].set(r->before[i]);
        }
        start_fn(&r->ok);
    }
}

bool emu_restart_or_revert(const int *before)
{
    revert_t r = { before, false };
    emu_paused(revert_fn, &r);
    if (s_started) {
        launch_emulation();
    }
    return r.ok;
}

typedef struct {
    int row, index;             /* emu_choose() */
    int drive;                  /* emu_take_offer() */
    bool done;
} change_t;

/* Settings changed by fn, then a restart; the old choices back if it
 * can't start (as the boards' setup screen)
 */
static void change_and_restart(void (*change)(change_t *c), change_t *c)
{
    int before[MAX_LIST];
    for (int i = 0; i < M->choice_count && i < MAX_LIST; i++) {
        before[i] = M->choices[i].get();
    }
    change(c);
    if (!c->done || !s_started) {
        return;
    }
    bool ok = false;
    start_fn(&ok);
    if (!ok) {
        EMU_LOGW(TAG, "can't start so: the choices as they were");
        for (int i = 0; i < M->choice_count && i < MAX_LIST; i++) {
            M->choices[i].set(before[i]);
        }
        start_fn(&ok);
    }
}

static void choose(change_t *c)
{
    M->choices[c->row].set(c->index);
    c->done = true;
}

static void choose_fn(void *arg)
{
    change_and_restart(choose, arg);
}

bool emu_choose(int row, int index)
{
    if (row < 0 || row >= M->choice_count || index < 0) {
        return false;
    }
    const emu_choice_t *ch = &M->choices[row];
    const char *const *names = M->choice_names ? M->choice_names(row) : NULL;
    names = names ? names : ch->names;
    int n = 0;
    while (names[n] && names[n][0]) {
        n++;
    }
    if (index >= n) {
        return false;
    }
    if (ch->live) {
        ch->set(index);
        return true;
    }
    if (!s_started) {
        /* not running (it couldn't start): try with the new choice */
        ch->set(index);
        emu_restart();
        return true;
    }
    change_t c = { .row = row, .index = index };
    emu_paused(choose_fn, &c);
    return s_started;
}

static void take_offer(change_t *c)
{
    if (disks_offer(c->drive) != NULL) {
        disks_config()->take_offer(c->drive);
        c->done = true;
    }
}

static void offer_fn(void *arg)
{
    change_and_restart(take_offer, arg);
}

bool emu_take_offer(int drive)
{
    change_t c = { .drive = drive };
    return emu_paused(offer_fn, &c) && c.done;
}

/* ---- The picture ---- */

static SDL_Window *s_window;
static SDL_Renderer *s_renderer;
static SDL_Texture *s_texture;
static emu_screen_t s_mode;
static uint16_t *s_pixels;              /* the picture as last drawn */
static pthread_mutex_t s_pixels_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_signature;
static bool s_have_picture;
static atomic_uint s_pictures;          /* drawn into the window */

static void make_texture(const emu_screen_t *mode)
{
    if (s_texture) {
        SDL_DestroyTexture(s_texture);
    }
    s_texture = SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, mode->width,
                                  mode->height);
    if (s_texture == NULL) {
        EMU_LOGE(TAG, "no texture: %s", SDL_GetError());
        return;
    }
    bool sharp = M->scale == EMU_SCALE_NEAREST;
#if SDL_VERSION_ATLEAST(3, 4, 0)
    if (!sharp || !SDL_SetTextureScaleMode(s_texture, SDL_SCALEMODE_PIXELART))
#endif
        SDL_SetTextureScaleMode(s_texture, sharp ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
}

/* A new picture into the texture if the machine's changed; true if so */
static bool update_picture(void)
{
    bool changed = false;
    pthread_mutex_lock(&s_present_lock);
    if (s_present_on) {
        /* The signature first, as the boards' presenter: it can choose the
         * frame that screen_mode() and render_line() then describe (the
         * Archimedes pins its latest): a mode read before it was the last
         * frame's, and a frame of another width overran the pixels
         */
        uint32_t sig = M->screen_signature();
        emu_screen_t mode = { 0 };
        M->screen_mode(&mode);
        if (mode.width > 0 && mode.height > 0) {
            if (mode.width != s_mode.width || mode.height != s_mode.height || s_texture == NULL) {
                pthread_mutex_lock(&s_pixels_lock);
                free(s_pixels);
                s_pixels = calloc((size_t)mode.width * mode.height, sizeof(uint16_t));
                pthread_mutex_unlock(&s_pixels_lock);
                make_texture(&mode);
                s_have_picture = false;
            }
            s_mode = mode;
            if ((!s_have_picture || sig != s_signature) && s_pixels && s_texture) {
                pthread_mutex_lock(&s_pixels_lock);
                for (int y = 0; y < mode.height; y++) {
                    M->render_line(y, s_pixels + (size_t)y * mode.width);
                }
                pthread_mutex_unlock(&s_pixels_lock);
                SDL_UpdateTexture(s_texture, NULL, s_pixels, mode.width * 2);
                s_signature = sig;
                s_have_picture = true;
                changed = true;
            }
        }
    }
    pthread_mutex_unlock(&s_present_lock);
    return changed;
}

/* Where the picture goes in a window of ww x wh pixels: the machine's
 * aspect (view_width at 600 lines, or view_w x view_h), as large as fits
 */
static void picture_rect(int ww, int wh, SDL_FRect *src, SDL_FRect *dst)
{
    int sx = 0, sw = s_mode.width;
    if (s_mode.src_w > 0 && s_mode.src_x + s_mode.src_w <= s_mode.width) {
        sx = s_mode.src_x;
        sw = s_mode.src_w;
    }
    double aspect = (s_mode.view_w > 0 && s_mode.view_h > 0) ? (double)s_mode.view_w / s_mode.view_h
                                                              : M->view_width / 600.0 * sw / s_mode.width;
    double w = wh * aspect, h = wh;
    if (w > ww) {
        w = ww;
        h = ww / aspect;
    }
    *src = (SDL_FRect){ (float)sx, 0, (float)sw, (float)s_mode.height };
    *dst = (SDL_FRect){ floorf((ww - (float)w) / 2), floorf((wh - (float)h) / 2), floorf((float)w), floorf((float)h) };
}

/* Text in SDL's 8 x 8 font, wrapped, at twice its size */
static void draw_text(const char *text, int ww, int wh)
{
    const int scale = 2, cols = ww / (8 * scale) - 4;
    if (cols < 10) {
        return;
    }
    char lines[16][256];
    int n = 0;
    const char *p = text;
    while (*p && n < 16) {
        int len = (int)strlen(p);
        if (len > cols) {
            len = cols;
            while (len > 0 && p[len] != ' ') {
                len--;
            }
            if (len == 0) {
                len = cols;
            }
        }
        snprintf(lines[n++], sizeof(lines[0]), "%.*s", len, p);
        p += len;
        while (*p == ' ') {
            p++;
        }
    }
    SDL_SetRenderScale(s_renderer, scale, scale);
    SDL_SetRenderDrawColor(s_renderer, 255, 255, 255, 255);
    float y = (wh / scale - n * 12) / 2.0f;
    for (int i = 0; i < n; i++, y += 12) {
        float x = (ww / scale - (float)strlen(lines[i]) * 8) / 2.0f;
        SDL_RenderDebugText(s_renderer, x, y, lines[i]);
    }
    SDL_SetRenderScale(s_renderer, 1, 1);
}

/* The picture (or the message) into the renderer, not shown yet */
static void draw_contents(void)
{
    int ww, wh;
    SDL_GetRenderOutputSize(s_renderer, &ww, &wh);
    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);
    if (s_have_picture && s_texture && s_started) {
        SDL_FRect src, dst;
        picture_rect(ww, wh, &src, &dst);
        SDL_RenderTexture(s_renderer, s_texture, &src, &dst);
        s_pictures++;
    }
    char message[sizeof(s_message)];
    pthread_mutex_lock(&s_info_lock);
    snprintf(message, sizeof(message), "%s", s_message);
    pthread_mutex_unlock(&s_info_lock);
    if (message[0] && !s_started) {
        draw_text(message, ww, wh);
    }
}

static void draw(void)
{
    s_redraw = false;
    draw_contents();
    SDL_RenderPresent(s_renderer);
}

typedef struct {
    uint16_t *pixels;
    int width, height;
} shot_t;

/* (inside emu_paused()) */
static void shot_fn(void *arg)
{
    shot_t *shot = arg;
    emu_screen_t mode = { 0 };
    if (!s_started) {
        return;
    }
    /* the presenter's lock: its screen_signature() can choose another
     * frame (of another width) between the mode and the lines
     */
    pthread_mutex_lock(&s_present_lock);
    M->screen_mode(&mode);
    if (mode.width > 0 && mode.height > 0) {
        shot->pixels = malloc((size_t)mode.width * mode.height * sizeof(uint16_t));
    }
    if (shot->pixels) {
        shot->width = mode.width;
        shot->height = mode.height;
        for (int y = 0; y < mode.height; y++) {
            M->render_line(y, shot->pixels + (size_t)y * mode.width);
        }
    }
    pthread_mutex_unlock(&s_present_lock);
}

/* <data>/screenshots/<machine><kind>-<n>.png, the next free n */
static void shot_name(const char *kind, char *name, size_t len)
{
    char dir[1100];
    snprintf(dir, sizeof(dir), "%s/screenshots", s_data_dir);
    mkdir(dir, 0755);
    char machine[64];
    snprintf(machine, sizeof(machine), "%s", M->short_name ? M->short_name : "machine");
    for (char *p = machine; *p; p++) {
        *p = (*p >= 'A' && *p <= 'Z') ? *p + 32 : (*p == ' ' || *p == '/') ? '-' : *p;
    }
    struct stat st;
    for (int n = 1; n < 100000; n++) {
        snprintf(name, len, "%s/%s%s-%04d.png", dir, machine, kind, n);
        if (stat(name, &st) != 0) {
            break;
        }
    }
}

const char *emu_short_path(const char *path, char *buf, size_t len)
{
    const char *home = getenv("HOME");
    size_t n = home ? strlen(home) : 0;
    if (n > 1 && strncmp(path, home, n) == 0 && (path[n] == '/' || path[n] == 0)) {
        snprintf(buf, len, "~%s", path + n);
    } else {
        snprintf(buf, len, "%s", path);
    }
    return buf;
}

/* ---- The window as a picture (the console's "window") ---- */

typedef struct {
    const char *path;
    bool ok, done;
} window_shot_t;

static pthread_mutex_t s_wshot_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_wshot_cond = PTHREAD_COND_INITIALIZER;

/* (the main thread) */
static void window_shot(window_shot_t *w)
{
    int ww, wh;
    SDL_GetRenderOutputSize(s_renderer, &ww, &wh);
    if (ui_is_open()) {
        ui_draw(ww, wh);
    } else {
        draw_contents();
    }
    SDL_Surface *s = SDL_RenderReadPixels(s_renderer, NULL);
    SDL_Surface *rgb = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGB24) : NULL;
    bool ok = rgb && emu_write_png_rgb(w->path, rgb->pixels, rgb->w, rgb->h, rgb->pitch);
    SDL_DestroySurface(rgb);
    SDL_DestroySurface(s);
    SDL_RenderPresent(s_renderer);
    pthread_mutex_lock(&s_wshot_lock);
    w->ok = ok;
    w->done = true;
    pthread_cond_broadcast(&s_wshot_cond);
    pthread_mutex_unlock(&s_wshot_lock);
}

static Uint32 s_ui_event;

bool emu_window_shot(const char *path, char *made, size_t len)
{
    char name[1200];
    if (path == NULL || path[0] == 0) {
        shot_name("-window", name, sizeof(name));
        path = name;
    }
    window_shot_t w = { path, false, false };
    SDL_Event e = { .type = s_ui_event };
    e.user.code = 3;
    e.user.data1 = &w;
    if (!SDL_PushEvent(&e)) {
        return false;
    }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 5;
    pthread_mutex_lock(&s_wshot_lock);
    while (!w.done) {
        if (pthread_cond_timedwait(&s_wshot_cond, &s_wshot_lock, &until) != 0) {
            break;
        }
    }
    bool done = w.done, ok = w.ok;
    pthread_mutex_unlock(&s_wshot_lock);
    if (!done) {
        /* the main thread still has it: it mustn't write into this frame */
        EMU_LOGE(TAG, "the window didn't answer: quitting would be safest");
        return false;
    }
    if (made) {
        snprintf(made, len, "%s", ok ? path : "");
    }
    return ok;
}

bool emu_screenshot(const char *path, char *made, size_t len)
{
    char name[1200];
    if (path == NULL || path[0] == 0) {
        shot_name("", name, sizeof(name));
        path = name;
    }
    /* From the machine itself, held between frames: the picture as it is now
     * (the window's may be behind: over X forwarding each one is a trip)
     */
    shot_t shot = { 0 };
    emu_paused(shot_fn, &shot);
    bool ok = shot.pixels && emu_write_png(path, shot.pixels, shot.width, shot.height);
    free(shot.pixels);
    if (made) {
        snprintf(made, len, "%s", ok ? path : "");
    }
    return ok;
}

/* ---- Input ---- */

static bool s_held[256];                /* HID usages down in the machine */
static bool s_swallow[256];             /* a host key's release */
static bool s_grabbed;
static uint8_t s_buttons;
static float s_mouse_x, s_mouse_y;
/* Without SDL's relative mode (XQuartz: pi.sh turns XInput2 off, SDL's
 * read axes the server never sends): the pointer hidden, held in the
 * window and put back in its middle, the motion from where it went.
 * Hidden by a cursor of one pixel at 1/255 opacity, not SDL_HideCursor():
 * MobaXterm's X server shows its arrow for a blank cursor (SDL's, XFixes's
 * too), beside the machine's own pointer; this one it takes, and draws
 * nothing
 */
static bool s_warping;
static bool s_warp_pending;             /* until motion from the middle */
static uint64_t s_warp_ns;
static float s_last_x, s_last_y;        /* the motion is measured from here */
static float s_from_x, s_from_y;        /* where the pointer was warped from */
static SDL_Cursor *s_faint_cursor;

/* Set again each time (from the default): a change, which XQuartz takes
 * when it has become the active application
 */
static void hide_pointer(void)
{
    if (s_faint_cursor == NULL) {
        SDL_Surface *s = SDL_CreateSurface(16, 16, SDL_PIXELFORMAT_ARGB8888);
        if (s != NULL) {
            SDL_FillSurfaceRect(s, NULL, 0);
            ((Uint32 *)s->pixels)[0] = 0x01000000;
            s_faint_cursor = SDL_CreateColorCursor(s, 0, 0);
            SDL_DestroySurface(s);
        }
    }
    if (s_faint_cursor != NULL) {
        SDL_SetCursor(SDL_GetDefaultCursor());
        SDL_SetCursor(s_faint_cursor);
    } else {
        SDL_ShowCursor();
        SDL_HideCursor();
    }
}

static void show_pointer(void)
{
    SDL_SetCursor(SDL_GetDefaultCursor());
    SDL_ShowCursor();
}

static void centre_pointer(float from_x, float from_y)
{
    int w, h;
    SDL_GetWindowSize(s_window, &w, &h);
    s_from_x = from_x;
    s_from_y = from_y;
    s_last_x = (float)(w / 2);
    s_last_y = (float)(h / 2);
    SDL_WarpMouseInWindow(s_window, s_last_x, s_last_y);
    s_warp_pending = true;
    s_warp_ns = now_ns();
}

static void grab(bool on)
{
    if (on == s_grabbed || (on && M->hid_mouse == NULL)) {
        return;
    }
    s_grabbed = on;
    if (on && !SDL_SetWindowRelativeMouseMode(s_window, true)) {
        EMU_LOGI(TAG, "no relative mouse mode (%s): by warps", SDL_GetError());
        s_warping = true;
        hide_pointer();
        SDL_SetWindowMouseGrab(s_window, true);
        float x, y;
        SDL_GetMouseState(&x, &y);
        centre_pointer(x, y);
    } else if (!on && s_warping) {
        s_warping = false;
        SDL_SetWindowMouseGrab(s_window, false);
        show_pointer();
    } else if (!on) {
        SDL_SetWindowRelativeMouseMode(s_window, false);
    }
    if (!on && s_buttons) {
        s_buttons = 0;
        M->hid_mouse(0, 0, 0);
    }
    s_redraw = true;
    EMU_LOGI(TAG, on ? "the mouse is the machine's (Ctrl+Alt+G lets go)" : "the mouse is free");
}

static void release_all(void)
{
    for (int u = 0; u < 256; u++) {
        if (s_held[u]) {
            s_held[u] = false;
            if (M->hid_key) {
                M->hid_key((uint8_t)u, false);
            }
        }
    }
    grab(false);
}

static void insert_fn(void *arg)
{
    const char *path = arg;
    int drive = disks_drive_for(path);
    if (drive < 0) {
        EMU_LOGW(TAG, "no drive takes %s", path);
    } else if (!disks_insert(drive, path)) {
        EMU_LOGW(TAG, "%s didn't go into %s", path, disks_config()->drive_names[drive]);
    }
}

static void insert_file(const char *path)
{
    char full[DISKS_NAME_LEN];
    if (path[0] != '/') {
        char cwd[DISKS_NAME_LEN / 2];
        snprintf(full, sizeof(full), "%s/%s", getcwd(cwd, sizeof(cwd)) ? cwd : ".", path);
    } else {
        snprintf(full, sizeof(full), "%s", path);
    }
    if (M->disks == NULL || !disks_ready()) {
        EMU_LOGW(TAG, "the %s has no drives", M->short_name);
        return;
    }
    emu_paused(insert_fn, full);
}

void emu_toggle_fullscreen(void)
{
    bool on = (SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN) == 0;
    SDL_SetWindowFullscreen(s_window, on);
}

/* The setup screen open, or back to the machine */
static void ui_toggle(void)
{
    if (!ui_is_open()) {
        release_all();
        ui_open();
    } else {
        ui_close_to_machine();
    }
}


void emu_ui_click(int x, int y)
{
    SDL_Event e = { .type = s_ui_event };
    e.user.code = 1;
    e.user.data1 = (void *)(intptr_t)x;
    e.user.data2 = (void *)(intptr_t)y;
    SDL_PushEvent(&e);
}

void emu_ui_toggle(void)
{
    SDL_Event e = { .type = s_ui_event };
    e.user.code = 2;
    SDL_PushEvent(&e);
}

/* Ctrl+Alt+<key>: the window's own keys, not the machine's */
static bool host_key(SDL_Scancode sc)
{
    char made[1200];
    switch (sc) {
    case SDL_SCANCODE_F:
    case SDL_SCANCODE_RETURN:
        emu_toggle_fullscreen();
        return true;
    case SDL_SCANCODE_M:
        ui_toggle();
        return true;
    case SDL_SCANCODE_G:
        grab(false);
        return true;
    case SDL_SCANCODE_W:
        s_warp = !s_warp;
        EMU_LOGI(TAG, "%s", s_warp ? "warp: as fast as it goes, no sound" : "real speed");
        return true;
    case SDL_SCANCODE_S:
        if (emu_screenshot(NULL, made, sizeof(made))) {
            EMU_LOGI(TAG, "screenshot %s", made);
        }
        return true;
    case SDL_SCANCODE_R:
        emu_request_reset();
        return true;
    case SDL_SCANCODE_Q:
        emu_quit();
        return true;
    default:
        return false;
    }
}

/* Ctrl+Alt+<key> is the window's, else the setup screen's while it's open,
 * else the machine's
 */
static void key_event(const SDL_Event *e)
{
    const SDL_KeyboardEvent *k = &e->key;
    SDL_Scancode sc = k->scancode;
    if (sc <= 0 || sc >= 256) {
        return;
    }
    if (!k->down && s_swallow[sc]) {
        s_swallow[sc] = false;
        return;
    }
    if (k->down && !k->repeat && (k->mod & SDL_KMOD_CTRL) && (k->mod & SDL_KMOD_ALT) && host_key(sc)) {
        s_swallow[sc] = true;
        return;
    }
    if (ui_is_open()) {
        ui_event(e);
        return;
    }
    if (k->repeat || M->hid_key == NULL || s_held[sc] == k->down) {
        return;
    }
    s_held[sc] = k->down;
    M->hid_key((uint8_t)sc, k->down);
}

static void mouse_button(const SDL_MouseButtonEvent *b)
{
    if (M->hid_mouse == NULL) {
        return;
    }
    if (!s_grabbed) {
        if (b->down && b->button == SDL_BUTTON_LEFT && s_started) {
            grab(true);
        }
        return;
    }
    uint8_t bit = b->button == SDL_BUTTON_LEFT ? 1 : b->button == SDL_BUTTON_RIGHT ? 2
                : b->button == SDL_BUTTON_MIDDLE ? 4 : 0;
    s_buttons = b->down ? s_buttons | bit : s_buttons & ~bit;
    M->hid_mouse(0, 0, s_buttons);
}

static void mouse_motion(const SDL_MouseMotionEvent *m)
{
    if (!s_grabbed || M->hid_mouse == NULL) {
        return;
    }
    float xrel = m->xrel, yrel = m->yrel;
    if (s_warping) {
        if (s_warp_pending) {
            if (fabsf(m->x - s_last_x) <= 1 && fabsf(m->y - s_last_y) <= 1) {
                return;                 /* the warp's own: in the X server, on the screen? */
            }
            if (hypotf(m->x - s_from_x, m->y - s_from_y) < hypotf(m->x - s_last_x, m->y - s_last_y)) {
                /* Motion from where the pointer was: from before the warp
                 * (over the network it comes even after the warp's own),
                 * or the pointer on the screen stayed there (XQuartz moves
                 * and hides it only as the active application, and the
                 * click that made it that came first): then again
                 */
                if (now_ns() - s_warp_ns > 200000000ull) {
                    hide_pointer();
                    centre_pointer(m->x, m->y);
                }
                return;
            }
            s_warp_pending = false;
        }
        xrel = m->x - s_last_x;
        yrel = m->y - s_last_y;
        s_last_x = m->x;
        s_last_y = m->y;
        int w, h;
        SDL_GetWindowSize(s_window, &w, &h);
        if (m->x < w / 4 || m->x > w * 3 / 4 || m->y < h / 4 || m->y > h * 3 / 4) {
            centre_pointer(m->x, m->y);
        }
    }
    s_mouse_x += xrel;
    s_mouse_y += yrel;
    int dx = (int)s_mouse_x, dy = (int)s_mouse_y;
    s_mouse_x -= dx;
    s_mouse_y -= dy;
    if (dx || dy) {
        M->hid_mouse(dx, dy, s_buttons);
    }
}

/* ---- Once a second ---- */

static float read_temperature(void)
{
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    long milli = 0;
    bool ok = f && fscanf(f, "%ld", &milli) == 1;
    if (f) {
        fclose(f);
    }
    return ok ? milli / 1000.0f : NAN;
}

static pthread_t s_main_thread;

static void *stats_thread(void *arg)
{
    unsigned last_frames = s_frames, last_pictures = s_pictures;
    uint64_t last = now_ns();
    double last_main = thread_cpu_s(s_main_thread), last_emu = 0;
    int tick = 0;
    while (!s_quit) {
        for (int i = 0; i < 10 && !s_quit; i++) {
            usleep(100000);
        }
        if (s_quit) {
            break;
        }
        uint64_t now = now_ns();
        double dt = (now - last) / 1e9;
        unsigned frames = s_frames, pictures = s_pictures;
        double cpu_main = thread_cpu_s(s_main_thread);
        double cpu_emu = s_thread_running ? thread_cpu_s(s_emu_thread) : 0;
        emu_info_t info;
        pthread_mutex_lock(&s_info_lock);
        info = s_info;
        info.fps = (float)((frames - last_frames) / dt);
        info.pictures = (float)((pictures - last_pictures) / dt);
        int hz = 50;
        if (s_started) {
            emu_screen_t mode = { 0 };
            M->screen_mode(&mode);
            hz = mode.hz > 0 ? mode.hz : 50;
        }
        info.speed = (int)lround(info.fps * 100.0 / hz);
        info.load[0] = (float)((cpu_main - last_main) / dt * 100.0);
        info.load[1] = (float)((cpu_emu - last_emu) / dt * 100.0);
        if (tick++ % 5 == 0) {
            info.temperature = read_temperature();
        }
        s_info = info;
        pthread_mutex_unlock(&s_info_lock);
        s_stats_seq++;
        last = now;
        last_frames = frames;
        last_pictures = pictures;
        last_main = cpu_main;
        last_emu = cpu_emu;

        disks_poll();
        if (s_started && M->every_second) {
            M->every_second();
        }
    }
    return NULL;
}

static void update_title(void)
{
    emu_info_t info;
    emu_info(&info);
    char title[256];
    if (ui_is_open()) {
        snprintf(title, sizeof(title), "%s - setup", M->name);
    } else if (!s_started) {
        snprintf(title, sizeof(title), "%s", M->name);
    } else if (info.state == EMU_STOPPED) {
        snprintf(title, sizeof(title), "%s - stopped in the debugger", M->name);
    } else {
        snprintf(title, sizeof(title), "%s - %d%%%s%s", M->name, info.speed, s_warp ? " (warp)" : "",
                 s_grabbed ? " - Ctrl+Alt+G frees the mouse" : "");
    }
    SDL_SetWindowTitle(s_window, title);
}

/* ---- Status ---- */

typedef struct {
    char model[96], hostname[96], address[160];
    long uptime;
    uint64_t mem_total, mem_free, data_total, data_free;
} host_t;

static void host_info(host_t *h)
{
    memset(h, 0, sizeof(*h));
    FILE *f = fopen("/proc/device-tree/model", "r");
    if (f == NULL || fgets(h->model, sizeof(h->model), f) == NULL) {
        snprintf(h->model, sizeof(h->model), "Linux");
    }
    if (f) {
        fclose(f);
    }
    h->model[strcspn(h->model, "\n")] = 0;
    for (size_t n = strlen(h->model); n > 0 && h->model[n - 1] == ' '; n--) {
        h->model[n - 1] = 0;
    }
    gethostname(h->hostname, sizeof(h->hostname) - 1);
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) == 0) {
        for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
            if (i->ifa_addr == NULL || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) ||
                strncmp(i->ifa_name, "docker", 6) == 0 || strncmp(i->ifa_name, "br-", 3) == 0 ||
                strncmp(i->ifa_name, "veth", 4) == 0 || strncmp(i->ifa_name, "virbr", 5) == 0) {
                continue;               /* containers' and VMs' bridges: not where the page is */
            }
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof(ip));
            size_t n = strlen(h->address);
            snprintf(h->address + n, sizeof(h->address) - n, "%s%s", n ? ", " : "", ip);
        }
        freeifaddrs(ifs);
    }
    double up = 0;
    f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &up) != 1) {
            up = 0;
        }
        fclose(f);
    }
    h->uptime = (long)up;
    f = fopen("/proc/meminfo", "r");
    char line[128];
    while (f && fgets(line, sizeof(line), f)) {
        unsigned long long kb;
        if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) {
            h->mem_total = kb * 1024;
        } else if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
            h->mem_free = kb * 1024;
        }
    }
    if (f) {
        fclose(f);
    }
    struct statvfs vfs;
    if (statvfs(s_data_dir, &vfs) == 0) {
        h->data_total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
        h->data_free = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    }
}

static void duration_text(long s, char *buf, size_t len)
{
    if (s >= 86400) {
        snprintf(buf, len, "%ld d %ld h", s / 86400, s % 86400 / 3600);
    } else if (s >= 3600) {
        snprintf(buf, len, "%ld h %ld min", s / 3600, s % 3600 / 60);
    } else {
        snprintf(buf, len, "%ld min %ld s", s / 60, s % 60);
    }
}

static void html_escape(const char *s, char *out, size_t len)
{
    size_t n = 0;
    for (; *s && n + 6 < len; s++) {
        const char *e = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '"' ? "&quot;" : NULL;
        if (e) {
            n += snprintf(out + n, len - n, "%s", e);
        } else {
            out[n++] = *s;
        }
    }
    out[n] = 0;
}

void emu_status_json(char *buf, size_t len)
{
    emu_json_t j;
    emu_json_begin(&j, buf, len);
    emu_info_t info;
    emu_info(&info);
    static const char *const states[] = { "setup", "running", "paused", "halted", "stopped" };

    /* The machine */
    emu_json_object(&j, "machine");
    emu_json_str(&j, "name", M->name);
    emu_json_str(&j, "state", ui_is_open() && info.state == EMU_RUNNING ? "paused" : states[info.state]);
    if (s_started) {
        emu_screen_t mode = { 0 };
        M->screen_mode(&mode);
        emu_json_int(&j, "hz", mode.hz);
        emu_json_num(&j, "fps", info.fps);
        emu_json_int(&j, "speed", info.speed);
        emu_json_int(&j, "session", info.session_s);
        emu_json_array(&j, "rows");
        if (M->status) {
            M->status(&j, EMU_STATUS_ROWS);
        }
        emu_json_end_array(&j);
    }
    emu_json_str(&j, "message", s_message[0] ? s_message : NULL);
    time_t now = time(NULL);
    struct tm tm;
    char clock[32];
    localtime_r(&now, &tm);
    strftime(clock, sizeof(clock), "%Y-%m-%d %H:%M:%S", &tm);
    emu_json_str(&j, "clock", clock);
    struct stat st;
    bool synced = stat("/run/systemd/timesync/synchronized", &st) == 0;
    emu_json_bool(&j, "clock_set", true);
    emu_json_str(&j, "clock_from", synced ? "internet" : "board");
    emu_json_end_object(&j);

    /* Drives, and what the machine offers for their disks */
    emu_json_array(&j, "drives");
    if (M->status && s_started) {
        M->status(&j, EMU_STATUS_DRIVES);
    }
    emu_json_end_array(&j);
    emu_json_array(&j, "offers");
    for (int d = 0; M->disks && disks_ready() && d < disks_config()->drives; d++) {
        const char *text = disks_offer(d);
        if (text) {
            emu_json_object(&j, NULL);
            emu_json_int(&j, "drive", d + 1);
            emu_json_str(&j, "text", text);
            emu_json_end_object(&j);
        }
    }
    emu_json_end_array(&j);

    /* The computer: the boards' fields the page reads, and rows of its own */
    host_t h;
    host_info(&h);
    emu_json_object(&j, "board");
    emu_json_str(&j, "name", h.model);
    emu_json_str(&j, "title", "Raspberry Pi");
    emu_json_int(&j, "uptime", h.uptime);
    emu_json_object(&j, "wifi");
    emu_json_str(&j, "ssid", NULL);
    emu_json_str(&j, "ip", h.address[0] ? h.address : NULL);
    emu_json_int(&j, "rssi", 0);
    emu_json_end_object(&j);
    emu_json_array(&j, "load");
    emu_json_int(&j, NULL, lroundf(info.load[0]));
    emu_json_int(&j, NULL, lroundf(info.load[1]));
    emu_json_end_array(&j);
    emu_json_num(&j, "temperature", info.temperature);
    emu_json_int(&j, "sd_total", (long long)h.data_total);
    emu_json_int(&j, "sd_free", (long long)h.data_free);
    emu_json_array(&j, "rows");
    char row[1400];
    snprintf(row, sizeof(row), "%s <span class=\"dim\">(%s)</span>", h.model, h.hostname);
    emu_json_row(&j, "Computer", row);
    if (h.address[0]) {
        emu_json_row(&j, "Address", h.address);
    }
    duration_text(h.uptime, row, sizeof(row));
    emu_json_row(&j, "Up for", row);
    snprintf(row, sizeof(row), "%ld%% of a core <span class=\"dim\">(the window)</span>", lroundf(info.load[0]));
    emu_json_row_bar(&j, "Window", row, (int)lroundf(info.load[0]));
    snprintf(row, sizeof(row), "%ld%% of a core <span class=\"dim\">(%s emulation)</span>", lroundf(info.load[1]),
             M->short_name);
    emu_json_row_bar(&j, "Emulation", row, (int)lroundf(info.load[1]));
    if (!isnan(info.temperature)) {
        snprintf(row, sizeof(row), "%.0f °C", info.temperature);
        emu_json_row(&j, "CPU", row);
    }
    if (h.mem_total) {
        snprintf(row, sizeof(row), "%.1f GB free of %.1f GB", h.mem_free / 1073741824.0, h.mem_total / 1073741824.0);
        emu_json_row_bar(&j, "Memory", row, (int)(100 - h.mem_free * 100 / h.mem_total));
    }
    if (h.data_total) {
        char path[1100];
        html_escape(s_data_dir, path, sizeof(path));
        snprintf(row, sizeof(row), "%s: %.1f GB free of %.1f GB", path, h.data_free / 1073741824.0,
                 h.data_total / 1073741824.0);
        emu_json_row_bar(&j, "Data folder", row, (int)(100 - h.data_free * 100 / h.data_total));
    }
    emu_json_end_array(&j);
    emu_json_end_object(&j);
    emu_json_end_object(&j);
}

/* ---- Options ---- */

/* A button's name on one line: "80286\n6 MHz" as "80286 6 MHz" */
static void one_line(const char *name, char *out, size_t len)
{
    snprintf(out, len, "%s", name);
    for (char *p = out; *p; p++) {
        *p = *p == '\n' ? ' ' : *p;
    }
}

static void choice_list(FILE *f)
{
    for (int i = 0; i < M->choice_count; i++) {
        const emu_choice_t *ch = &M->choices[i];
        const char *const *names = M->choice_names ? M->choice_names(i) : NULL;
        names = names ? names : ch->names;
        fprintf(f, "  %-12s", ch->label);
        for (int n = 0; names[n] && names[n][0]; n++) {
            char name[128];
            one_line(names[n], name, sizeof(name));
            fprintf(f, "%s%s", n ? ", " : "", name);
        }
        fprintf(f, "\n");
    }
}

static void usage(const char *prog, FILE *f)
{
    fprintf(f,
            "%s: %s on the Raspberry Pi (rpi5emu)\n\n"
            "Usage: %s [options] [images ...]\n\n"
            "  images               disks, tapes, snapshots ...: each into the first drive\n"
            "                       that takes it\n"
            "  -S, --start          start the machine at once (else the setup screen first)\n"
            "  -d, --data DIR       the data folder: roms/, the disk folders\n"
            "                       (default $RPI5EMU_HOME, else ~/rpi5emu)\n"
            "  -f, --fullscreen     start full screen (Ctrl+Alt+F switches)\n"
            "  -s, --scale N        the window's size: N times the machine's own (default 1)\n"
            "  -c, --choice L=NAME  a setup choice, e.g. --choice %s=%s (kept, as the\n"
            "                       boards' setup screen)\n"
            "      --no-sound\n"
            "  -p, --console PORT   line commands on 127.0.0.1:PORT (default 6400, 0: none);\n"
            "                       also from the terminal (`h` lists them)\n"
            "      --no-stdin       not from the terminal\n"
            "  -w, --web PORT       the web page (disk images, status) on PORT of every\n"
            "                       network address (default 8080, 0: none)\n"
            "      --debug PORT     the debugger (esp32emu's docs/debugger.md) on\n"
            "                       127.0.0.1:PORT\n"
            "  -v, --verbose\n"
            "  -h, --help\n\n"
            "Keys: Ctrl+Alt+M the setup screen, Ctrl+Alt+F full screen, Ctrl+Alt+G frees\n"
            "the mouse (a click in the window gives it to the machine), Ctrl+Alt+W warp\n"
            "speed, Ctrl+Alt+S screenshot, Ctrl+Alt+R reset, Ctrl+Alt+Q quit. Images can\n"
            "be dropped on the window.\n",
            prog, M->name, prog, M->choice_count ? M->choices[0].label : "Model",
            M->choice_count ? M->choices[0].names[0] : "1");
    if (M->choice_count) {
        fprintf(f, "\nChoices:\n");
        choice_list(f);
    }
    fprintf(f, "\nSettings: %s\n", emu_settings_path());
}

static void parse_options(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "data", required_argument, NULL, 'd' },
        { "fullscreen", no_argument, NULL, 'f' },
        { "scale", required_argument, NULL, 's' },
        { "choice", required_argument, NULL, 'c' },
        { "no-sound", no_argument, NULL, 'n' },
        { "console", required_argument, NULL, 'p' },
        { "no-stdin", no_argument, NULL, 'i' },
        { "debug", required_argument, NULL, 'g' },
        { "start", no_argument, NULL, 'S' },
        { "web", required_argument, NULL, 'w' },
        { "verbose", no_argument, NULL, 'v' },
        { "help", no_argument, NULL, 'h' },
        { 0 },
    };
    const char *prog = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    int c;
    while ((c = getopt_long(argc, argv, "d:fs:c:p:w:Svh", longopts, NULL)) != -1) {
        switch (c) {
        case 'S': opt.start = true; break;
        case 'w': opt.web_port = atoi(optarg); break;
        case 'd': opt.data_dir = optarg; break;
        case 'f': opt.fullscreen = true; break;
        case 's': opt.scale = atof(optarg); break;
        case 'c':
            if (opt.choice_count < MAX_LIST) {
                opt.choices[opt.choice_count++] = optarg;
            }
            break;
        case 'n': opt.no_sound = true; break;
        case 'p': opt.console_port = atoi(optarg); break;
        case 'i': opt.no_stdin = true; break;
        case 'g': opt.debug_port = atoi(optarg); break;
        case 'v': emu_log_verbose = true; break;
        case 'h': usage(prog, stdout); exit(0);
        default: usage(prog, stderr); exit(2);
        }
    }
    for (int i = optind; i < argc && opt.image_count < MAX_LIST; i++) {
        opt.images[opt.image_count++] = argv[i];
    }
    if (opt.scale < 0.25 || opt.scale > 8) {
        opt.scale = 1;
    }
}

static void make_dirs(const char *path)
{
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/", path);
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(dir, 0755);
            *p = '/';
        }
    }
}

static void set_data_dir(void)
{
    const char *d = opt.data_dir ? opt.data_dir : getenv("RPI5EMU_HOME");
    if (d != NULL && d[0]) {
        snprintf(s_data_dir, sizeof(s_data_dir), "%s", d);
    } else {
        const char *home = getenv("HOME");
        snprintf(s_data_dir, sizeof(s_data_dir), "%s/rpi5emu", home ? home : ".");
    }
    size_t n = strlen(s_data_dir);
    while (n > 1 && s_data_dir[n - 1] == '/') {
        s_data_dir[--n] = 0;
    }
    make_dirs(s_data_dir);
    struct stat st;
    s_info.sd_card = stat(s_data_dir, &st) == 0 && S_ISDIR(st.st_mode);
}

/* "--choice Model=+3": by the button's name (its first line, any case) or number */
static void apply_choice(const char *text)
{
    const char *eq = strchr(text, '=');
    if (eq == NULL) {
        EMU_LOGW(TAG, "--choice %s: LABEL=NAME", text);
        return;
    }
    for (int i = 0; i < M->choice_count; i++) {
        const emu_choice_t *ch = &M->choices[i];
        if (strlen(ch->label) != (size_t)(eq - text) || strncasecmp(ch->label, text, eq - text) != 0) {
            continue;
        }
        const char *const *names = M->choice_names ? M->choice_names(i) : NULL;
        names = names ? names : ch->names;
        for (int n = 0; names[n] && names[n][0]; n++) {
            /* its first line, or both ("80286 8 MHz") */
            size_t len = strcspn(names[n], "\n");
            char whole[128];
            one_line(names[n], whole, sizeof(whole));
            if ((strlen(eq + 1) == len && strncasecmp(names[n], eq + 1, len) == 0) ||
                strcasecmp(whole, eq + 1) == 0) {
                ch->set(n);
                return;
            }
        }
        char *end;
        long n = strtol(eq + 1, &end, 10), count = 0;
        while (names[count] && names[count][0]) {
            count++;
        }
        if (*end == 0 && end != eq + 1 && n >= 0 && n < count) {
            ch->set((int)n);
            return;
        }
        EMU_LOGW(TAG, "--choice %s: no such button", text);
        return;
    }
    EMU_LOGW(TAG, "--choice %s: no such choice", text);
}

/* ---- The window ---- */

static bool open_window(void)
{
    double aspect = M->view_width / 600.0;
    int h = (int)lround(600 * opt.scale), w = (int)lround(h * aspect);
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    SDL_Rect usable;
    if (display && SDL_GetDisplayUsableBounds(display, &usable) && (w > usable.w || h > usable.h) &&
        usable.w > 0 && usable.h > 0) {
        double f = fmin((double)usable.w / w, (double)usable.h / h) * 0.95;
        w = (int)(w * f);
        h = (int)(h * f);
    }
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (opt.fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    if (!SDL_CreateWindowAndRenderer(M->name, w, h, flags, &s_window, &s_renderer)) {
        EMU_LOGE(TAG, "no window: %s", SDL_GetError());
        return false;
    }
    /* vsync, unless the display's rate isn't one: XQuartz numbers its
     * modes 1, 2 ... Hz, and the software renderer's vsync (a wait of one
     * refresh) then held each picture, and the input, for a second
     */
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(s_window));
    bool no_rate = dm && dm->refresh_rate > 0 && dm->refresh_rate < 20;
    SDL_SetRenderVSync(s_renderer, no_rate ? 0 : 1);
    SDL_SetWindowMinimumSize(s_window, 160, 120);
    EMU_LOGI(TAG, "window %d x %d through %s, drawn by %s%s", w, h, SDL_GetCurrentVideoDriver(),
             SDL_GetRendererName(s_renderer), no_rate ? ", no vsync (the display's rate is no rate)" : "");
    return true;
}

static void handle_event(SDL_Event *e)
{
    bool ui = ui_is_open();
    if (e->type == s_ui_event) {
        if (e->user.code == 1) {
            ui_click((float)(intptr_t)e->user.data1, (float)(intptr_t)e->user.data2);
        } else if (e->user.code == 3) {
            window_shot(e->user.data1);
        } else {
            ui_toggle();
        }
        return;
    }
    switch (e->type) {
    case SDL_EVENT_QUIT:
        s_quit = true;
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        key_event(e);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
        if (ui) {
            SDL_ConvertEventToRenderCoordinates(s_renderer, e);
            ui_event(e);
        } else if (e->type == SDL_EVENT_MOUSE_MOTION) {
            mouse_motion(&e->motion);
        } else if (e->type != SDL_EVENT_MOUSE_WHEEL) {
            mouse_button(&e->button);
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        release_all();
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        s_redraw = true;
        ui_touch();
        break;
    case SDL_EVENT_DROP_FILE:
        if (e->drop.data) {
            insert_file(e->drop.data);
            ui_touch();
        }
        break;
    default:
        break;
    }
}

static void main_loop(void)
{
    unsigned presented = s_frames - 1, title_seq = s_stats_seq - 1;
    bool ui_was_open = false;
    uint64_t ui_drawn = 0;
    while (!s_quit) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 20)) {
            do {
                if (e.type == s_frame_event) {
                    s_frame_event_pending = false;
                    continue;
                }
                handle_event(&e);
            } while (SDL_PollEvent(&e));
        }
        if (ui_is_open()) {
            /* the setup screen: on any change, and twice a second (the
             * drives, a message)
             */
            uint64_t now = now_ns();
            if (!ui_was_open || ui_dirty() || now - ui_drawn > 500000000ull) {
                int ww, wh;
                SDL_GetRenderOutputSize(s_renderer, &ww, &wh);
                ui_draw(ww, wh);
                SDL_RenderPresent(s_renderer);
                ui_drawn = now;
            }
            if (!ui_was_open) {
                ui_was_open = true;
                update_title();
            }
            continue;
        }
        if (ui_was_open) {
            ui_was_open = false;
            s_redraw = true;
            update_title();
        }
        unsigned frames = s_frames;
        bool changed = false;
        if (frames != presented) {
            presented = frames;
            changed = update_picture();
        }
        if (changed || s_redraw) {
            draw();
        }
        if (s_stats_seq != title_seq) {
            title_seq = s_stats_seq;
            update_title();
        }
    }
}

static void on_signal(int sig)
{
    s_quit = true;
}

int emu_main(const emu_machine_t *machine, int argc, char **argv)
{
    M = machine;
    s_main_thread = pthread_self();
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_pause_lock, &attr);
    parse_options(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);           /* a console read in the background fails instead of stopping us */
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    set_data_dir();
    EMU_LOGI(TAG, "%s: data in %s, settings in %s", M->name, s_data_dir, emu_settings_path());
    if (M->reserve_memory) {
        M->reserve_memory();
    }
    if (M->storage) {
        M->storage(s_info.sd_card);
    }
    for (int i = 0; i < opt.choice_count; i++) {
        apply_choice(opt.choices[i]);
    }
    if (M->disks && s_info.sd_card) {
        disks_init(M->disks);
        disks_restore();        /* before the machine starts: a disk in the first drive boots */
    }
    for (int i = 0; i < opt.image_count; i++) {
        insert_file(opt.images[i]);
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        EMU_LOGE(TAG, "SDL: %s", SDL_GetError());
        return 1;
    }
    s_frame_event = SDL_RegisterEvents(2);
    s_ui_event = s_frame_event + 1;
    if (!open_window()) {
        SDL_Quit();
        return 1;
    }
    audio_open();
    ui_init(s_renderer);
    if (opt.debug_port > 0) {
        emudbg_serve(opt.debug_port);
    }

    /* The setup screen first, as on the boards (--start: straight in); again
     * when the machine can't start
     */
    if (opt.start) {
        bool ok = false;
        start_fn(&ok);
        pthread_mutex_lock(&s_present_lock);
        s_present_on = ok;
        pthread_mutex_unlock(&s_present_lock);
        if (ok) {
            launch_emulation();
        } else if (!s_message[0]) {
            emu_set_message("The machine could not be started: see the log.");
        }
    }
    if (!s_started) {
        ui_open();
    }
    s_redraw = true;

    pthread_t stats;
    pthread_create(&stats, NULL, stats_thread, NULL);
    emu_console_start(opt.console_port, !opt.no_stdin);
    emu_web_start(opt.web_port);

    main_loop();

    /* Out: the emulation stopped between frames (a debugger stop let go) */
    s_quit = true;
    emudbg_suspend();
    pthread_mutex_lock(&s_lock);
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
    if (s_thread_running) {
        pthread_join(s_emu_thread, NULL);
    }
    pthread_join(stats, NULL);
    if (s_started && M->shutdown) {
        M->shutdown();
    }
    disks_poll();
    if (s_audio) {
        SDL_DestroyAudioStream(s_audio);
    }
    SDL_DestroyTexture(s_texture);
    SDL_DestroyRenderer(s_renderer);
    SDL_DestroyWindow(s_window);
    SDL_Quit();
    EMU_LOGI(TAG, "bye");
    return 0;
}
