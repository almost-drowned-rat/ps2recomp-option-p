/*
 * game_setup_ui_win95_sdl.c - the launcher's window on Linux: game_setup_ui_win95.c (the Windows window) drawn with
 * SDL3, pixel for pixel the same page - the maintainer's rule for every port's launcher ("the more old-fashioned the
 * better") and for this port's Linux build (2026-11-21: "a direct port of the launcher with all its features and its
 * look"). The same layout constants, colours, bevels, owner-drawn buttons, sunken fields, title bar with the close
 * box, etched line and blocky progress bar; the same texts and checks. Only the system parts differ: the font is
 * Liberation Sans (SIL OFL 1.1, engine/data/fonts; MS Sans Serif is a Windows system font) at 8 pt, drawn without
 * antialiasing as Windows draws its dialog font, rasterised by stb_truetype; the dialogs are SDL3's (the desktop's
 * own file and folder pickers by its portal); the window is borderless and dragged by its title bar. The window is
 * drawn at a whole multiple of its size on a high-DPI screen (nearest-neighbour, the pixels stay square).
 * Agent L, 2026-11-21. The interface is game_setup_ui.h's.
 */
#include "game_setup_ui.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

/* Liberation Sans Regular and Bold (engine/data/fonts), built in by CMakeLists.txt */
static const unsigned char k_font_regular[] = {
#include "liberation_sans_regular.inc"
};
static const unsigned char k_font_bold[] = {
#include "liberation_sans_bold.inc"
};

typedef struct { Uint8 r, g, b; } Rgb;
static const Rgb K_FACE = {192, 192, 192}, K_LIGHT = {255, 255, 255}, K_SHADOW = {128, 128, 128}, K_DARK = {0, 0, 0},
                 K_TITLE = {0, 0, 128}, K_TITLE_TEXT = {255, 255, 255}, K_TEXT = {0, 0, 0},
                 K_PROGRESS = {0, 0, 128}, K_WINDOW = {255, 255, 255};

#define UI_W          520
#define UI_H          236               /* with one disc line; a second one adds UI_ROW */
#define UI_ROW        34
#define UI_TITLE_H    18
#define FONT_PX       11.0f             /* 8 pt at 96 dpi: Windows' -MulDiv(8, 96, 72) = an 11-pixel em */
#define FIELD_MAX     4096

enum { ID_ISO = 100, ID_ISO2, ID_TARGET, ID_FILE, ID_BROWSE = 200, ID_BROWSE2, ID_BROWSE_TARGET, ID_COPY, ID_BROWSE_FILE,
       ID_GO = 300, ID_QUIT };

typedef struct {
    int  id, x, y, w, h;
    int  button;                        /* 1: a push button, 0: an edit field */
    char label[32];
    char text[FIELD_MAX];               /* an edit field's text (UTF-8) */
    int  caret, scroll, sel_all;        /* caret (byte offset), first shown pixel, all selected */
    int  enabled;
} Ctl;

typedef struct {
    stbtt_fontinfo info;
    float          scale;
    int            ascent;              /* in pixels */
} Font;

struct XrSetupUi {
    char          title[160];
    char          extra_label[120];     /* the second disc's line ("" none) */
    int           rows, lines, has_extra;
    char          file_label[120];
    char          file_filter_name[120], file_filter[60];
    char         *file_path;
    size_t        file_n;
    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Texture  *icon;
    const void   *icon_bmp;
    size_t        icon_size;
    int           scale;
    Font          font, bold;
    int           fonts_ok;
    Ctl           ctl[12];
    int           nctl, focus, pressed, pressed_inside;
    int           failed, closed, close_down, close_inside, form_result, in_form, progress, sdl_up;
    int           dialog_open;          /* a file / folder picker is up: the page waits for its answer */
    char          dialog_path[FIELD_MAX];
    int           dialog_target, dialog_done;
    char          intro[400];
    char          status[600];
    Uint64        last_draw, caret_blink;
};

void xr_setup_ui_icon(XrSetupUi *ui, const void *bmp, size_t n)
{
    if (ui) {
        ui->icon_bmp = bmp;
        ui->icon_size = n;
    }
}

XrSetupUi *xr_setup_ui_open(const char *title)
{
    XrSetupUi *ui = (XrSetupUi *)calloc(1, sizeof *ui);
    if (ui)
        snprintf(ui->title, sizeof ui->title, "%s Setup", title ? title : "Game");
    return ui;
}

/* ── drawing ─────────────────────────────────────────────────────────── */

static void colour(XrSetupUi *ui, Rgb c) { SDL_SetRenderDrawColor(ui->ren, c.r, c.g, c.b, 255); }

static void fill(XrSetupUi *ui, int l, int t, int r, int b, Rgb c)     /* [l, r) x [t, b), as FillRect */
{
    SDL_FRect f;
    if (r <= l || b <= t)
        return;
    f.x = (float)l; f.y = (float)t; f.w = (float)(r - l); f.h = (float)(b - t);
    colour(ui, c);
    SDL_RenderFillRect(ui->ren, &f);
}

/* GDI's MoveTo + LineTo: the end point not drawn (only straight lines here) */
static void line(XrSetupUi *ui, int x0, int y0, int x1, int y1, Rgb c)
{
    if (y0 == y1) {
        if (x1 > x0) fill(ui, x0, y0, x1, y0 + 1, c);
        else         fill(ui, x1 + 1, y0, x0 + 1, y0 + 1, c);
    } else {
        if (y1 > y0) fill(ui, x0, y0, x0 + 1, y1, c);
        else         fill(ui, x0, y1 + 1, x0 + 1, y0 + 1, c);
    }
}

/* The classic two-pixel bevel: raised (buttons, the window) or sunken (fields, the progress groove). */
static void bevel(XrSetupUi *ui, int l, int t, int r, int b, int raised)
{
    const Rgb otl = raised ? K_LIGHT : K_SHADOW, obr = raised ? K_DARK : K_LIGHT;
    const Rgb itl = raised ? K_FACE : K_DARK, ibr = raised ? K_SHADOW : K_FACE;
    line(ui, l, b - 1, l, t, otl);
    line(ui, l, t, r - 1, t, otl);
    line(ui, r - 1, t, r - 1, b - 1, obr);
    line(ui, r - 1, b - 1, l - 1, b - 1, obr);
    line(ui, l + 1, b - 2, l + 1, t + 1, itl);
    line(ui, l + 1, t + 1, r - 2, t + 1, itl);
    line(ui, r - 2, t + 1, r - 2, b - 2, ibr);
    line(ui, r - 2, b - 2, l, b - 2, ibr);
}

static int font_init(Font *f, const unsigned char *data)
{
    int ascent, descent, gap;
    if (!stbtt_InitFont(&f->info, data, stbtt_GetFontOffsetForIndex(data, 0)))
        return 0;
    f->scale = stbtt_ScaleForMappingEmToPixels(&f->info, FONT_PX);
    stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &gap);
    f->ascent = (int)(ascent * f->scale + 0.5f);
    return 1;
}

/* the next code point of UTF-8 text (a bad byte as itself) */
static int next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int cp = *p, n = 0;
    if (cp < 0x80) n = 0;
    else if ((cp & 0xE0) == 0xC0) { cp &= 0x1F; n = 1; }
    else if ((cp & 0xF0) == 0xE0) { cp &= 0x0F; n = 2; }
    else if ((cp & 0xF8) == 0xF0) { cp &= 0x07; n = 3; }
    p++;
    while (n-- > 0 && (*p & 0xC0) == 0x80)
        cp = (cp << 6) | (*p++ & 0x3F);
    *s = (const char *)p;
    return cp;
}

/* a glyph's advance in whole pixels, as a bitmap font's (GDI's non-antialiased text steps by whole pixels) */
static int advance(const Font *f, int cp, int next)
{
    int adv, lsb;
    float x;
    stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
    x = adv * f->scale;
    if (next)
        x += stbtt_GetCodepointKernAdvance(&f->info, cp, next) * f->scale;
    return (int)(x + 0.5f);
}

static int text_width_n(const Font *f, const char *s, size_t n)
{
    const char *p = s, *end = s + n;
    int w = 0;
    while (p < end && *p) {
        const int cp = next_cp(&p);
        const char *q = p;
        const int next = q < end && *q ? next_cp(&q) : 0;
        w += advance(f, cp, next);
    }
    return w;
}

static int text_width(const Font *f, const char *s) { return text_width_n(f, s, strlen(s)); }

/* text with its cell's top at y (as TextOut: the ascent below y), in one colour, no antialiasing */
static void text_n(XrSetupUi *ui, const Font *f, int x, int y, const char *s, size_t n, Rgb c, int clip_l, int clip_r)
{
    const char *p = s, *end = s + n;
    unsigned char glyph[64 * 64];
    colour(ui, c);
    while (p < end && *p) {
        const int cp = next_cp(&p);
        const char *q = p;
        const int next = q < end && *q ? next_cp(&q) : 0;
        int x0, y0, x1, y1, gw, gh, gx, gy;
        stbtt_GetCodepointBitmapBox(&f->info, cp, f->scale, f->scale, &x0, &y0, &x1, &y1);
        gw = x1 - x0;
        gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw <= 64 && gh <= 64) {
            stbtt_MakeCodepointBitmap(&f->info, glyph, gw, gh, gw, f->scale, f->scale, cp);
            for (gy = 0; gy < gh; ++gy)
                for (gx = 0; gx < gw; ++gx)
                    if (glyph[gy * gw + gx] >= 112) {
                        const int px = x + x0 + gx;
                        if (px >= clip_l && px < clip_r)
                            SDL_RenderPoint(ui->ren, (float)px, (float)(y + f->ascent + y0 + gy));
                    }
        }
        x += advance(f, cp, next);
    }
}

static void text(XrSetupUi *ui, const Font *f, int x, int y, const char *s, Rgb c)
{
    text_n(ui, f, x, y, s, strlen(s), c, -100000, 100000);
}

static int line_height(const Font *f)
{
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &gap);
    return (int)((ascent - descent) * f->scale + 0.5f);
}

/* DrawText's DT_WORDBREAK: the words wrapped at the rectangle's width, lines stacked from its top */
static void text_wrapped(XrSetupUi *ui, const Font *f, int l, int t, int r, int b, const char *s, Rgb c)
{
    const int width = r - l, step = line_height(f);
    int y = t;
    while (*s && y + step <= b + step / 2) {
        size_t fit = 0, k = 0, n = strlen(s);
        while (*s == ' ')
            s++, n--;
        /* the longest run of whole words that fits; a word longer than the line broken where it must */
        while (k < n) {
            size_t e = k;
            while (e < n && s[e] != ' ' && s[e] != '\n')
                e++;
            if (text_width_n(f, s, e) > width) {
                if (!fit) {
                    fit = e;
                    while (fit > 1 && text_width_n(f, s, fit) > width)
                        fit--;
                }
                break;
            }
            fit = e;
            if (e < n && s[e] == '\n')
                break;
            k = e + 1;
        }
        if (!fit)
            fit = n;
        text_n(ui, f, l, y, s, fit, c, l, r);
        s += fit;
        if (*s == '\n')
            s++;
        y += step;
    }
}

/* Marlett's "r" (the close box's cross) at 8 pt: two pixels thick, 8 x 7 */
static void close_cross(XrSetupUi *ui, int x, int y)
{
    static const char *rows[7] = {"##....##", ".##..##.", "..####..", "...##...", "..####..", ".##..##.", "##....##"};
    int r, i;
    colour(ui, K_TEXT);
    for (r = 0; r < 7; ++r)
        for (i = 0; i < 8; ++i)
            if (rows[r][i] == '#')
                SDL_RenderPoint(ui->ren, (float)(x + i), (float)(y + r));
}

/* DrawFocusRect: a dotted rectangle, every other pixel */
static void focus_rect(XrSetupUi *ui, int l, int t, int r, int b)
{
    int i;
    colour(ui, K_DARK);
    for (i = l; i < r; i += 2) {
        SDL_RenderPoint(ui->ren, (float)i, (float)t);
        SDL_RenderPoint(ui->ren, (float)i, (float)(b - 1));
    }
    for (i = t; i < b; i += 2) {
        SDL_RenderPoint(ui->ren, (float)l, (float)i);
        SDL_RenderPoint(ui->ren, (float)(r - 1), (float)i);
    }
}

static Ctl *find(XrSetupUi *ui, int id)
{
    int i;
    for (i = 0; i < ui->nctl; ++i)
        if (ui->ctl[i].id == id)
            return &ui->ctl[i];
    return NULL;
}

/* Owner-drawn push button: raised, sunken while pressed, the default one framed, a dotted focus rectangle. */
static void draw_button(XrSetupUi *ui, const Ctl *c, int focused)
{
    int l = c->x, t = c->y, r = c->x + c->w, b = c->y + c->h, tw, down;
    down = ui->pressed == c->id && ui->pressed_inside;
    fill(ui, l, t, r, b, K_FACE);
    if (c->id == ID_GO) {                  /* FrameRect black, then inside it */
        fill(ui, l, t, r, t + 1, K_DARK);
        fill(ui, l, b - 1, r, b, K_DARK);
        fill(ui, l, t, l + 1, b, K_DARK);
        fill(ui, r - 1, t, r, b, K_DARK);
        l++, t++, r--, b--;
    }
    bevel(ui, l, t, r, b, !down);
    tw = text_width(&ui->font, c->label);
    text(ui, &ui->font, l + (r - l - tw) / 2 + down, t + (b - t - line_height(&ui->font)) / 2 + down, c->label,
         c->enabled ? K_TEXT : K_SHADOW);
    if (focused)
        focus_rect(ui, l + 4, t + 4, r - 4, b - 4);
}

/* A sunken white field (WS_EX_CLIENTEDGE), the face colour when disabled; its text scrolled to keep the caret. */
static void draw_edit(XrSetupUi *ui, Ctl *c, int focused)
{
    const int l = c->x, t = c->y, r = c->x + c->w, b = c->y + c->h;
    const int il = l + 2 + 1, ir = r - 2 - 1;       /* the client edge, then the edit's own margin */
    const int ty = t + 2 + (b - t - 4 - line_height(&ui->font)) / 2;
    int caret_x;
    fill(ui, l, t, r, b, c->enabled ? K_WINDOW : K_FACE);
    bevel(ui, l, t, r, b, 0);
    caret_x = text_width_n(&ui->font, c->text, (size_t)c->caret);
    if (caret_x - c->scroll > ir - il - 1)
        c->scroll = caret_x - (ir - il - 1);
    if (caret_x - c->scroll < 0)
        c->scroll = caret_x;
    if (c->scroll < 0)
        c->scroll = 0;
    if (focused && c->sel_all && c->text[0]) {
        const int w = text_width(&ui->font, c->text);
        fill(ui, il - c->scroll < il ? il : il - c->scroll, ty, il + w - c->scroll > ir ? ir : il + w - c->scroll,
             ty + line_height(&ui->font), K_TITLE);
        text_n(ui, &ui->font, il - c->scroll, ty, c->text, strlen(c->text), K_LIGHT, il, ir);
    } else {
        text_n(ui, &ui->font, il - c->scroll, ty, c->text, strlen(c->text), c->enabled ? K_TEXT : K_SHADOW, il, ir);
    }
    if (focused && c->enabled && !c->sel_all && ((SDL_GetTicks() - ui->caret_blink) / 530) % 2 == 0) {
        const int x = il + caret_x - c->scroll;
        if (x >= il && x < ir)
            fill(ui, x, ty, x + 1, ty + line_height(&ui->font), K_TEXT);
    }
}

static void paint(XrSetupUi *ui)
{
    const int cw = UI_W, ch = UI_H + UI_ROW * (ui->lines - 1);
    int title_l = 3, title_t = 3, title_r = cw - 3, title_b = 3 + UI_TITLE_H;
    int inner_l, inner_w, slots, blocks, b, i;
    const int gap = 2, down = UI_ROW * (ui->lines - 1);
    SDL_SetRenderTarget(ui->ren, NULL);
    fill(ui, 0, 0, cw, ch, K_FACE);
    bevel(ui, 0, 0, cw, ch, 1);
    /* the title bar: solid navy, the icon left, white bold caption; a close box with Marlett's cross */
    fill(ui, title_l, title_t, title_r, title_b, K_TITLE);
    if (ui->icon) {
        SDL_FRect d = {(float)(title_l + 2), (float)(title_t + 1), 16.0f, 16.0f};
        SDL_RenderTexture(ui->ren, ui->icon, NULL, &d);
    }
    text(ui, &ui->bold, title_l + (ui->icon ? 21 : 4), title_t + 2, ui->title, K_TITLE_TEXT);
    {
        const int l = title_r - 18, t = title_t + 2, r = title_r - 2, bb = title_t + 16;
        const int pressed = ui->close_down && ui->close_inside;
        fill(ui, l, t, r, bb, K_FACE);
        bevel(ui, l, t, r, bb, !pressed);
        close_cross(ui, l + 4 + pressed, t + 3 + pressed);
    }
    /* the page: what to do, the disc image's line */
    text_wrapped(ui, &ui->font, 14, 30, cw - 14, 58, ui->intro, K_TEXT);
    text(ui, &ui->font, 14, 70, "Disc image or game folder:", K_TEXT);
    if (ui->has_extra)
        text(ui, &ui->font, 14, 70 + UI_ROW, ui->extra_label, K_TEXT);
    if (ui->file_path)
        text(ui, &ui->font, 14, 70 + UI_ROW * (ui->rows - 1), ui->file_label, K_TEXT);
    text(ui, &ui->font, 14, 70 + UI_ROW * ui->rows, "Target folder:", K_TEXT);
    /* an etched line above the progress area */
    line(ui, 10, 104 + down, cw - 10, 104 + down, K_SHADOW);
    line(ui, 10, 105 + down, cw - 10, 105 + down, K_LIGHT);
    /* the step's text and the progress bar: a sunken groove with navy blocks spread over its whole width */
    text_wrapped(ui, &ui->font, 14, 114 + down, cw - 14, 146 + down, ui->status, K_TEXT);
    {
        const int gl = 14, gt = 152 + down, gr = cw - 14, gb = 172 + down;
        fill(ui, gl, gt, gr, gb, K_FACE);
        bevel(ui, gl, gt, gr, gb, 0);
        inner_l = gl + 3;
        inner_w = gr - gl - 6;
        slots = (inner_w + gap) / (10 + gap);
        if (slots < 1)
            slots = 1;
        blocks = (slots * (ui->progress < 0 ? 0 : ui->progress > 100 ? 100 : ui->progress) + 50) / 100;
        for (b = 0; b < blocks; ++b)
            fill(ui, inner_l + b * (inner_w + gap) / slots, gt + 3, inner_l + (b + 1) * (inner_w + gap) / slots - gap,
                 gb - 3, K_PROGRESS);
    }
    for (i = 0; i < ui->nctl; ++i) {
        Ctl *c = &ui->ctl[i];
        if (c->button)
            draw_button(ui, c, ui->focus == i);
        else
            draw_edit(ui, c, ui->focus == i);
    }
    SDL_RenderPresent(ui->ren);
}

/* ── the controls ────────────────────────────────────────────────────── */

static Ctl *child(XrSetupUi *ui, int button, const char *label, int x, int y, int w, int h, int id)
{
    Ctl *c = &ui->ctl[ui->nctl++];
    memset(c, 0, sizeof *c);
    c->id = id;
    c->button = button;
    snprintf(c->label, sizeof c->label, "%s", label);
    c->x = x; c->y = y; c->w = w; c->h = h;
    c->enabled = 1;
    return c;
}

static void set_text(Ctl *c, const char *s)
{
    if (!c)
        return;
    snprintf(c->text, sizeof c->text, "%s", s ? s : "");
    c->caret = (int)strlen(c->text);
    c->scroll = 0;
    c->sel_all = 0;
}

static const char *get_text(XrSetupUi *ui, int id)
{
    Ctl *c = find(ui, id);
    return c ? c->text : "";
}

static void set_focus(XrSetupUi *ui, int index)
{
    if (index < 0 || index >= ui->nctl || !ui->ctl[index].enabled)
        return;
    ui->focus = index;
    ui->caret_blink = SDL_GetTicks();
    if (!ui->ctl[index].button) {
        ui->ctl[index].sel_all = 1;     /* a field reached by Tab selects its text, as a dialog's does */
        SDL_StartTextInput(ui->win);
    } else {
        SDL_StopTextInput(ui->win);
    }
}

static void tab(XrSetupUi *ui, int back)
{
    int i, k = ui->focus;
    for (i = 0; i < ui->nctl; ++i) {
        k = (k + (back ? ui->nctl - 1 : 1)) % ui->nctl;
        if (ui->ctl[k].enabled) {
            set_focus(ui, k);
            return;
        }
    }
}

static int exists(const char *path)
{
    SDL_PathInfo info;
    return path && *path && SDL_GetPathInfo(path, &info);
}

/* ── the dialogs ─────────────────────────────────────────────────────── */

static int no_dialogs_env(void)
{
    const char *e = getenv("RECOMP_NO_DIALOGS");
    return e && *e == '1';
}

static int box(XrSetupUi *ui, SDL_MessageBoxFlags flags, const char *msg, int yes_no)
{
    SDL_MessageBoxButtonData yn[2] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes"},
                                      {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No"}};
    SDL_MessageBoxButtonData ok[1] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,
                                       1, "OK"}};
    SDL_MessageBoxData d;
    int chosen = 0;
    char shown[2048];
    size_t i;
    /* the launcher's messages name its logs Windows' way (data\launcher.log): Linux's slashes here */
    snprintf(shown, sizeof shown, "%s", msg);
    for (i = 0; shown[i]; ++i)
        if (shown[i] == '\\')
            shown[i] = '/';
    memset(&d, 0, sizeof d);
    d.flags = flags;
    d.window = ui && ui->win ? ui->win : NULL;
    d.title = ui ? ui->title : "Setup";
    d.message = shown;
    d.numbuttons = yes_no ? 2 : 1;
    d.buttons = yes_no ? yn : ok;
    if (!SDL_ShowMessageBox(&d, &chosen)) {
        fprintf(stderr, "%s: %s\n", d.title, shown);
        return 0;
    }
    return chosen;
}

static void SDLCALL picked(void *user, const char *const *files, int filter)
{
    XrSetupUi *ui = (XrSetupUi *)user;
    (void)filter;
    if (files && files[0])
        snprintf(ui->dialog_path, sizeof ui->dialog_path, "%s", files[0]);
    else
        ui->dialog_path[0] = 0;
    ui->dialog_done = 1;
}

static void browse(XrSetupUi *ui, int id)
{
    static const SDL_DialogFileFilter discs[2] = {
        {"PS2 disc images (*.iso, *.chd), a game folder's disc.idx", "iso;chd;idx"}, {"All files (*.*)", "*"}};
    SDL_DialogFileFilter file[2];
    ui->dialog_open = 1;
    ui->dialog_done = 0;
    ui->dialog_path[0] = 0;
    ui->dialog_target = id == ID_BROWSE ? ID_ISO : id == ID_BROWSE2 ? ID_ISO2 : id == ID_BROWSE_FILE ? ID_FILE : ID_TARGET;
    if (id == ID_BROWSE_TARGET) {
        SDL_ShowOpenFolderDialog(picked, ui, ui->win, NULL, false);
    } else if (id == ID_BROWSE_FILE) {
        char pattern[60];
        snprintf(pattern, sizeof pattern, "%s", ui->file_filter);
        file[0].name = ui->file_filter_name;
        file[0].pattern = strncmp(ui->file_filter, "*.", 2) ? ui->file_filter : ui->file_filter + 2;
        file[1].name = "All files (*.*)";
        file[1].pattern = "*";
        SDL_ShowOpenFileDialog(picked, ui, ui->win, file, 2, NULL, false);
    } else {
        SDL_ShowOpenFileDialog(picked, ui, ui->win, discs, 2, NULL, false);
    }
}

/* ── events ──────────────────────────────────────────────────────────── */

static void command(XrSetupUi *ui, int id)
{
    switch (id) {
    case ID_BROWSE:
    case ID_BROWSE2:
    case ID_BROWSE_FILE:
    case ID_BROWSE_TARGET:
        browse(ui, id);
        break;
    case ID_GO:
        if (ui->in_form)
            ui->form_result = 1;
        break;
    case ID_QUIT:
        ui->closed = ui->closed;        /* as WM_CLOSE */
        if (!ui->in_form && !box(ui, SDL_MESSAGEBOX_INFORMATION, "The game is still being built. Stop and quit?", 1))
            break;
        ui->closed = 1;
        if (ui->in_form)
            ui->form_result = 0;
        break;
    }
}

static void close_window(XrSetupUi *ui) { command(ui, ID_QUIT); }

static int hit(const Ctl *c, int x, int y) { return x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h; }

static int on_close_box(int x, int y) { return x >= UI_W - 21 && x < UI_W - 5 && y >= 5 && y < 19; }

static SDL_HitTestResult SDLCALL hit_test(SDL_Window *w, const SDL_Point *pt, void *data)
{
    XrSetupUi *ui = (XrSetupUi *)data;
    const int x = pt->x / (ui->scale ? ui->scale : 1), y = pt->y / (ui->scale ? ui->scale : 1);
    (void)w;
    if (y >= 3 && y < 3 + UI_TITLE_H && x < UI_W - 22)       /* dragged by the title bar */
        return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

/* the byte offset in a field's text nearest to pixel x (from the field's text start) */
static int caret_at(XrSetupUi *ui, const Ctl *c, int x)
{
    const char *p = c->text;
    int best = 0, bestd = 1 << 30;
    for (;;) {
        const int off = (int)(p - c->text), w = text_width_n(&ui->font, c->text, (size_t)off);
        const int d = abs(w - x);
        if (d < bestd) {
            bestd = d;
            best = off;
        }
        if (!*p)
            break;
        next_cp(&p);
    }
    return best;
}

static int prev_offset(const char *s, int at)
{
    if (at <= 0)
        return 0;
    at--;
    while (at > 0 && ((unsigned char)s[at] & 0xC0) == 0x80)
        at--;
    return at;
}

static int next_offset(const char *s, int at)
{
    const char *p = s + at;
    if (!*p)
        return at;
    next_cp(&p);
    return (int)(p - s);
}

static void edit_insert(Ctl *c, const char *s)
{
    size_t n = strlen(s), len;
    if (c->sel_all) {
        c->text[0] = 0;
        c->caret = 0;
        c->sel_all = 0;
    }
    len = strlen(c->text);
    if (len + n >= sizeof c->text)
        return;
    memmove(c->text + c->caret + n, c->text + c->caret, len - (size_t)c->caret + 1);
    memcpy(c->text + c->caret, s, n);
    c->caret += (int)n;
}

static void key(XrSetupUi *ui, const SDL_KeyboardEvent *k)
{
    Ctl *c = ui->focus >= 0 && ui->focus < ui->nctl ? &ui->ctl[ui->focus] : NULL;
    const int ctrl = (k->mod & SDL_KMOD_CTRL) != 0;
    ui->caret_blink = SDL_GetTicks();
    if (k->key == SDLK_TAB) {
        tab(ui, (k->mod & SDL_KMOD_SHIFT) != 0);
        return;
    }
    if (k->key == SDLK_RETURN || k->key == SDLK_KP_ENTER) {   /* the focused button, else the default one (Go) */
        Ctl *go = find(ui, ID_GO);
        if (c && c->button && c->enabled)
            command(ui, c->id);
        else if (go && go->enabled)
            command(ui, ID_GO);
        return;
    }
    if (!c)
        return;
    if (c->button) {
        if (k->key == SDLK_SPACE && c->enabled)
            command(ui, c->id);
        return;
    }
    if (!c->enabled)
        return;
    if (ctrl && k->key == SDLK_A) {
        c->sel_all = 1;
        return;
    }
    if (ctrl && (k->key == SDLK_C || k->key == SDLK_X)) {
        if (c->sel_all) {
            SDL_SetClipboardText(c->text);
            if (k->key == SDLK_X) {
                c->text[0] = 0;
                c->caret = 0;
                c->sel_all = 0;
            }
        }
        return;
    }
    if (ctrl && k->key == SDLK_V) {
        char *clip = SDL_GetClipboardText();
        if (clip) {
            char *nl = strpbrk(clip, "\r\n");
            if (nl)
                *nl = 0;
            edit_insert(c, clip);
            SDL_free(clip);
        }
        return;
    }
    switch (k->key) {
    case SDLK_BACKSPACE:
        if (c->sel_all) { c->text[0] = 0; c->caret = 0; c->sel_all = 0; break; }
        if (c->caret > 0) {
            const int p = prev_offset(c->text, c->caret);
            memmove(c->text + p, c->text + c->caret, strlen(c->text + c->caret) + 1);
            c->caret = p;
        }
        break;
    case SDLK_DELETE:
        if (c->sel_all) { c->text[0] = 0; c->caret = 0; c->sel_all = 0; break; }
        if (c->text[c->caret]) {
            const int n = next_offset(c->text, c->caret);
            memmove(c->text + c->caret, c->text + n, strlen(c->text + n) + 1);
        }
        break;
    case SDLK_LEFT:
        c->caret = c->sel_all ? 0 : prev_offset(c->text, c->caret);
        c->sel_all = 0;
        break;
    case SDLK_RIGHT:
        c->caret = c->sel_all ? (int)strlen(c->text) : next_offset(c->text, c->caret);
        c->sel_all = 0;
        break;
    case SDLK_HOME:
        c->caret = 0;
        c->sel_all = 0;
        break;
    case SDLK_END:
        c->caret = (int)strlen(c->text);
        c->sel_all = 0;
        break;
    default:
        break;
    }
}

static void pump(XrSetupUi *ui)
{
    SDL_Event e;
    int dirty = 0;
    while (SDL_PollEvent(&e)) {
        const float s = (float)(ui->scale ? ui->scale : 1);
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            close_window(ui);
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
            dirty = 1;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button == SDL_BUTTON_LEFT && !ui->dialog_open) {
                const int x = (int)(e.button.x / s), y = (int)(e.button.y / s);
                int i;
                if (on_close_box(x, y)) {
                    ui->close_down = 1;
                    ui->close_inside = 1;
                    SDL_CaptureMouse(true);
                }
                for (i = 0; i < ui->nctl; ++i) {
                    Ctl *c = &ui->ctl[i];
                    if (!c->enabled || !hit(c, x, y))
                        continue;
                    set_focus(ui, i);
                    if (c->button) {
                        ui->pressed = c->id;
                        ui->pressed_inside = 1;
                        SDL_CaptureMouse(true);
                    } else {
                        c->sel_all = 0;
                        c->caret = caret_at(ui, c, x - (c->x + 3) + c->scroll);
                    }
                }
                dirty = 1;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (ui->close_down || ui->pressed) {
                const int x = (int)(e.motion.x / s), y = (int)(e.motion.y / s);
                Ctl *c = ui->pressed ? find(ui, ui->pressed) : NULL;
                ui->close_inside = on_close_box(x, y);
                ui->pressed_inside = c && hit(c, x, y);
                dirty = 1;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT) {
                const int x = (int)(e.button.x / s), y = (int)(e.button.y / s);
                SDL_CaptureMouse(false);
                if (ui->close_down) {
                    ui->close_down = 0;
                    if (on_close_box(x, y))
                        close_window(ui);
                }
                if (ui->pressed) {
                    Ctl *c = find(ui, ui->pressed);
                    const int id = ui->pressed;
                    ui->pressed = 0;
                    if (c && hit(c, x, y))
                        command(ui, id);
                }
                dirty = 1;
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (!ui->dialog_open)
                key(ui, &e.key);
            dirty = 1;
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (!ui->dialog_open && ui->focus >= 0 && ui->focus < ui->nctl && !ui->ctl[ui->focus].button &&
                ui->ctl[ui->focus].enabled)
                edit_insert(&ui->ctl[ui->focus], e.text.text);
            dirty = 1;
            break;
        default:
            break;
        }
    }
    if (ui->dialog_open && ui->dialog_done) {
        ui->dialog_open = 0;
        if (ui->dialog_path[0])
            set_text(find(ui, ui->dialog_target), ui->dialog_path);
        dirty = 1;
    }
    (void)dirty;
    if (ui->win)
        paint(ui);                      /* (cheap: the caret blinks, the page is a few hundred rectangles) */
}

/* ── the window ──────────────────────────────────────────────────────── */

/* The window, made once (the first page or the first progress call). */
static int ui_show(XrSetupUi *ui)
{
    int height, s;
    float display_scale;
    SDL_DisplayID display;
    if (ui->win || ui->failed)
        return ui->win != NULL;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        ui->failed = 1;                 /* headless: build without a window */
        return 0;
    }
    ui->sdl_up = 1;
    ui->fonts_ok = font_init(&ui->font, k_font_regular) && font_init(&ui->bold, k_font_bold);
    if (!ui->fonts_ok) {
        ui->failed = 1;
        return 0;
    }
    ui->rows = 1 + (ui->has_extra ? 1 : 0) + (ui->file_path ? 1 : 0);
    ui->lines = ui->rows + 1;
    height = UI_H + UI_ROW * (ui->lines - 1);
    display = SDL_GetPrimaryDisplay();
    display_scale = display ? SDL_GetDisplayContentScale(display) : 1.0f;
    s = (int)(display_scale + 0.25f);   /* a whole multiple: 1x up to 1.74, 2x from 1.75 */
    if (s < 1)
        s = 1;
    ui->scale = s;
    if (!SDL_CreateWindowAndRenderer(ui->title, UI_W * s, height * s, SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN,
                                     &ui->win, &ui->ren)) {
        ui->failed = 1;
        return 0;
    }
    SDL_SetRenderScale(ui->ren, (float)s, (float)s);
    SDL_SetRenderVSync(ui->ren, 1);
    SDL_SetWindowHitTest(ui->win, hit_test, ui);
    SDL_SetWindowPosition(ui->win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (ui->icon_bmp && ui->icon_size) {
        SDL_Surface *bmp = SDL_LoadBMP_IO(SDL_IOFromConstMem(ui->icon_bmp, ui->icon_size), true);
        if (bmp) {
            SDL_SetWindowIcon(ui->win, bmp);
            ui->icon = SDL_CreateTextureFromSurface(ui->ren, bmp);
            if (ui->icon)
                SDL_SetTextureScaleMode(ui->icon, SDL_SCALEMODE_NEAREST);
            SDL_DestroySurface(bmp);
        }
    }
    /* sunken white fields, Browse..., Go, Quit - the Windows window's places */
    child(ui, 0, "", 170, 67, 250, 21, ID_ISO);
    child(ui, 1, "Browse...", 428, 67, 76, 22, ID_BROWSE);
    if (ui->has_extra) {
        child(ui, 0, "", 170, 67 + UI_ROW, 250, 21, ID_ISO2);
        child(ui, 1, "Browse...", 428, 67 + UI_ROW, 76, 22, ID_BROWSE2);
    }
    if (ui->file_path) {
        const int y = 67 + UI_ROW * (ui->rows - 1);
        child(ui, 0, "", 170, y, 250, 21, ID_FILE);
        child(ui, 1, "Browse...", 428, y, 76, 22, ID_BROWSE_FILE);
    }
    child(ui, 0, "", 170, 67 + UI_ROW * ui->rows, 250, 21, ID_TARGET);
    child(ui, 1, "Browse...", 428, 67 + UI_ROW * ui->rows, 76, 22, ID_BROWSE_TARGET);
    {
        const int bottom = height - 46;
        child(ui, 1, "Go", UI_W - 184, bottom, 80, 24, ID_GO);
        child(ui, 1, "Quit", UI_W - 96, bottom, 80, 24, ID_QUIT);
    }
    ui->focus = -1;
    SDL_ShowWindow(ui->win);
    SDL_RaiseWindow(ui->win);
    return 1;
}

static void enable_page(XrSetupUi *ui, int on)
{
    int i;
    for (i = 0; i < ui->nctl; ++i)
        if (ui->ctl[i].id != ID_QUIT)
            ui->ctl[i].enabled = on;
    if (!on && ui->focus >= 0 && ui->focus < ui->nctl && ui->ctl[ui->focus].id != ID_QUIT) {
        ui->focus = -1;
        SDL_StopTextInput(ui->win);
    }
}

static int index_of(XrSetupUi *ui, int id)
{
    int i;
    for (i = 0; i < ui->nctl; ++i)
        if (ui->ctl[i].id == id)
            return i;
    return -1;
}

int xr_setup_ui_form(XrSetupUi *ui, const char *disc_name, char *iso, size_t n, const char *note,
                     const char *extra_label, char *extra, size_t extra_n, char *target, size_t target_n, int *copy)
{
    char path[FIELD_MAX], path2[FIELD_MAX], folder[FIELD_MAX];
    if (!ui || xr_no_dialogs())
        return -1;
    if (!ui->win && extra_label && *extra_label && extra) {     /* the second line: before the window is made */
        snprintf(ui->extra_label, sizeof ui->extra_label, "%s", extra_label);
        ui->has_extra = 1;
    }
    if (!ui_show(ui))
        return -1;
    snprintf(ui->intro, sizeof ui->intro, "%s%sChoose your %s disc image (or a game folder you kept), then Go. The game "
             "is built from it once and the disc unpacked into the game folder (the image is only read).",
             note ? note : "", note && *note ? " " : "", disc_name ? disc_name : "");
    ui->status[0] = 0;
    ui->progress = 0;
    set_text(find(ui, ID_ISO), iso);
    if (find(ui, ID_ISO2) && extra)
        set_text(find(ui, ID_ISO2), extra);
    if (find(ui, ID_FILE))
        set_text(find(ui, ID_FILE), ui->file_path);
    set_text(find(ui, ID_TARGET), target);
    enable_page(ui, 1);
    set_focus(ui, index_of(ui, ID_GO));
    ui->in_form = 1;
    ui->form_result = -1;
    for (;;) {
        pump(ui);
        if (ui->form_result == 1) {
            snprintf(path, sizeof path, "%s", get_text(ui, ID_ISO));
            if (!path[0] || !exists(path)) {
                box(ui, SDL_MESSAGEBOX_ERROR, "Please choose your disc image (.iso, .chd) or a game folder with Browse...",
                    0);
                ui->form_result = -1;
                continue;
            }
            snprintf(folder, sizeof folder, "%s", get_text(ui, ID_TARGET));
            if (!folder[0]) {
                box(ui, SDL_MESSAGEBOX_ERROR, "Please choose the target folder with Browse...", 0);
                ui->form_result = -1;
                continue;
            }
            path2[0] = 0;
            if (find(ui, ID_ISO2))
                snprintf(path2, sizeof path2, "%s", get_text(ui, ID_ISO2));
            if (path2[0] && !exists(path2)) {    /* optional, but real */
                box(ui, SDL_MESSAGEBOX_ERROR, "The second disc image cannot be found. Leave its line empty if you do not "
                    "have that disc.", 0);
                ui->form_result = -1;
                continue;
            }
            if (find(ui, ID_FILE)) {
                const char *f = get_text(ui, ID_FILE);
                if (f[0] && !exists(f)) {
                    box(ui, SDL_MESSAGEBOX_ERROR, "That file cannot be found. Leave its line empty if you do not have it.",
                        0);
                    ui->form_result = -1;
                    continue;
                }
                snprintf(ui->file_path, ui->file_n, "%s", f);
            }
            break;
        }
        if (ui->form_result == 0)
            break;
        SDL_Delay(15);
    }
    ui->in_form = 0;
    if (ui->form_result != 1)
        return 0;
    snprintf(iso, n, "%s", path);
    if (extra && extra_n)
        snprintf(extra, extra_n, "%s", find(ui, ID_ISO2) ? path2 : "");
    if (target && target_n)
        snprintf(target, target_n, "%s", folder);
    if (copy)
        *copy = 1;                      /* (the discs are always unpacked into game/) */
    enable_page(ui, 0);
    snprintf(ui->intro, sizeof ui->intro, "The game is being built from your %s disc - this happens only once.",
             disc_name ? disc_name : "");
    pump(ui);
    return 1;
}

void xr_setup_ui_progress(void *p, double fraction, const char *msg)
{
    XrSetupUi *ui = (XrSetupUi *)p;
    Uint64 now;
    if (!ui || !ui_show(ui))
        return;
    if (!ui->intro[0])
        snprintf(ui->intro, sizeof ui->intro, "The game is being built from your disc - this happens only once.");
    enable_page(ui, 0);
    now = SDL_GetTicks();
    ui->progress = (int)((fraction < 0 ? 0 : fraction > 1 ? 1 : fraction) * 100.0 + 0.5);
    snprintf(ui->status, sizeof ui->status, "%s", msg ? msg : "");
    if (fraction < 1.0 && now - ui->last_draw < 50) {
        SDL_PumpEvents();               /* the window stays alive between redraws */
        return;                         /* 20 redraws a second are plenty */
    }
    ui->last_draw = now;
    pump(ui);
}

void xr_setup_ui_file_line(XrSetupUi *ui, const char *label, const char *filter_name, const char *filter, char *path,
                           size_t n)
{
    if (!ui || ui->win || !label || !*label || !path || !n)
        return;                         /* (before the window is made) */
    snprintf(ui->file_label, sizeof ui->file_label, "%s", label);
    snprintf(ui->file_filter_name, sizeof ui->file_filter_name, "%s",
             filter_name && *filter_name ? filter_name : "Files");
    snprintf(ui->file_filter, sizeof ui->file_filter, "%s", filter && *filter ? filter : "*.*");
    ui->file_path = path;
    ui->file_n = n;
}

int xr_setup_ui_cancelled(void *p)
{
    XrSetupUi *ui = (XrSetupUi *)p;
    if (ui && ui->win && !ui->in_form)
        pump(ui);                       /* the close box answered while a step runs */
    return ui && ui->closed;
}

int xr_no_dialogs(void) { return no_dialogs_env(); }

void xr_setup_ui_message(XrSetupUi *ui, int error, const char *msg)
{
    if (xr_no_dialogs())
        return;
    box(ui, error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_WARNING, msg ? msg : "", 0);
}

void xr_setup_ui_close(XrSetupUi *ui)
{
    if (!ui)
        return;
    if (ui->icon)
        SDL_DestroyTexture(ui->icon);
    if (ui->ren)
        SDL_DestroyRenderer(ui->ren);
    if (ui->win)
        SDL_DestroyWindow(ui->win);
    free(ui);
}
