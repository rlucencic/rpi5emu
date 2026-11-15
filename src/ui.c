/*
 * The setup screen, as the boards' (esp32emu's setup_ui.c) but drawn with
 * SDL into the window: the machine's choice rows, the sound, the drives
 * and their images, Start / Back / Restart, Reset, Quit. Shown at start
 * (unless --start) and again with Ctrl+Alt+M; the machine waits while it's
 * open. Laid out on a 1024 x 600 canvas, scaled to the window.
 *
 * Text through SDL3_ttf when the build has it (a system font), else SDL's
 * own 8 x 8 font.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef RPI5EMU_TTF
#include <SDL3_ttf/SDL_ttf.h>
#endif
#include "emu/emu.h"
#include "emu/log.h"
#include "emu/settings.h"
#include "emu_internal.h"

static const char *TAG = "setup";

#define CANVAS_W    1024
#define CANVAS_H    600
#define MAX_ROWS    8
#define MAX_HITS    96

enum { PAGE_MAIN, PAGE_DISKS, PAGE_IMAGES };

typedef enum { F_SMALL, F_NORMAL, F_BIG, F_COUNT } font_t;
static const int font_size[F_COUNT] = { 17, 22, 32 };

typedef struct {
    Uint8 r, g, b;
} rgb_t;

static const rgb_t C_BG = { 0x1b, 0x20, 0x2a }, C_CARD = { 0x26, 0x2d, 0x3a }, C_BUTTON = { 0x38, 0x41, 0x54 },
                   C_HOVER = { 0x47, 0x52, 0x6a }, C_ON = { 0x2f, 0x7d, 0xe1 }, C_TEXT = { 0xf2, 0xf4, 0xf8 },
                   C_DIM = { 0x9a, 0xa4, 0xb5 }, C_WARN = { 0xf0, 0xb0, 0x40 }, C_RED = { 0xc8, 0x45, 0x45 };

/* What a click does */
enum {
    A_NONE, A_CHOICE, A_MUTE, A_SLIDER, A_START, A_RESET, A_QUIT, A_DISKS, A_BACK, A_INSERT, A_EJECT,
    A_NEW, A_OFFER, A_IMAGE, A_FULLSCREEN,
};

typedef struct {
    SDL_FRect r;                /* canvas units */
    int action, a, b;
} hit_t;

static struct {
    SDL_Renderer *renderer;
    bool open, holding;
    int page, drive;            /* PAGE_IMAGES: for this drive */
    int before[MAX_ROWS];       /* the choices when it opened */
    bool reset;
    float k, ox, oy;            /* canvas to window pixels */
    int ww, wh;                 /* the window when last drawn */
    float mx, my;               /* the mouse, canvas units */
    bool sliding;
    hit_t hits[MAX_HITS];
    int hit_count;
    char **images;              /* PAGE_IMAGES' list */
    int image_count, scroll;
    bool dirty;
#ifdef RPI5EMU_TTF
    TTF_Font *fonts[F_COUNT];
    float font_k;
#endif
} ui;

/* ---- Text ---- */

#ifdef RPI5EMU_TTF
static const char *const font_files[] = {
    "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
    "/usr/share/fonts/truetype/ubuntu/Ubuntu[wdth,wght].ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    NULL,
};

static void fonts_for(float k)
{
    if (ui.fonts[0] && ui.font_k == k) {
        return;
    }
    ui.font_k = k;
    for (int f = 0; f < F_COUNT; f++) {
        float size = font_size[f] * k * 0.85f;
        if (ui.fonts[f]) {
            TTF_SetFontSize(ui.fonts[f], size);
            continue;
        }
        for (const char *const *p = font_files; *p && !ui.fonts[f]; p++) {
            ui.fonts[f] = TTF_OpenFont(*p, size);
        }
    }
}
#endif

static float text_width(font_t f, const char *s)
{
#ifdef RPI5EMU_TTF
    if (ui.fonts[f]) {
        int w = 0, h = 0;
        TTF_GetStringSize(ui.fonts[f], s, 0, &w, &h);
        return w / ui.k;
    }
#endif
    return strlen(s) * font_size[f] * 0.75f;
}

/* Left at x, centred on the line y .. y + font size */
static void text(font_t f, float x, float y, rgb_t c, const char *s)
{
    if (s == NULL || s[0] == 0) {
        return;
    }
    float px = ui.ox + x * ui.k, py = ui.oy + y * ui.k;
#ifdef RPI5EMU_TTF
    if (ui.fonts[f]) {
        SDL_Surface *surface = TTF_RenderText_Blended(ui.fonts[f], s, 0, (SDL_Color){ c.r, c.g, c.b, 255 });
        if (surface) {
            SDL_Texture *t = SDL_CreateTextureFromSurface(ui.renderer, surface);
            if (t) {
                SDL_FRect dst = { SDL_roundf(px), SDL_roundf(py + (font_size[f] * ui.k - surface->h) / 2),
                                  (float)surface->w, (float)surface->h };
                SDL_RenderTexture(ui.renderer, t, NULL, &dst);
                SDL_DestroyTexture(t);
            }
            SDL_DestroySurface(surface);
        }
        return;
    }
#endif
    /* 8 x 8 pixels a character, 6 of them drawn: a character 0.75 of the size wide */
    float scale = font_size[f] * 0.75f * ui.k / 8;
    SDL_SetRenderScale(ui.renderer, scale, scale);
    SDL_SetRenderDrawColor(ui.renderer, c.r, c.g, c.b, 255);
    SDL_RenderDebugText(ui.renderer, px / scale, (py + (font_size[f] * ui.k - 8 * scale) / 2) / scale, s);
    SDL_SetRenderScale(ui.renderer, 1, 1);
}

/* As much of s as fits in w (with "..." at its end) */
static void fit(font_t f, const char *s, float w, char *out, size_t len)
{
    char src[1400];
    snprintf(src, sizeof(src), "%s", s);       /* out may be s */
    snprintf(out, len, "%s", src);
    if (text_width(f, out) <= w) {
        return;
    }
    size_t n = strlen(src);
    while (n > 0) {
        n--;
        snprintf(out, len, "%.*s...", (int)n, src);
        if (text_width(f, out) <= w) {
            return;
        }
    }
}

/* ---- Shapes and buttons ---- */

static void fill(float x, float y, float w, float h, rgb_t c)
{
    SDL_SetRenderDrawColor(ui.renderer, c.r, c.g, c.b, 255);
    SDL_FRect r = { ui.ox + x * ui.k, ui.oy + y * ui.k, w * ui.k, h * ui.k };
    SDL_RenderFillRect(ui.renderer, &r);
}

static bool inside(const SDL_FRect *r, float x, float y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static void add_hit(float x, float y, float w, float h, int action, int a, int b)
{
    if (ui.hit_count < MAX_HITS) {
        ui.hits[ui.hit_count++] = (hit_t){ { x, y, w, h }, action, a, b };
    }
}

/* A button with its text centred (a "\n" in it: a smaller second line) */
static void button(float x, float y, float w, float h, const char *label, bool on, rgb_t base, int action, int a,
                   int b)
{
    SDL_FRect r = { x, y, w, h };
    bool hover = action != A_NONE && inside(&r, ui.mx, ui.my);
    fill(x, y, w, h, on ? C_ON : hover ? C_HOVER : base);
    char first[128], second[128] = "";
    size_t n = strcspn(label, "\n");
    snprintf(first, sizeof(first), "%.*s", (int)n, label);
    if (label[n] == '\n') {
        snprintf(second, sizeof(second), "%s", label + n + 1);
    }
    char shown[128];
    fit(F_NORMAL, first, w - 12, shown, sizeof(shown));
    if (second[0]) {
        text(F_NORMAL, x + (w - text_width(F_NORMAL, shown)) / 2, y + h / 2 - font_size[F_NORMAL] + 2, C_TEXT,
             shown);
        char small[128];
        fit(F_SMALL, second, w - 12, small, sizeof(small));
        text(F_SMALL, x + (w - text_width(F_SMALL, small)) / 2, y + h / 2 + 1, C_DIM, small);
    } else {
        text(F_NORMAL, x + (w - text_width(F_NORMAL, shown)) / 2, y + (h - font_size[F_NORMAL]) / 2,
             action == A_NONE ? C_DIM : C_TEXT, shown);
    }
    if (action != A_NONE) {
        add_hit(x, y, w, h, action, a, b);
    }
}

/* ---- Pages ---- */

static const char *const *row_names(int i)
{
    const emu_machine_t *m = emu_machine();
    const char *const *names = m->choice_names ? m->choice_names(i) : NULL;
    return names ? names : m->choices[i].names;
}

/* A non-live choice differs from when the screen opened: Restart */
static bool changed(void)
{
    const emu_machine_t *m = emu_machine();
    for (int i = 0; i < m->choice_count && i < MAX_ROWS; i++) {
        if (!m->choices[i].live && m->choices[i].get() != ui.before[i]) {
            return true;
        }
    }
    return false;
}

static void page_main(void)
{
    const emu_machine_t *m = emu_machine();
    char line[1400];
    text(F_BIG, 32, 22, C_TEXT, m->name);
    char shown[1200];
    snprintf(line, sizeof(line), "Data folder %s", emu_short_path(emu_data_dir(), shown, sizeof(shown)));
    fit(F_SMALL, line, 600, line, sizeof(line));
    text(F_SMALL, 32, 66, C_DIM, line);
    if (emu_drive_info()[0]) {
        char info[600];
        fit(F_SMALL, emu_drive_info(), 600, info, sizeof(info));
        text(F_SMALL, 32, 88, C_DIM, info);
    }
    if (emu_message()[0]) {
        char message[600];
        fit(F_SMALL, emu_message(), CANVAS_W - 64, message, sizeof(message));
        text(F_SMALL, 32, 112, C_WARN, message);
    }
    /* top right: Reset (when running), full screen, Quit */
    button(CANVAS_W - 132, 24, 100, 40, "Quit", false, C_RED, A_QUIT, 0, 0);
    button(CANVAS_W - 278, 24, 136, 40, "Full screen", false, C_BUTTON, A_FULLSCREEN, 0, 0);
    if (emu_started()) {
        button(CANVAS_W - 388, 24, 100, 40, "Reset", false, C_BUTTON, A_RESET, 0, 0);
    }

    /* the card: the choice rows, then the sound */
    int rows = m->choice_count < MAX_ROWS ? m->choice_count : MAX_ROWS;
    float top = 142, row_h = (rows + 1) * 58 <= 360 ? 58 : 360.0f / (rows + 1);
    fill(20, top - 10, CANVAS_W - 40, (rows + 1) * row_h + 20, C_CARD);
    const float bx = 230, bw_all = CANVAS_W - 40 - bx;
    for (int i = 0; i < rows; i++) {
        float y = top + i * row_h;
        text(F_NORMAL, 40, y + (row_h - 8 - font_size[F_NORMAL]) / 2, C_TEXT, m->choices[i].label);
        const char *const *names = row_names(i);
        int n = 0;
        while (names[n] && names[n][0]) {
            n++;
        }
        if (n == 0) {
            continue;
        }
        float gap = 8, bw = (bw_all - gap * (n - 1)) / n;
        bw = bw > 190 ? 190 : bw;
        int now = m->choices[i].get();
        for (int j = 0; j < n; j++) {
            button(bx + j * (bw + gap), y, bw, row_h - 8, names[j], j == now, C_BUTTON, A_CHOICE, i, j);
        }
    }
    /* the sound: mute, then a slider */
    float y = top + rows * row_h;
    text(F_NORMAL, 40, y + (row_h - 8 - font_size[F_NORMAL]) / 2, C_TEXT, "Sound");
    button(bx, y, 120, row_h - 8, emu_muted() ? "Muted" : "Mute", emu_muted(), C_BUTTON, A_MUTE, 0, 0);
    float sx = bx + 150, sw = CANVAS_W - 40 - sx - 90, sy = y + (row_h - 8) / 2;
    fill(sx, sy - 3, sw, 6, C_BUTTON);
    float knob = sx + sw * emu_volume() / 100.0f;
    fill(sx, sy - 3, knob - sx, 6, emu_muted() ? C_DIM : C_ON);
    fill(knob - 8, sy - 14, 16, 28, emu_muted() ? C_DIM : C_TEXT);
    snprintf(line, sizeof(line), "%d", emu_volume());
    text(F_NORMAL, sx + sw + 24, y + (row_h - 8 - font_size[F_NORMAL]) / 2, emu_muted() ? C_DIM : C_TEXT, line);
    add_hit(sx - 10, y, sw + 20, row_h - 8, A_SLIDER, 0, 0);

    /* bottom: Disks, then Start / Back / Restart */
    float by = CANVAS_H - 70;
    if (m->disks && disks_ready()) {
        button(32, by, 160, 48, disks_config()->button ? disks_config()->button : "Disks", false, C_BUTTON,
               A_DISKS, 0, 0);
    }
    const char *go = !emu_started() ? "Start" : changed() ? "Restart" : "Back";
    button(CANVAS_W - 212, by, 180, 48, go, true, C_BUTTON, A_START, 0, 0);
    const char *keys = "Ctrl+Alt+M this screen   Ctrl+Alt+F full screen   Esc back";
    text(F_SMALL, (CANVAS_W - text_width(F_SMALL, keys)) / 2, by + 15, C_DIM, keys);
}

static void page_disks(void)
{
    const disks_config_t *d = disks_config();
    char line[1400];
    text(F_BIG, 32, 22, C_TEXT, d->title ? d->title : "Disks");
    fit(F_SMALL, d->description ? d->description : "", CANVAS_W - 64, line, sizeof(line));
    text(F_SMALL, 32, 66, C_DIM, line);
    char shown[1200];
    snprintf(line, sizeof(line), "Images in %s (copy them there, or drop them on the window)",
             emu_short_path(disks_folder(), shown, sizeof(shown)));
    fit(F_SMALL, line, CANVAS_W - 64, line, sizeof(line));
    text(F_SMALL, 32, 88, C_DIM, line);

    int drives = d->drives;
    float top = 128, row_h = drives <= 4 ? 84 : 72;
    fill(20, top - 10, CANVAS_W - 40, drives * row_h + 10, C_CARD);
    for (int i = 0; i < drives; i++) {
        float y = top + i * row_h;
        text(F_NORMAL, 40, y + 10, C_TEXT, d->drive_names[i]);
        const char *name = disks_name(i);
        const char *shown = name ? (strrchr(name, '/') ? strrchr(name, '/') + 1 : name) : "empty";
        char fitted[256];
        fit(F_NORMAL, shown, 420, fitted, sizeof(fitted));
        text(F_NORMAL, 170, y + 10, name ? C_TEXT : C_DIM, fitted);
        const char *offer = disks_offer(i);
        if (offer) {
            fit(F_SMALL, offer, 560, line, sizeof(line));
            text(F_SMALL, 170, y + 40, C_WARN, line);
        }
        float bx = CANVAS_W - 40 - 3 * 128;
        button(bx, y + 4, 120, 40, "Insert...", false, C_BUTTON, A_INSERT, i, 0);
        if (name) {
            button(bx + 128, y + 4, 120, 40, "Eject", false, C_BUTTON, A_EJECT, i, 0);
        }
        if (d->create && (d->takes_blank == NULL || d->takes_blank(i))) {
            button(bx + 256, y + 4, 120, 40, "New blank", false, C_BUTTON, A_NEW, i, 0);
        }
        if (offer) {
            button(bx + 128, y + 4 + 40 + 2, 248, row_h - 52, emu_started() ? "Set up and restart" : "Set up",
                   true, C_BUTTON, A_OFFER, i, 0);
        }
    }
    button(32, CANVAS_H - 70, 160, 48, "Back", false, C_BUTTON, A_BACK, 0, 0);
}

static void free_images(void)
{
    if (ui.images) {
        disks_free_list(ui.images, ui.image_count);
    }
    ui.images = NULL;
    ui.image_count = 0;
}

/* The images the drive takes */
static void load_images(int drive)
{
    free_images();
    char **all;
    int n = disks_list(&all);
    const disks_config_t *d = disks_config();
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (d->accepts == NULL || d->accepts(drive, all[i])) {
            all[kept++] = all[i];
        } else {
            free(all[i]);
        }
    }
    ui.images = all;
    ui.image_count = kept;
    ui.scroll = 0;
}

#define LIST_TOP    128
#define LIST_ROW    38
#define LIST_ROWS   10

static void page_images(void)
{
    const disks_config_t *d = disks_config();
    char line[1400];
    char shown[1200];
    snprintf(line, sizeof(line), "Into %s", d->drive_names[ui.drive]);
    text(F_BIG, 32, 22, C_TEXT, line);
    snprintf(line, sizeof(line), "%d image%s in %s", ui.image_count, ui.image_count == 1 ? "" : "s",
             emu_short_path(disks_folder(), shown, sizeof(shown)));
    fit(F_SMALL, line, CANVAS_W - 64, line, sizeof(line));
    text(F_SMALL, 32, 66, C_DIM, line);
    fill(20, LIST_TOP - 10, CANVAS_W - 40, LIST_ROWS * LIST_ROW + 20, C_CARD);
    if (ui.image_count == 0) {
        text(F_NORMAL, 40, LIST_TOP + 10, C_DIM, "None that this drive takes.");
    }
    const char *now = disks_name(ui.drive);
    for (int r = 0; r < LIST_ROWS && ui.scroll + r < ui.image_count; r++) {
        int i = ui.scroll + r;
        char fitted[512];
        fit(F_NORMAL, ui.images[i], CANVAS_W - 120, fitted, sizeof(fitted));
        float y = LIST_TOP + r * LIST_ROW;
        SDL_FRect rect = { 30, y, CANVAS_W - 60, LIST_ROW - 4 };
        bool current = now && strcmp(now, ui.images[i]) == 0;
        if (current || inside(&rect, ui.mx, ui.my)) {
            fill(rect.x, rect.y, rect.w, rect.h, current ? C_ON : C_HOVER);
        }
        text(F_NORMAL, 44, y + (LIST_ROW - 4 - font_size[F_NORMAL]) / 2, C_TEXT, fitted);
        add_hit(rect.x, rect.y, rect.w, rect.h, A_IMAGE, i, 0);
    }
    if (ui.image_count > LIST_ROWS) {
        snprintf(line, sizeof(line), "%d-%d of %d: the wheel or the arrow keys scroll", ui.scroll + 1,
                 ui.scroll + LIST_ROWS < ui.image_count ? ui.scroll + LIST_ROWS : ui.image_count, ui.image_count);
        text(F_SMALL, 220, CANVAS_H - 56, C_DIM, line);
    }
    button(32, CANVAS_H - 70, 160, 48, "Back", false, C_BUTTON, A_BACK, 0, 0);
}

/* ---- Open, close, act ---- */

bool ui_is_open(void)
{
    return ui.open;
}

void ui_init(SDL_Renderer *renderer)
{
    ui.renderer = renderer;
#ifdef RPI5EMU_TTF
    if (!TTF_Init()) {
        EMU_LOGW(TAG, "SDL_ttf: %s", SDL_GetError());
    }
#endif
}

void ui_open(void)
{
    if (ui.open) {
        return;
    }
    const emu_machine_t *m = emu_machine();
    for (int i = 0; i < m->choice_count && i < MAX_ROWS; i++) {
        ui.before[i] = m->choices[i].get();
    }
    ui.holding = emu_started();
    if (ui.holding) {
        emu_hold();
    }
    ui.open = true;
    ui.page = PAGE_MAIN;
    ui.reset = false;
    ui.dirty = true;
}

static void close_ui(void)
{
    free_images();
    ui.open = false;
    if (ui.holding) {
        ui.holding = false;
        emu_release();
    }
}

/* Start, Back or Restart: false while the machine can't start */
static bool go(void)
{
    if (!emu_started()) {
        if (!emu_restart()) {
            return false;
        }
    } else if (changed()) {
        EMU_LOGI(TAG, "restarting the %s with the new settings", emu_machine()->short_name);
        emu_restart_or_revert(ui.before);
    } else if (ui.reset) {
        emu_request_reset();
    }
    close_ui();
    return true;
}

typedef struct {
    int drive;
    const char *name;           /* NULL: eject; "+": a new blank one */
} drive_op_t;

static void drive_op(void *arg)
{
    drive_op_t *op = arg;
    char made[DISKS_NAME_LEN];
    if (op->name == NULL) {
        disks_eject(op->drive);
    } else if (strcmp(op->name, "+") == 0) {
        disks_create(op->drive, disks_default_size(op->drive), made, sizeof(made));
    } else if (!disks_insert(op->drive, op->name)) {
        char message[DISKS_NAME_LEN + 64];
        snprintf(message, sizeof(message), "%s didn't go into %s", op->name,
                 disks_config()->drive_names[op->drive]);
        EMU_LOGW(TAG, "%s", message);
    }
}

static void set_volume_at(float x)
{
    float sx = 230 + 150, sw = CANVAS_W - 40 - sx - 90;
    int v = (int)((x - sx) * 100 / sw + 0.5f);
    emu_set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
}

static void act(const hit_t *h)
{
    const emu_machine_t *m = emu_machine();
    switch (h->action) {
    case A_CHOICE:
        m->choices[h->a].set(h->b);         /* live ones change the machine now (it's held) */
        break;
    case A_MUTE:
        emu_set_muted(!emu_muted());
        break;
    case A_SLIDER:
        ui.sliding = true;
        set_volume_at(ui.mx);
        break;
    case A_START:
        go();
        break;
    case A_RESET:
        ui.reset = true;
        if (!changed()) {
            go();
        }
        break;
    case A_QUIT:
        close_ui();
        emu_quit();
        break;
    case A_FULLSCREEN:
        emu_toggle_fullscreen();
        break;
    case A_DISKS:
        ui.page = PAGE_DISKS;
        break;
    case A_BACK:
        ui.page = ui.page == PAGE_IMAGES ? PAGE_DISKS : PAGE_MAIN;
        free_images();
        break;
    case A_INSERT:
        ui.drive = h->a;
        load_images(h->a);
        ui.page = PAGE_IMAGES;
        break;
    case A_EJECT: {
        drive_op_t op = { h->a, NULL };
        emu_paused(drive_op, &op);
        break;
    }
    case A_NEW: {
        drive_op_t op = { h->a, "+" };
        emu_paused(drive_op, &op);
        break;
    }
    case A_OFFER:
        if (emu_take_offer(h->a)) {
            for (int i = 0; i < m->choice_count && i < MAX_ROWS; i++) {
                ui.before[i] = m->choices[i].get();     /* restarted already */
            }
            if (!emu_started()) {
                go();
            } else {
                close_ui();
            }
        }
        break;
    case A_IMAGE:
        if (h->a < ui.image_count) {
            drive_op_t op = { ui.drive, ui.images[h->a] };
            emu_paused(drive_op, &op);
            ui.page = PAGE_DISKS;
            free_images();
        }
        break;
    default:
        break;
    }
}

void ui_close_to_machine(void)
{
    if (ui.open && emu_started()) {
        go();
    }
}

static void scroll_by(int n)
{
    int max = ui.image_count > LIST_ROWS ? ui.image_count - LIST_ROWS : 0;
    ui.scroll += n;
    ui.scroll = ui.scroll < 0 ? 0 : ui.scroll > max ? max : ui.scroll;
}

static void to_canvas(float x, float y)
{
    ui.mx = (x - ui.ox) / ui.k;
    ui.my = (y - ui.oy) / ui.k;
}

/* Every event while it's open (coordinates already the renderer's) */
bool ui_event(const SDL_Event *e)
{
    if (!ui.open) {
        return false;
    }
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        to_canvas(e->motion.x, e->motion.y);
        if (ui.sliding) {
            set_volume_at(ui.mx);
        }
        ui.dirty = true;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        to_canvas(e->button.x, e->button.y);
        if (ui.dirty && ui.ww > 0) {
            ui_draw(ui.ww, ui.wh);      /* the buttons as they are now (not shown: the next draw is) */
            ui.dirty = true;
        }
        if (e->button.button == SDL_BUTTON_LEFT) {
            for (int i = ui.hit_count - 1; i >= 0; i--) {
                if (inside(&ui.hits[i].r, ui.mx, ui.my)) {
                    hit_t h = ui.hits[i];
                    act(&h);
                    break;
                }
            }
        }
        ui.dirty = true;
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        ui.sliding = false;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (ui.page == PAGE_IMAGES) {
            scroll_by(e->wheel.y > 0 ? -3 : e->wheel.y < 0 ? 3 : 0);
            ui.dirty = true;
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        switch (e->key.scancode) {
        case SDL_SCANCODE_ESCAPE:
            if (ui.page != PAGE_MAIN) {
                ui.page = ui.page == PAGE_IMAGES ? PAGE_DISKS : PAGE_MAIN;
                free_images();
            } else if (emu_started()) {
                go();
            }
            break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            if (ui.page == PAGE_MAIN) {
                go();
            }
            break;
        case SDL_SCANCODE_UP: scroll_by(-1); break;
        case SDL_SCANCODE_DOWN: scroll_by(1); break;
        case SDL_SCANCODE_PAGEUP: scroll_by(-LIST_ROWS); break;
        case SDL_SCANCODE_PAGEDOWN: scroll_by(LIST_ROWS); break;
        default: break;
        }
        ui.dirty = true;
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_EXPOSED:
        ui.dirty = true;
        break;
    default:
        break;
    }
    return true;
}

void ui_touch(void)
{
    ui.dirty = true;
}

/* A click at canvas (x, y) (the console's t: 1024 x 600, as the MaTouch's) */
void ui_click(float x, float y)
{
    if (!ui.open || ui.k <= 0) {
        return;
    }
    SDL_Event e = { .type = SDL_EVENT_MOUSE_BUTTON_DOWN };
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = true;
    e.button.x = ui.ox + x * ui.k;
    e.button.y = ui.oy + y * ui.k;
    ui_event(&e);
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.down = false;
    ui_event(&e);
}

bool ui_dirty(void)
{
    return ui.dirty;
}

void ui_draw(int ww, int wh)
{
    ui.dirty = false;
    ui.ww = ww;
    ui.wh = wh;
    ui.k = ww / (float)CANVAS_W < wh / (float)CANVAS_H ? ww / (float)CANVAS_W : wh / (float)CANVAS_H;
    ui.ox = (ww - CANVAS_W * ui.k) / 2;
    ui.oy = (wh - CANVAS_H * ui.k) / 2;
#ifdef RPI5EMU_TTF
    fonts_for(ui.k);
#endif
    SDL_SetRenderDrawColor(ui.renderer, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(ui.renderer);
    ui.hit_count = 0;
    switch (ui.page) {
    case PAGE_DISKS: page_disks(); break;
    case PAGE_IMAGES: page_images(); break;
    default: page_main(); break;
    }
}
