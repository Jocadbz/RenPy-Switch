/* Native boot-time game picker — see game_picker.h. Runs BEFORE Python/Ren'Py:
   it draws its own SDL window, and on selection tears that window down (but leaves
   SDL video up, like show_logos) so Ren'Py can create its own window afterwards. */

#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <strings.h>
#include <zlib.h>

#include "game_picker.h"

#define GAMES_DIR   "sdmc:/renpy/"
#define FONT_PATH   "romfs:/Contents/renpy/common/DejaVuSans.ttf"
#define MAX_GAMES   256
#define COLS        4
#define ICON        256
#define CELL_W      320          /* 1280 / COLS */
#define TITLE_H     40
#define ROW_H       (ICON + TITLE_H + 28)
#define HEADER_H    52           /* fixed "Ren'Py Loader" banner at the top */
#define FOOTER_H    40           /* fixed version / game-count strip at the bottom */
#define TOP_MARGIN  (HEADER_H + 20)   /* grid starts below the header banner */
#define SCREEN_W    1280
#define SCREEN_H    720

/* Engine version shown in the footer. Kept in one place so it can be overridden at
   build time (-DRENPY_LOADER_VERSION=\"x.y.z\"); otherwise it tracks the built engine. */
#ifndef RENPY_LOADER_VERSION
#define RENPY_LOADER_VERSION "8.5.3"
#endif

/* ---------------------------------------------------------------------------
   Minimal ZIP reader (central directory + raw-deflate via zlib). Enough to list
   entries, detect the game root, and extract a named file (the icon).
--------------------------------------------------------------------------- */

typedef struct {
    char    *name;
    uint16_t method;   /* 0 = stored, 8 = deflate */
    uint32_t csize, usize, lho;
} zentry;

typedef struct {
    FILE   *f;
    zentry *entries;
    int     count;
} zipf;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void zip_close(zipf *z) {
    if (z->entries) {
        for (int i = 0; i < z->count; i++) free(z->entries[i].name);
        free(z->entries);
    }
    if (z->f) fclose(z->f);
    memset(z, 0, sizeof *z);
}

static int zip_open(zipf *z, const char *path) {
    memset(z, 0, sizeof *z);
    z->f = fopen(path, "rb");
    if (!z->f) return 0;

    fseek(z->f, 0, SEEK_END);
    long fsz = ftell(z->f);
    if (fsz < 22) { zip_close(z); return 0; }

    /* Find the End Of Central Directory record in the last <=64KB. */
    long scan = fsz > 66000 ? 66000 : fsz;
    uint8_t *buf = (uint8_t *)malloc(scan);
    if (!buf) { zip_close(z); return 0; }
    fseek(z->f, fsz - scan, SEEK_SET);
    if (fread(buf, 1, scan, z->f) != (size_t)scan) { free(buf); zip_close(z); return 0; }

    long eo = -1;
    for (long i = scan - 22; i >= 0; i--) {
        if (buf[i] == 0x50 && buf[i+1] == 0x4b && buf[i+2] == 0x05 && buf[i+3] == 0x06) { eo = i; break; }
    }
    if (eo < 0) { free(buf); zip_close(z); return 0; }

    uint16_t total = rd16(buf + eo + 10);
    uint32_t cdoff = rd32(buf + eo + 16);
    free(buf);
    if (total == 0) { zip_close(z); return 0; }

    z->entries = (zentry *)calloc(total, sizeof(zentry));
    if (!z->entries) { zip_close(z); return 0; }

    fseek(z->f, cdoff, SEEK_SET);
    for (int i = 0; i < total; i++) {
        uint8_t hdr[46];
        if (fread(hdr, 1, 46, z->f) != 46) break;
        if (rd32(hdr) != 0x02014b50) break;
        uint16_t nlen = rd16(hdr + 28), elen = rd16(hdr + 30), clen = rd16(hdr + 32);
        zentry *e = &z->entries[z->count];
        e->method = rd16(hdr + 10);
        e->csize  = rd32(hdr + 20);
        e->usize  = rd32(hdr + 24);
        e->lho    = rd32(hdr + 42);
        e->name   = (char *)malloc(nlen + 1);
        if (!e->name) break;
        if (fread(e->name, 1, nlen, z->f) != nlen) { free(e->name); break; }
        e->name[nlen] = 0;
        fseek(z->f, elen + clen, SEEK_CUR);
        z->count++;
    }
    return z->count > 0;
}

static zentry *zip_find(zipf *z, const char *name) {
    for (int i = 0; i < z->count; i++)
        if (strcmp(z->entries[i].name, name) == 0) return &z->entries[i];
    return NULL;
}

/* Extract an entry into a malloc'd buffer (caller frees). Returns NULL on error. */
static void *zip_extract(zipf *z, zentry *e, size_t *out_size) {
    uint8_t lh[30];
    fseek(z->f, e->lho, SEEK_SET);
    if (fread(lh, 1, 30, z->f) != 30 || rd32(lh) != 0x04034b50) return NULL;
    uint16_t nlen = rd16(lh + 26), elen = rd16(lh + 28);
    long data_off = (long)e->lho + 30 + nlen + elen;

    uint8_t *cbuf = (uint8_t *)malloc(e->csize ? e->csize : 1);
    if (!cbuf) return NULL;
    fseek(z->f, data_off, SEEK_SET);
    if (fread(cbuf, 1, e->csize, z->f) != e->csize) { free(cbuf); return NULL; }

    if (e->method == 0) { *out_size = e->csize; return cbuf; }   /* stored */

    uint8_t *ubuf = (uint8_t *)malloc(e->usize ? e->usize : 1);
    if (!ubuf) { free(cbuf); return NULL; }
    z_stream s;
    memset(&s, 0, sizeof s);
    s.next_in = cbuf; s.avail_in = e->csize;
    s.next_out = ubuf; s.avail_out = e->usize;
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK) { free(cbuf); free(ubuf); return NULL; }
    int r = inflate(&s, Z_FINISH);
    inflateEnd(&s);
    free(cbuf);
    if (r != Z_STREAM_END) { free(ubuf); return NULL; }
    *out_size = e->usize;
    return ubuf;
}

/* Detect the game root + kind. APK: an entry under assets/x-game/. ZIP: an entry
   whose path has a "game/" component. Fills kind ("apk"/"zip") and root. */
static int detect_game(zipf *z, char *kind, char *root, size_t rootsz) {
    for (int i = 0; i < z->count; i++) {
        if (strncmp(z->entries[i].name, "assets/x-game/", 14) == 0) {
            strcpy(kind, "apk");
            snprintf(root, rootsz, "assets/x-game/");
            return 1;
        }
    }
    for (int i = 0; i < z->count; i++) {
        const char *n = z->entries[i].name;
        const char *g = strstr(n, "game/");
        if (g && (g == n || g[-1] == '/')) {
            size_t plen = (size_t)(g - n) + 5;   /* include "game/" */
            if (plen < rootsz) {
                memcpy(root, n, plen);
                root[plen] = 0;
                strcpy(kind, "zip");
                return 1;
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
   Game list
--------------------------------------------------------------------------- */

typedef struct {
    char         path[600];
    char         title[128];
    char         kind[8];
    char         root[288];
    SDL_Texture *icon;      /* NULL => placeholder tile */
} game_entry;

static int ends_with_ci(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcasecmp(s + ls - lf, suffix) == 0;
}

/* Load window_icon.png from an already-opened archive into a texture. */
static SDL_Texture *load_icon(zipf *z, const char *kind, const char *root, SDL_Renderer *ren) {
    char ip[400];
    if (strcmp(kind, "apk") == 0)
        snprintf(ip, sizeof ip, "%sx-gui/x-window_icon.png", root);
    else
        snprintf(ip, sizeof ip, "%sgui/window_icon.png", root);

    zentry *e = zip_find(z, ip);
    if (!e) return NULL;

    size_t sz = 0;
    void *png = zip_extract(z, e, &sz);
    if (!png) return NULL;

    SDL_Texture *tex = NULL;
    SDL_RWops *rw = SDL_RWFromConstMem(png, (int)sz);
    if (rw) {
        SDL_Surface *surf = IMG_Load_RW(rw, 1 /* free rw */);
        if (surf) {
            tex = SDL_CreateTextureFromSurface(ren, surf);
            SDL_FreeSurface(surf);
        }
    }
    free(png);
    return tex;
}

/* Minimal extractor for a top-level "key": "value" string in flat JSON. Assumes a
   NUL-terminated buffer. Returns 1 and fills out on success. The token search is
   quote-delimited ("name"), so it won't match keys like "icon_name". */
static int json_get_string(const char *json, const char *key, char *out, size_t osz) {
    char tok[64];
    int n = snprintf(tok, sizeof tok, "\"%s\"", key);
    const char *p = strstr(json, tok);
    if (!p) return 0;
    p += n;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return 0;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < osz) {
        if (*p == '\\' && p[1]) p++;   /* skip the backslash, copy the escaped char */
        out[o++] = *p++;
    }
    out[o] = 0;
    return o > 0;
}

/* APK games carry a human name in assets/x-android.json (key "name"), and also in
   the game's x-cache/x-build_info.json. Try both; return 1 if a name was found. */
static int apk_name_from_json(zipf *z, const char *root, char *out, size_t osz) {
    char bi[400];
    snprintf(bi, sizeof bi, "%sx-cache/x-build_info.json", root);
    const char *candidates[2] = { "assets/x-android.json", bi };

    for (int c = 0; c < 2; c++) {
        zentry *e = zip_find(z, candidates[c]);
        if (!e) continue;
        size_t sz = 0;
        void *raw = zip_extract(z, e, &sz);
        if (!raw) continue;
        char *js = (char *)malloc(sz + 1);
        if (!js) { free(raw); continue; }
        memcpy(js, raw, sz);
        js[sz] = 0;
        free(raw);
        int ok = json_get_string(js, "name", out, osz);
        free(js);
        if (ok && out[0]) return 1;
    }
    return 0;
}

/* Boot "Loading..." screen with progress; defined below in the rendering helpers,
   forward-declared here so scan_games() can call it. */
static void draw_loading(SDL_Renderer *ren, SDL_Texture *logo, TTF_Font *big,
                         TTF_Font *small, int cur, int total);

static int scan_games(game_entry *games, int max, SDL_Renderer *ren,
                      TTF_Font *big, TTF_Font *small) {
    /* Ren'Py logo behind the loading text (drawn at 50% opacity); reuse splash.png. */
    SDL_Texture *logo = IMG_LoadTexture(ren, "romfs:/Contents/splash.png");

    struct dirent *de;

    /* Pass 1: count candidate files so the loading screen can show "game x of y".
       We re-open the directory for pass 2 rather than rewinddir(), which is unreliable
       on the Switch SD devoptab — it left the stream at EOF, so pass 2 found nothing
       and the picker reported "no games". */
    int total = 0;
    DIR *d = opendir(GAMES_DIR);
    if (!d) { printf("[picker] cannot open %s\n", GAMES_DIR); if (logo) SDL_DestroyTexture(logo); return 0; }
    while ((de = readdir(d))) {
        const char *fn = de->d_name;
        if (ends_with_ci(fn, ".zip") || ends_with_ci(fn, ".apk")) total++;
    }
    closedir(d);

    /* Pass 2: process each candidate, updating the loading screen as we go. */
    d = opendir(GAMES_DIR);
    if (!d) { printf("[picker] cannot re-open %s\n", GAMES_DIR); if (logo) SDL_DestroyTexture(logo); return 0; }

    int n = 0, idx = 0;
    while ((de = readdir(d)) && n < max) {
        const char *fn = de->d_name;
        if (!(ends_with_ci(fn, ".zip") || ends_with_ci(fn, ".apk"))) continue;

        idx++;
        draw_loading(ren, logo, big, small, idx, total);   /* show progress before the slow read */

        game_entry g;
        memset(&g, 0, sizeof g);
        snprintf(g.path, sizeof g.path, "%s%s", GAMES_DIR, fn);

        zipf z;
        if (!zip_open(&z, g.path)) { printf("[picker] not a zip: %s\n", fn); continue; }
        if (!detect_game(&z, g.kind, g.root, sizeof g.root)) {
            printf("[picker] no game/ in %s\n", fn);
            zip_close(&z);
            continue;
        }

        /* Title: APKs carry a real name in assets/x-android.json (or the game's
           x-build_info.json); prefer that. Otherwise (and for zips) use the filename
           without its extension. */
        int got_title = 0;
        if (strcmp(g.kind, "apk") == 0)
            got_title = apk_name_from_json(&z, g.root, g.title, sizeof g.title);
        if (!got_title) {
            snprintf(g.title, sizeof g.title, "%s", fn);
            char *dot = strrchr(g.title, '.');
            if (dot) *dot = 0;
        }

        g.icon = load_icon(&z, g.kind, g.root, ren);
        zip_close(&z);

        games[n++] = g;
        printf("[picker] game: '%s' (kind=%s, root=%s, icon=%s)\n",
               g.title, g.kind, g.root, g.icon ? "yes" : "placeholder");
    }
    if (logo) SDL_DestroyTexture(logo);
    closedir(d);
    return n;
}

/* Load the SELECTED game's gui/main_menu.png into a texture, for use as the boot
   splash. Reads the archive/kind/root from the env vars the picker set on selection.
   Returns NULL if there's no selection or the game ships no main_menu.png; the caller
   then falls back to the default splash.png. Caller owns the returned texture. */
SDL_Texture *load_selected_game_splash(SDL_Renderer *ren) {
    const char *path = getenv("RENPY_SWITCH_GAME_ARCHIVE");
    const char *kind = getenv("RENPY_SWITCH_GAME_KIND");
    const char *root = getenv("RENPY_SWITCH_GAME_ROOT");
    if (!path || !kind || !root) return NULL;

    zipf z;
    if (!zip_open(&z, path)) return NULL;

    char ip[400];
    if (strcmp(kind, "apk") == 0)
        snprintf(ip, sizeof ip, "%sx-gui/x-main_menu.png", root);   /* apk x-prefixes */
    else
        snprintf(ip, sizeof ip, "%sgui/main_menu.png", root);

    SDL_Texture *tex = NULL;
    zentry *e = zip_find(&z, ip);
    if (e) {
        size_t sz = 0;
        void *png = zip_extract(&z, e, &sz);
        if (png) {
            SDL_RWops *rw = SDL_RWFromConstMem(png, (int)sz);
            if (rw) {
                SDL_Surface *surf = IMG_Load_RW(rw, 1 /* free rw */);
                if (surf) {
                    tex = SDL_CreateTextureFromSurface(ren, surf);
                    SDL_FreeSurface(surf);
                }
            }
            free(png);
        }
    }
    zip_close(&z);
    return tex;
}

/* ---------------------------------------------------------------------------
   Rendering helpers
--------------------------------------------------------------------------- */

static void draw_text_centered(SDL_Renderer *ren, TTF_Font *font, const char *text,
                               int cx, int y, int max_w, SDL_Color col) {
    if (!font || !text[0]) return;
    SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, col);
    if (!s) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    int w = s->w, h = s->h;
    SDL_FreeSurface(s);
    if (!t) return;
    if (w > max_w) w = max_w;              /* crude clip for long titles */
    SDL_Rect dst = { cx - w / 2, y, w, h };
    SDL_RenderCopy(ren, t, NULL, &dst);
    SDL_DestroyTexture(t);
}

/* Draw text with its LEFT edge at x (top at y). */
static void draw_text_left(SDL_Renderer *ren, TTF_Font *font, const char *text,
                           int x, int y, SDL_Color col) {
    if (!font || !text || !text[0]) return;
    SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, col);
    if (!s) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    int w = s->w, h = s->h;
    SDL_FreeSurface(s);
    if (!t) return;
    SDL_Rect dst = { x, y, w, h };
    SDL_RenderCopy(ren, t, NULL, &dst);
    SDL_DestroyTexture(t);
}

/* Draw text with its RIGHT edge at x (top at y). */
static void draw_text_right(SDL_Renderer *ren, TTF_Font *font, const char *text,
                            int x, int y, SDL_Color col) {
    if (!font || !text || !text[0]) return;
    SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, col);
    if (!s) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    int w = s->w, h = s->h;
    SDL_FreeSurface(s);
    if (!t) return;
    SDL_Rect dst = { x - w, y, w, h };
    SDL_RenderCopy(ren, t, NULL, &dst);
    SDL_DestroyTexture(t);
}

/* Boot "Loading..." screen: the Ren'Py logo (splash.png) at 50% opacity behind a
   centered "Loading..." with a smaller "Reading game x of y" progress line under it. */
static void draw_loading(SDL_Renderer *ren, SDL_Texture *logo, TTF_Font *big,
                         TTF_Font *small, int cur, int total) {
    SDL_SetRenderDrawColor(ren, 20, 22, 28, 255);
    SDL_RenderClear(ren);

    /* Logo behind at 50% opacity, contain-fit centered (never upscaled). */
    if (logo) {
        int lw = 0, lh = 0;
        SDL_QueryTexture(logo, NULL, NULL, &lw, &lh);
        if (lw > 0 && lh > 0) {
            double sx = (double)SCREEN_W / lw, sy = (double)SCREEN_H / lh;
            double s = sx < sy ? sx : sy;
            if (s > 1.0) s = 1.0;
            int dw = (int)(lw * s), dh = (int)(lh * s);
            SDL_Rect dst = { (SCREEN_W - dw) / 2, (SCREEN_H - dh) / 2, dw, dh };
            SDL_SetTextureBlendMode(logo, SDL_BLENDMODE_BLEND);
            SDL_SetTextureAlphaMod(logo, 128);          /* ~50% */
            SDL_RenderCopy(ren, logo, NULL, &dst);
        }
    }

    SDL_Color white = { 235, 235, 235, 255 };
    SDL_Color dim   = { 170, 170, 170, 255 };
    draw_text_centered(ren, big, "Loading...", SCREEN_W / 2, SCREEN_H / 2 + 130, SCREEN_W - 40, white);

    if (total > 0) {
        char buf[64];
        snprintf(buf, sizeof buf, "Reading game %d of %d", cur, total);
        draw_text_centered(ren, small, buf, SCREEN_W / 2, SCREEN_H / 2 + 165, SCREEN_W - 40, dim);
    }

    SDL_RenderPresent(ren);
}

/* ---------------------------------------------------------------------------
   Public entry point
--------------------------------------------------------------------------- */

int game_picker_run(void) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        printf("[picker] SDL init failed: %s\n", SDL_GetError());
        return 0;
    }
    IMG_Init(IMG_INIT_PNG);
    TTF_Init();

    SDL_Window *win = SDL_CreateWindow("Picker", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       SCREEN_W, SCREEN_H, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
    if (!win) { printf("[picker] window failed: %s\n", SDL_GetError()); return 0; }
    /* No PRESENTVSYNC: on the Switch the GL swap can block on a vblank/fence that
       never returns (the same reason Ren'Py runs with vsync off). SDL_Delay paces us. */
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) { SDL_DestroyWindow(win); printf("[picker] renderer failed: %s\n", SDL_GetError()); return 0; }
    SDL_RenderSetLogicalSize(ren, SCREEN_W, SCREEN_H);

    TTF_Font *font       = TTF_OpenFont(FONT_PATH, 22);
    TTF_Font *title_font = TTF_OpenFont(FONT_PATH, 30);

    /* Open all controllers so any pad works. */
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);

    game_entry *games = (game_entry *)calloc(MAX_GAMES, sizeof(game_entry));
    int count = games ? scan_games(games, MAX_GAMES, ren, title_font, font) : 0;

    int chosen = -1, sel = 0, scroll = 0;
    int running = 1;
    int follow_sel = 0;                 /* snap view to `sel` only right after D-pad nav */
    int touch_down = 0, touch_moved = 0;
    int touch_start_x = 0, touch_start_y = 0, touch_last_y = 0;
    SDL_TouchID touch_id = 0;

    SDL_Color white = { 235, 235, 235, 255 };
    SDL_Color dim   = { 150, 150, 150, 255 };

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (count == 0) continue;
            if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (ev.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  if (sel > 0)         { sel--;        follow_sel = 1; } break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if (sel < count - 1) { sel++;        follow_sel = 1; } break;
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:    if (sel - COLS >= 0) { sel -= COLS;  follow_sel = 1; } break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  if (sel + COLS < count) { sel += COLS; follow_sel = 1; } break;
                    case SDL_CONTROLLER_BUTTON_A:          chosen = sel; running = 0; break;
                    default: break;
                }
            } else if (ev.type == SDL_FINGERDOWN) {
                /* Just remember where the touch began. Scrolling is driven by polling
                   the live finger position each frame (below); tap-vs-swipe is decided
                   on release. NOTHING is selected on touch-down. */
                touch_down    = 1;
                touch_moved   = 0;
                touch_id      = ev.tfinger.touchId;
                touch_start_x = (int)(ev.tfinger.x * SCREEN_W);
                touch_start_y = (int)(ev.tfinger.y * SCREEN_H);
                touch_last_y  = touch_start_y;
            } else if (ev.type == SDL_FINGERUP) {
                /* Compare release to start: if the finger travelled, it was a swipe
                   (scrolled, selects nothing). Otherwise it's a tap -> select. This
                   works even if the platform never sent motion events. */
                int ux = (int)(ev.tfinger.x * SCREEN_W);
                int uy = (int)(ev.tfinger.y * SCREEN_H);
                int adx = ux - touch_start_x, ady = uy - touch_start_y;
                if (adx < 0) adx = -adx;
                if (ady < 0) ady = -ady;
                if (adx > 12 || ady > 12) touch_moved = 1;
                if (touch_down && !touch_moved) {
                    int tx = touch_start_x, ty = touch_start_y;
                    for (int i = 0; i < count; i++) {
                        int col = i % COLS, row = i / COLS;
                        int ix = col * CELL_W + (CELL_W - ICON) / 2;
                        int iy = TOP_MARGIN + row * ROW_H - scroll;
                        if (tx >= ix && tx < ix + ICON && ty >= iy && ty < iy + ICON) {
                            chosen = i; running = 0; break;
                        }
                    }
                }
                touch_down = 0;
            }
        }

        /* Drag-to-scroll: poll the live finger position every frame while a touch is
           held. This does not depend on SDL_FINGERMOTION events being delivered. */
        if (touch_down && count > 0 && SDL_GetNumTouchFingers(touch_id) > 0) {
            SDL_Finger *fin = SDL_GetTouchFinger(touch_id, 0);
            if (fin) {
                int y = (int)(fin->y * SCREEN_H);
                int x = (int)(fin->x * SCREEN_W);
                scroll -= (y - touch_last_y);      /* the list follows the finger */
                touch_last_y = y;
                int dx = x - touch_start_x, dy = y - touch_start_y;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (dx > 12 || dy > 12) touch_moved = 1;   /* past this, it's a swipe */
            }
        }

        if (count > 0) {
            /* After D-pad navigation, snap the view so the selected row is visible.
               Touch swipes scroll freely and deliberately do NOT trigger this. */
            if (follow_sel) {
                int row = sel / COLS;
                int rowtop = TOP_MARGIN + row * ROW_H - scroll;
                if (rowtop < TOP_MARGIN) scroll -= (TOP_MARGIN - rowtop);
                int rowbot = rowtop + ROW_H;
                if (rowbot > SCREEN_H - FOOTER_H) scroll += (rowbot - (SCREEN_H - FOOTER_H));
                follow_sel = 0;
            }

            /* Clamp scroll to the content range so touch swipes can't overscroll past
               the first/last row. */
            int rows = (count + COLS - 1) / COLS;
            int max_scroll = TOP_MARGIN + rows * ROW_H - (SCREEN_H - FOOTER_H);
            if (max_scroll < 0) max_scroll = 0;
            if (scroll < 0) scroll = 0;
            if (scroll > max_scroll) scroll = max_scroll;
        }

        SDL_SetRenderDrawColor(ren, 20, 22, 28, 255);
        SDL_RenderClear(ren);

        if (count == 0) {
            draw_text_centered(ren, title_font, "No games found in sdmc:/renpy/",
                               SCREEN_W / 2, SCREEN_H / 2 - 40, SCREEN_W - 80, white);
            draw_text_centered(ren, font, "Put a Ren'Py .zip or .apk there and relaunch.",
                               SCREEN_W / 2, SCREEN_H / 2 + 8, SCREEN_W - 80, dim);
        } else {
            for (int i = 0; i < count; i++) {
                int col = i % COLS, row = i / COLS;
                int ix = col * CELL_W + (CELL_W - ICON) / 2;
                int iy = TOP_MARGIN + row * ROW_H - scroll;
                if (iy + ROW_H < 0 || iy > SCREEN_H) continue;   /* offscreen */

                if (i == sel) {   /* highlight border */
                    SDL_Rect hl = { ix - 8, iy - 8, ICON + 16, ICON + 16 };
                    SDL_SetRenderDrawColor(ren, 90, 160, 255, 255);
                    SDL_RenderFillRect(ren, &hl);
                }

                SDL_Rect dst = { ix, iy, ICON, ICON };
                if (games[i].icon) {
                    SDL_RenderCopy(ren, games[i].icon, NULL, &dst);
                } else {
                    SDL_SetRenderDrawColor(ren, 60, 64, 72, 255);
                    SDL_RenderFillRect(ren, &dst);
                    draw_text_centered(ren, title_font, "?", ix + ICON / 2, iy + ICON / 2 - 20,
                                       ICON, dim);
                }

                draw_text_centered(ren, font, games[i].title, ix + ICON / 2, iy + ICON + 6,
                                   CELL_W - 8, i == sel ? white : dim);
            }
        }

        /* Fixed chrome, drawn last so it sits above any scrolled icons: a header
           banner ("Ren'Py Loader") and a footer strip (engine version + game count). */
        SDL_SetRenderDrawColor(ren, 14, 15, 20, 255);
        SDL_Rect topbar = { 0, 0, SCREEN_W, HEADER_H };
        SDL_RenderFillRect(ren, &topbar);
        SDL_Rect botbar = { 0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H };
        SDL_RenderFillRect(ren, &botbar);

        draw_text_centered(ren, title_font, "Ren'Py Loader", SCREEN_W / 2, 8, SCREEN_W - 40, white);

        char verbuf[64], cntbuf[48];
        snprintf(verbuf, sizeof verbuf, "Ren'Py Version: %s", RENPY_LOADER_VERSION);
        snprintf(cntbuf, sizeof cntbuf, "Games found: %d", count);
        int footer_y = SCREEN_H - FOOTER_H + 8;
        draw_text_left(ren, font, verbuf, 20, footer_y, dim);
        draw_text_right(ren, font, cntbuf, SCREEN_W - 20, footer_y, dim);

        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    /* Record the choice as env vars for renpy.loader.find_switch_archive(). */
    int rv = 0;
    if (chosen >= 0 && chosen < count) {
        setenv("RENPY_SWITCH_GAME_ARCHIVE", games[chosen].path, 1);
        setenv("RENPY_SWITCH_GAME_KIND",    games[chosen].kind, 1);
        setenv("RENPY_SWITCH_GAME_ROOT",    games[chosen].root, 1);
        printf("[picker] selected '%s' -> %s (%s, %s)\n",
               games[chosen].title, games[chosen].path, games[chosen].kind, games[chosen].root);
        rv = 1;
    }

    /* Tear the picker fully down — including SDL video — EXACTLY like show_logos, so
       Ren'Py then re-initialises SDL/Mesa from a clean state (the known-good boot
       path: one GPU teardown+reinit, which is proven to work within a run). */
    for (int i = 0; i < count; i++)
        if (games[i].icon) SDL_DestroyTexture(games[i].icon);
    free(games);
    if (font) TTF_CloseFont(font);
    if (title_font) TTF_CloseFont(title_font);
    TTF_Quit();
    IMG_Quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);

    return rv;
}
