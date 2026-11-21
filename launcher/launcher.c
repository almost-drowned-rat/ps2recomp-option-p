/* launcher.c -- the player's one exe (see launcher.h). */

#include "launcher.h"

#include "game_setup_ui.h"
#include "sha256.h"
#include "zip_read.h"

#include "libchdr/cdrom.h"
#include "libchdr/chd.h"

#include <SDL3/SDL.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#define LP 1024
#define MAX_ARGS 256
#define TRAILER_MAGIC "XRPAYLD1"            /* the Xbox launcher's trailer: one release script for both */
#define TRAILER_SIZE 48                     /* magic (8), payload offset (8, LE), release (32, NUL padded) */
#define WRONG_DISC 3                        /* player_build.py's exit code: not the game's disc */
#define PACKAGE_INCOMPLETE "The package is incomplete - please download it again."

typedef struct {
    const PsLauncherConfig *cfg;
    /* root = the exe's folder (writable: the game, its settings and saves live here too); tools = data/tools,
     * the payload unpacked; work = data/build/work, the translated sources; obj = data/build/obj */
    char root[LP], tools[LP], data[LP], build[LP], work[LP], obj[LP];
    char exe[LP];                           /* this exe (the payload trailer) */
    char out[LP];                           /* the game: root/game_exe ({OUT}) */
    char iso[LP];                           /* the player's disc image */
    char lift_iso[LP];                      /* {ISO}: the image as the build's tools read it - a CHD's .iso copy */
    char release[33];
    char clang[LP], python[LP];             /* toolchain.txt: absolute */
    char jobs[16];                          /* {JOBS} */
    XrSetupUi *ui;
    SDL_Mutex *log_mx;
    SDL_IOStream *log;
    double base, span;
    char step[128];
} L;

/* ── small helpers ─────────────────────────────────────────────────────── */

static void lg(L *l, const char *fmt, ...)
{
    char line[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[LAUNCHER] %s\n", line);
    if (l->log_mx)
        SDL_LockMutex(l->log_mx);
    if (l->log) {
        SDL_WriteIO(l->log, line, strlen(line));
        SDL_WriteIO(l->log, "\n", 1);
        SDL_FlushIO(l->log);
    }
    if (l->log_mx)
        SDL_UnlockMutex(l->log_mx);
}

static void join(char *out, size_t n, const char *a, const char *b)
{
    size_t k = strlen(a);
    snprintf(out, n, "%s%s%s", a, k && (a[k - 1] == '/' || a[k - 1] == '\\') ? "" : "/", b);
}

static int is_file(const char *p)
{
    SDL_PathInfo pi;
    return SDL_GetPathInfo(p, &pi) && pi.type == SDL_PATHTYPE_FILE;
}

static Sint64 file_size(const char *p)
{
    SDL_PathInfo pi;
    return SDL_GetPathInfo(p, &pi) && pi.type == SDL_PATHTYPE_FILE ? (Sint64)pi.size : -1;
}

static char *read_text(const char *p)
{
    return (char *)SDL_LoadFile(p, NULL);   /* SDL_LoadFile adds a NUL */
}

static int write_text(const char *p, const char *text)
{
    return SDL_SaveFile(p, text, strlen(text));
}

typedef struct { int ok; } RmCtx;
static int rm_tree(const char *p);
static SDL_EnumerationResult SDLCALL rm_cb(void *u, const char *dir, const char *name)
{
    char p[LP];
    join(p, sizeof p, dir, name);
    if (!rm_tree(p))
        ((RmCtx *)u)->ok = 0;
    return SDL_ENUM_CONTINUE;
}

/* a disc image by its name: the player's, never removed (Orta GitHub #6, 2026-11-17: an installer deleted a player's
 * only image that lay inside its folder - the rule for every launcher: the image is read only, wherever it lies) */
static int is_disc_image(const char *p, Sint64 size)
{
    /* image formats only - not .zip/.gz/.7z: the launcher's own tool bundles are archives and go with a new release;
     * .bin/.img only from 32 MB on: the build writes small .bin files of its own (stage overlays, VU data) */
    static const char *const ext[] = { ".iso", ".cue", ".chd", ".mdf", ".mds", ".nrg", ".cso", ".zso", ".ecm" };
    static const char *const big[] = { ".bin", ".img" };
    size_t n = strlen(p), i, k;
    for (i = 0; i < sizeof ext / sizeof *ext; i++) {
        k = strlen(ext[i]);
        if (n > k && !SDL_strcasecmp(p + n - k, ext[i]))
            return 1;
    }
    for (i = 0; i < sizeof big / sizeof *big; i++) {
        k = strlen(big[i]);
        if (n > k && !SDL_strcasecmp(p + n - k, big[i]) && size >= ((Sint64)32 << 20))
            return 1;
    }
    return 0;
}

/* removes p and everything below it - only ever the launcher's own folders (data/build, data/tools/bundle/unpack);
 * a disc image found there stays (and so does its folder) */
static int rm_tree(const char *p)
{
    SDL_PathInfo pi;
    RmCtx c = { 1 };
    if (!SDL_GetPathInfo(p, &pi))
        return 1;
    if (pi.type == SDL_PATHTYPE_FILE && is_disc_image(p, (Sint64)pi.size)) {
        SDL_Log("kept %s: a disc image is never removed", p);
        return 0;
    }
    if (pi.type == SDL_PATHTYPE_DIRECTORY)
        SDL_EnumerateDirectory(p, rm_cb, &c);
    return SDL_RemovePath(p) && c.ok;
}

/* "{TOOLS}/x" with the placeholders filled in */
static void expand(L *l, const char *in, char *out, size_t n)
{
    size_t o = 0;
    while (*in && o + 1 < n) {
        const char *v = NULL;
        size_t skip = 0;
        int dir = 0;                        /* a folder: its trailing separator dropped */
        if (!strncmp(in, "{TOOLS}", 7)) v = l->tools, skip = 7, dir = 1;
        else if (!strncmp(in, "{ROOT}", 6)) v = l->root, skip = 6, dir = 1;
        else if (!strncmp(in, "{WORK}", 6)) v = l->work, skip = 6, dir = 1;
        else if (!strncmp(in, "{OUT}", 5)) v = l->out, skip = 5;
        else if (!strncmp(in, "{ISO}", 5)) v = l->lift_iso[0] ? l->lift_iso : l->iso, skip = 5;
        else if (!strncmp(in, "{JOBS}", 6)) v = l->jobs, skip = 6;
        if (v) {
            size_t k = strlen(v);
            if (dir && k && (v[k - 1] == '/' || v[k - 1] == '\\'))
                k--;
            if (o + k >= n)
                break;
            memcpy(out + o, v, k);
            o += k;
            in += skip;
        } else {
            out[o++] = *in++;
        }
    }
    out[o] = 0;
}

static int split_tabs(char *line, char **argv, int max)
{
    int n = 0;
    char *s = line;
    while (n < max - 1) {
        char *t = strchr(s, '\t');
        argv[n++] = s;
        if (!t)
            break;
        *t = 0;
        s = t + 1;
    }
    argv[n] = NULL;
    return n;
}

/* runs argv in cwd, stdout + stderr appended to log_path (NULL: dropped); the exit code, -1 when it could not
 * start */
static int run_proc(L *l, char **argv, const char *cwd, const char *log_path)
{
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_IOStream *out = log_path ? SDL_IOFromFile(log_path, "ab") : NULL;
    SDL_Process *proc;
    int code = -1;
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv);
    if (cwd)
        SDL_SetStringProperty(p, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, cwd);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    if (out) {
        SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
        SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, out);
        SDL_SetBooleanProperty(p, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    } else {
        SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    }
#ifdef _WIN32
    SDL_SetBooleanProperty(p, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);   /* (no console window) */
#endif
    /* (not in the background elsewhere: SDL gives a background process's exit code as 0 - a failed compile and the
       wrong disc's code 3 would pass unseen; on Windows the background flag stays for "no console window" and the
       code is read from our own handle below) */
    proc = SDL_CreateProcessWithProperties(p);
    SDL_DestroyProperties(p);
    if (!proc) {
        lg(l, "cannot start %s: %s", argv[0], SDL_GetError());
    } else {
#ifdef _WIN32
        /* SDL gives a background process's exit code as 0 on Windows too (SDL_process.c SDL_WaitProcess: found
           2026-11-21 by a forced failure - a crashed translation passed as "done"). The real code from our own handle:
           the pid stays valid while SDL keeps its handle, until SDL_DestroyProcess. */
        HANDLE own = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                 (DWORD)SDL_GetNumberProperty(SDL_GetProcessProperties(proc), SDL_PROP_PROCESS_PID_NUMBER, 0));
        SDL_WaitProcess(proc, true, &code);
        if (own) {
            DWORD rc;
            if (WaitForSingleObject(own, INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(own, &rc))
                code = (int)rc;
            else
                code = -1;
            CloseHandle(own);
        } else {
            lg(l, "%s: no handle for its exit code (error %lu)", argv[0], (unsigned long)GetLastError());
            code = -1;
        }
#else
        SDL_WaitProcess(proc, true, &code);
#endif
        SDL_DestroyProcess(proc);
    }
    if (out)
        SDL_CloseIO(out);
    return code;
}

/* ── a step on a worker thread, the window kept alive meanwhile ────────── */

typedef struct {
    int (*fn)(L *, void *);
    L *l;
    void *arg;
    SDL_AtomicInt done;
    int rc;
} Job;

static int SDLCALL job_main(void *p)
{
    Job *j = (Job *)p;
    j->rc = j->fn(j->l, j->arg);
    SDL_SetAtomicInt(&j->done, 1);
    return 0;
}

static void ui_say(L *l, double within, const char *extra)
{
    char t[256];
    snprintf(t, sizeof t, "%s%s%s", l->step, extra && *extra ? " - " : "", extra ? extra : "");
    if (l->ui)
        xr_setup_ui_progress(l->ui, l->base + l->span * (within < 0 ? 0 : within > 1 ? 1 : within), t);
}

typedef struct { SDL_AtomicInt done_n; int total; } Prog;

static double prog_frac(void *a)
{
    Prog *p = (Prog *)a;
    return p->total ? (double)SDL_GetAtomicInt(&p->done_n) / p->total : 0.0;
}

/* runs fn on a thread; its code (0 = done). progress: 0..1 within the step, or NULL */
static int run_job(L *l, const char *step, double base, double span, int (*fn)(L *, void *), void *arg,
                   double (*progress)(void *))
{
    Job j;
    SDL_Thread *t;
    Uint64 t0 = SDL_GetTicks();
    memset(&j, 0, sizeof j);
    j.fn = fn; j.l = l; j.arg = arg;
    snprintf(l->step, sizeof l->step, "%s", step);
    l->base = base;
    l->span = span;
    lg(l, "%s", step);
    t = SDL_CreateThread(job_main, "launcher step", &j);
    if (!t)
        return fn(l, arg);
    while (!SDL_GetAtomicInt(&j.done)) {
        char e[64];
        unsigned s = (unsigned)((SDL_GetTicks() - t0) / 1000);
        snprintf(e, sizeof e, "%u:%02u", s / 60, s % 60);
        ui_say(l, progress ? progress(arg) : 0.0, e);
        SDL_Delay(100);
    }
    SDL_WaitThread(t, NULL);
    lg(l, "%s: %s (code %d) after %u s", step, j.rc ? "FAILED" : "done", j.rc,
       (unsigned)((SDL_GetTicks() - t0) / 1000));
    return j.rc;
}

/* ── the disc image ────────────────────────────────────────────────────── */

/* pc_settings.ini (the game's own settings file, key=value lines): a key's value - "disc", the image the game last
 * played; a second disc's key (PsLauncherConfig.extra_disc_key) */
static int settings_get(L *l, const char *key, char *out, size_t n)
{
    char p[LP], *t, *line, *save = NULL;
    const size_t k = strlen(key);
    int found = 0;
    join(p, sizeof p, l->root, "pc_settings.ini");
    if (!(t = read_text(p)))
        return 0;
    for (line = SDL_strtok_r(t, "\r\n", &save); line; line = SDL_strtok_r(NULL, "\r\n", &save))
        if (!strncmp(line, key, k) && line[k] == '=' && line[k + 1]) {
            snprintf(out, n, "%s", line + k + 1);
            found = 1;
        }
    SDL_free(t);
    return found;
}

static int settings_disc(L *l, char *out, size_t n) { return settings_get(l, "disc", out, n); }

/* a key set in the game's settings (an empty value: the key removed) - only an ASCII value: the game reads the file
 * in the system's code page (a path that is not: the game asks for it itself) */
static void settings_set(L *l, const char *key, const char *value)
{
    char p[LP], *t, *line, *save = NULL, *outbuf;
    const unsigned char *c;
    const size_t k = strlen(key);
    size_t cap;
    for (c = (const unsigned char *)value; *c; c++)
        if (*c >= 0x80) {
            lg(l, "%s: the path is not plain ASCII: the game asks for it itself", key);
            return;
        }
    join(p, sizeof p, l->root, "pc_settings.ini");
    t = read_text(p);
    cap = (t ? strlen(t) : 0) + k + strlen(value) + 16;
    outbuf = (char *)calloc(1, cap);
    if (!outbuf) {
        SDL_free(t);
        return;
    }
    if (t)
        for (line = SDL_strtok_r(t, "\r\n", &save); line; line = SDL_strtok_r(NULL, "\r\n", &save))
            if (strncmp(line, key, k) || line[k] != '=') {
                strcat(outbuf, line);
                strcat(outbuf, "\n");
            }
    if (*value) {
        strcat(outbuf, key);
        strcat(outbuf, "=");
        strcat(outbuf, value);
        strcat(outbuf, "\n");
    }
    if (!write_text(p, outbuf))
        lg(l, "cannot write %s", p);
    free(outbuf);
    SDL_free(t);
}

/* the game's settings get the image (so the game does not ask for it again) */
static void settings_set_disc(L *l) { settings_set(l, "disc", l->iso); }

/* ── a disc as its sectors: an .iso, a CHD, or an unpacked disc's folder (disc.idx: the game's Disc::Unpack) ── */

typedef struct { Uint32 lsn, count; Uint64 bytes; char path[LP]; } IdxRun;

typedef struct {
    int kind;                               /* 0 .iso, 1 CHD, 2 unpacked folder */
    SDL_IOStream *f;
    chd_file *chd;
    unsigned char *hunk;
    Uint32 hunk_bytes, unit_bytes, units_per_hunk, hunk_no, data_at;
    IdxRun *runs;
    int nruns, rf_i;
    SDL_IOStream *rf;
    Uint64 sectors;
} Src;

/* an unpacked disc's folder: disc.idx in it */
static int is_disc_dir(const char *p)
{
    char i[LP];
    join(i, sizeof i, p, "disc.idx");
    return is_file(i);
}

static int run_cmp(const void *a, const void *b)
{
    const IdxRun *x = (const IdxRun *)a, *y = (const IdxRun *)b;
    return x->lsn < y->lsn ? -1 : x->lsn > y->lsn;
}

static void src_close(Src *s)
{
    if (s->f) SDL_CloseIO(s->f);
    if (s->rf) SDL_CloseIO(s->rf);
    if (s->chd) chd_close(s->chd);
    SDL_free(s->hunk);
    SDL_free(s->runs);
    memset(s, 0, sizeof *s);
}

static int src_open(Src *s, const char *p)
{
    size_t k = strlen(p);
    memset(s, 0, sizeof *s);
    s->rf_i = -1;
    s->hunk_no = ~0u;
    if (is_disc_dir(p)) {
        char ip[LP], *t, *line, *save = NULL;
        int cap = 0;
        join(ip, sizeof ip, p, "disc.idx");
        if (!(t = read_text(ip)))
            return 0;
        s->kind = 2;
        for (line = SDL_strtok_r(t, "\r\n", &save); line; line = SDL_strtok_r(NULL, "\r\n", &save)) {
            if (!strncmp(line, "sectors ", 8)) {
                s->sectors = SDL_strtoull(line + 8, NULL, 10);
            } else if (!strncmp(line, "run ", 4)) {
                char *e;
                IdxRun r;
                r.lsn = (Uint32)SDL_strtoul(line + 4, &e, 10);
                r.count = (Uint32)SDL_strtoul(e, &e, 10);
                r.bytes = SDL_strtoull(e, &e, 10);
                if (*e != ' ')
                    continue;
                join(r.path, sizeof r.path, p, e + 1);
                if (s->nruns == cap) {
                    cap = cap ? cap * 2 : 64;
                    s->runs = (IdxRun *)SDL_realloc(s->runs, (size_t)cap * sizeof *s->runs);
                    if (!s->runs) { SDL_free(t); return 0; }
                }
                s->runs[s->nruns++] = r;
            }
        }
        SDL_free(t);
        if (s->nruns)
            SDL_qsort(s->runs, (size_t)s->nruns, sizeof *s->runs, run_cmp);
        return s->sectors > 0;
    }
    if (k > 4 && !SDL_strcasecmp(p + k - 4, ".chd")) {
        const chd_header *h;
        if (chd_open(p, CHD_OPEN_READ, NULL, &s->chd) != CHDERR_NONE || !s->chd)
            return 0;
        h = chd_get_header(s->chd);
        if (!h || !h->hunkbytes || !h->unitbytes || h->hunkbytes % h->unitbytes ||
            (h->unitbytes != 2048 && h->unitbytes != CD_FRAME_SIZE)) {
            src_close(s);
            return 0;
        }
        s->kind = 1;
        s->hunk_bytes = h->hunkbytes;
        s->unit_bytes = h->unitbytes;
        s->units_per_hunk = h->hunkbytes / h->unitbytes;
        s->sectors = h->logicalbytes / h->unitbytes;
        s->hunk = (unsigned char *)SDL_malloc(h->hunkbytes);
        return s->hunk != NULL;
    }
    if (!(s->f = SDL_IOFromFile(p, "rb")))
        return 0;
    s->sectors = (Uint64)SDL_GetIOSize(s->f) / 2048;
    return 1;
}

/* n sectors' 2048 data bytes from lsn (past the end, or in no run of a folder: zeros); 0 on a read error */
static int src_read(Src *s, Uint64 lsn, Uint32 n, unsigned char *out)
{
    Uint32 i;
    if (s->kind == 0) {
        size_t want = (size_t)n * 2048, got;
        if (SDL_SeekIO(s->f, (Sint64)(lsn * 2048), SDL_IO_SEEK_SET) < 0)
            return 0;
        got = SDL_ReadIO(s->f, out, want);
        if (got < want)
            memset(out + got, 0, want - got);
        return 1;
    }
    for (i = 0; i < n; i++) {
        unsigned char *dst = out + (size_t)i * 2048;
        const Uint64 at = lsn + i;
        if (at >= s->sectors) {
            memset(dst, 0, 2048);
            continue;
        }
        if (s->kind == 1) {
            const Uint32 hunk = (Uint32)(at / s->units_per_hunk), u = (Uint32)(at % s->units_per_hunk);
            const unsigned char *unit;
            if (hunk != s->hunk_no) {
                if (chd_read(s->chd, hunk, s->hunk) != CHDERR_NONE)
                    return 0;
                s->hunk_no = hunk;
            }
            unit = s->hunk + (size_t)u * s->unit_bytes;
            if (s->unit_bytes == CD_FRAME_SIZE) {      /* a CD's raw sector (+ subcode): its 2048 data bytes */
                if (!s->data_at)
                    s->data_at = unit[15] == 2 ? 24 : 16;
                unit += s->data_at;
            }
            memcpy(dst, unit, 2048);
        } else {
            int lo = 0, hi = s->nruns - 1, found = -1;
            while (lo <= hi) {                          /* the last run starting at or before `at` */
                const int mid = (lo + hi) / 2;
                if (s->runs[mid].lsn <= at) { found = mid; lo = mid + 1; } else hi = mid - 1;
            }
            memset(dst, 0, 2048);
            if (found >= 0 && at - s->runs[found].lsn < s->runs[found].count) {
                const IdxRun *r = &s->runs[found];
                const Uint64 from = (at - r->lsn) * 2048;
                if (from < r->bytes) {
                    const size_t have = (size_t)(r->bytes - from < 2048 ? r->bytes - from : 2048);
                    if (s->rf_i != found) {
                        if (s->rf) SDL_CloseIO(s->rf);
                        s->rf = SDL_IOFromFile(r->path, "rb");
                        s->rf_i = s->rf ? found : -1;
                    }
                    if (!s->rf || SDL_SeekIO(s->rf, (Sint64)from, SDL_IO_SEEK_SET) < 0 ||
                        SDL_ReadIO(s->rf, dst, have) != have)
                        return 0;
                }
            }
        }
    }
    return 1;
}

/* The build's stamp: the release and the disc by its content - its size and the SHA-256 of its first 4 MiB (the
 * volume descriptors, SYSTEM.CNF and the boot executable's start) - not its path or form: the same disc moved,
 * renamed, as a CHD or unpacked into game\ builds nothing again (the maintainer 2026-11-20, ZOE issue 17; the
 * unpacked disc 2026-11-21). An .iso's stamp is what it was. */
static void stamp_text(L *l, char *out, size_t n)
{
    XrSha256 s;
    uint8_t digest[32];
    char hex[65] = "?";
    Uint64 bytes = 0;
    Src d;
    if (src_open(&d, l->iso)) {
        static unsigned char block[64 * 2048];
        Uint64 at, sectors = d.sectors < 2048 ? d.sectors : 2048;
        bytes = d.sectors * 2048;
        xr_sha256_init(&s);
        for (at = 0; at < sectors; at += 64) {
            const Uint32 k = (Uint32)(sectors - at < 64 ? sectors - at : 64);
            if (!src_read(&d, at, k, block))
                break;
            xr_sha256_update(&s, block, (size_t)k * 2048);
        }
        src_close(&d);
        xr_sha256_final(&s, digest);
        xr_sha256_hex(digest, hex);
    }
    snprintf(out, n, "%s\nimage %lld %s\n", l->release, (long long)bytes, hex);
}

typedef struct { L *l; char found[LP]; } FindIso;

static SDL_EnumerationResult SDLCALL iso_cb(void *u, const char *dir, const char *name)
{
    FindIso *f = (FindIso *)u;
    size_t k = strlen(name);
    if (k > 4 && (!SDL_strcasecmp(name + k - 4, ".iso") || !SDL_strcasecmp(name + k - 4, ".chd"))) {
        join(f->found, sizeof f->found, dir, name);
        return SDL_ENUM_SUCCESS;
    }
    return SDL_ENUM_CONTINUE;
}

typedef struct { SDL_AtomicInt done; char path[LP]; } Pick;

static void SDLCALL pick_cb(void *u, const char *const *files, int filter)
{
    Pick *p = (Pick *)u;
    (void)filter;
    if (files && files[0])
        snprintf(p->path, sizeof p->path, "%s", files[0]);
    SDL_SetAtomicInt(&p->done, 1);
}

static int choose_image(L *l, char *out, size_t n)
{
    static const SDL_DialogFileFilter f[] = { { "PS2 disc image", "iso;chd" }, { "All files", "*" } };
    Pick p;
    memset(&p, 0, sizeof p);
    SDL_ShowOpenFileDialog(pick_cb, &p, NULL, f, 2, NULL, false);
    while (!SDL_GetAtomicInt(&p.done)) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            ;
        SDL_Delay(20);
    }
    if (!p.path[0])
        return 0;
    snprintf(out, n, "%s", p.path);
    lg(l, "disc image picked: %s", out);
    return 1;
}

enum { B_CHOOSE = 1, B_OPEN, B_AGAIN, B_QUIT };

static int no_game_dialog(L *l, const char *why)
{
    const SDL_MessageBoxButtonData b[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, B_CHOOSE, "Choose image..." },
        { 0, B_OPEN, "Open folder" },
        { 0, B_AGAIN, "Check again" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, B_QUIT, "Quit" },
    };
    char msg[2000];
    SDL_MessageBoxData d;
    int id = B_QUIT;
    snprintf(msg, sizeof msg,
             "%s\n\nChoose your %s disc image (.iso) with \"Choose image...\" - it is read where it is and never "
             "changed - or place it in:\n\n%s\n\nand choose \"Check again\". \"Open folder\" shows that folder.",
             why, l->cfg->disc, l->root);
    memset(&d, 0, sizeof d);
    d.flags = SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
    d.title = l->cfg->window_title;
    d.message = msg;
    d.numbuttons = (int)(sizeof b / sizeof b[0]);
    d.buttons = b;
    if (!SDL_ShowMessageBox(&d, &id))
        return B_QUIT;
    return id;
}

static void open_folder(L *l, const char *dir)
{
    char url[LP + 16], *s;
    snprintf(url, sizeof url, "file://%s%s", dir[0] == '/' ? "" : "/", dir);
    for (s = url; *s; s++)
        if (*s == '\\')
            *s = '/';
    if (!SDL_OpenURL(url))
        lg(l, "cannot open %s: %s", url, SDL_GetError());
}

/* the first disc found: --image, the game's settings, the unpacked game\disc, an .iso beside the exe or in game/ */
static int find_image(L *l, const char *image_arg)
{
    FindIso f;
    char p[LP];
    if (image_arg && (is_file(image_arg) || is_disc_dir(image_arg))) {
        snprintf(l->iso, sizeof l->iso, "%s", image_arg);
        return 1;
    }
    if (settings_disc(l, p, sizeof p) && (is_file(p) || is_disc_dir(p))) {
        snprintf(l->iso, sizeof l->iso, "%s", p);
        return 1;
    }
    join(p, sizeof p, l->root, "game");
    SDL_strlcat(p, "/disc", sizeof p);
    if (is_disc_dir(p)) {
        snprintf(l->iso, sizeof l->iso, "%s", p);
        return 1;
    }
    memset(&f, 0, sizeof f);
    SDL_EnumerateDirectory(l->root, iso_cb, &f);
    if (!f.found[0]) {
        join(p, sizeof p, l->root, "game");
        SDL_EnumerateDirectory(p, iso_cb, &f);
    }
    if (f.found[0]) {
        snprintf(l->iso, sizeof l->iso, "%s", f.found);
        return 1;
    }
    return 0;
}

/* ── the payload ───────────────────────────────────────────────────────── */

static int read_trailer(L *l, Uint64 *offset)
{
    SDL_IOStream *f = SDL_IOFromFile(l->exe, "rb");
    unsigned char t[TRAILER_SIZE];
    Sint64 size;
    int ok = 0, i;
    if (!f)
        return 0;
    size = SDL_GetIOSize(f);
    if (size > TRAILER_SIZE && SDL_SeekIO(f, size - TRAILER_SIZE, SDL_IO_SEEK_SET) >= 0
        && SDL_ReadIO(f, t, sizeof t) == sizeof t && !memcmp(t, TRAILER_MAGIC, 8)) {
        *offset = 0;
        for (i = 7; i >= 0; i--)
            *offset = (*offset << 8) | t[8 + i];
        memcpy(l->release, t + 16, 32);
        l->release[32] = 0;
        ok = *offset < (Uint64)size;
    }
    SDL_CloseIO(f);
    return ok;
}

/* every entry of the zip at zp below dest, through the zip-slip guard, streamed (no size cap) */
static int unzip_into(L *l, const char *zp, const char *dest, Prog *prog, const char *what)
{
    char err[256];
    ZipFile *z = zip_open(zp, err, sizeof err);
    int i, bad = 0;
    if (!z) {
        lg(l, "%s: %s", what, err);
        return 1;
    }
    prog->total = zip_count(z);
    for (i = 0; i < zip_count(z) && !bad; i++) {
        const ZipEntry *e = zip_entry(z, i);
        char rel[LP], dst[LP], *slash;
        if (!zip_safe_path(e->name, rel, sizeof rel)) {
            lg(l, "%s: refused entry %s", what, e->name);
            bad = 1;
            break;
        }
        join(dst, sizeof dst, dest, rel);
        if (e->dir) {
            SDL_CreateDirectory(dst);
        } else {
            if ((slash = strrchr(dst, '/')) != NULL) {
                *slash = 0;
                SDL_CreateDirectory(dst);
                *slash = '/';
            }
            if (!zip_extract_to_file(z, i, dst, err, sizeof err)) {
                lg(l, "%s: %s: %s", what, e->name, err);
                bad = 1;
            }
#ifndef _WIN32
            /* Linux: a program in the zip (the compiler, Python) runnable again - its Unix mode kept */
            else if (e->mode & 0111)
                chmod(dst, 0755);
#endif
        }
        SDL_AddAtomicInt(&prog->done_n, 1);
    }
    zip_close(z);
    return bad;
}

typedef struct { Prog p; Uint64 offset; } UnpackArg;

static int unpack_job(L *l, void *a)
{
    UnpackArg *u = (UnpackArg *)a;
    char zp[LP], marker[LP];
    SDL_IOStream *in = SDL_IOFromFile(l->exe, "rb"), *out;
    Sint64 end;
    int bad = 0;
    void *buf;
    if (!in)
        return 1;
    end = SDL_GetIOSize(in) - TRAILER_SIZE;
    join(zp, sizeof zp, l->root, "data/payload.zip");
    out = SDL_IOFromFile(zp, "wb");
    if (!out || SDL_SeekIO(in, (Sint64)u->offset, SDL_IO_SEEK_SET) < 0) {
        SDL_CloseIO(in);
        if (out)
            SDL_CloseIO(out);
        return 1;
    }
    buf = malloc(1 << 20);
    while (buf && SDL_TellIO(in) < end) {
        Sint64 left = end - SDL_TellIO(in);
        size_t want = left > (1 << 20) ? (1 << 20) : (size_t)left, got = SDL_ReadIO(in, buf, want);
        if (!got || SDL_WriteIO(out, buf, got) != got) {
            bad = 1;
            break;
        }
    }
    free(buf);
    SDL_CloseIO(in);
    SDL_CloseIO(out);
    if (bad)
        return 1;
    rm_tree(l->tools);
    SDL_CreateDirectory(l->tools);
    bad = unzip_into(l, zp, l->tools, &u->p, "payload");
    SDL_RemovePath(zp);
    if (bad)
        return 1;
    join(marker, sizeof marker, l->tools, "release.txt");
    return write_text(marker, l->release) ? 0 : 1;
}

/* ── the compiler (toolchain.txt format 2, as the Xbox launcher's) ─────── */
/*
 *   format  2
 *   clang   llvm-mingw/bin/clang++.exe      the compiler driver
 *   python  python/python.exe               the translator's Python
 *   bundle  bundle/<zip> <sha256> <size> <folder in the zip> <folder under tools>
 *                                           optional: a shipped zip unpacked at the first start
 */
typedef struct {
    int format;
    char clang[LP], python[LP];
    char zip[LP], sha[65], folder[256], dest[256];
    Sint64 size;
    Prog unpack;
} Toolchain;

static int safe_rel(const char *p)
{
    return p[0] && p[0] != '/' && p[0] != '\\' && !strchr(p, ':') && !strstr(p, "..");
}

static int read_toolchain(L *l, Toolchain *t)
{
    char p[LP], *text, *line, *save = NULL;
    memset(t, 0, sizeof *t);
    join(p, sizeof p, l->tools, "toolchain.txt");
    if (!(text = read_text(p)))
        return 0;
    for (line = SDL_strtok_r(text, "\r\n", &save); line; line = SDL_strtok_r(NULL, "\r\n", &save)) {
        char *argv[8];
        int n = split_tabs(line, argv, 8);
        if (n < 2 || argv[0][0] == '#')
            continue;
        if (!strcmp(argv[0], "format")) t->format = SDL_atoi(argv[1]);
        else if (!strcmp(argv[0], "clang")) snprintf(t->clang, sizeof t->clang, "%s", argv[1]);
        else if (!strcmp(argv[0], "python")) snprintf(t->python, sizeof t->python, "%s", argv[1]);
        else if (!strcmp(argv[0], "bundle") && n >= 6) {
            snprintf(t->zip, sizeof t->zip, "%s", argv[1]);
            snprintf(t->sha, sizeof t->sha, "%s", argv[2]);
            t->size = (Sint64)SDL_strtoll(argv[3], NULL, 10);
            snprintf(t->folder, sizeof t->folder, "%s", argv[4]);
            snprintf(t->dest, sizeof t->dest, "%s", argv[5]);
        }
    }
    SDL_free(text);
    if (t->format != 2 || !safe_rel(t->clang) || !safe_rel(t->python))
        return 0;
    if (t->zip[0] && (!safe_rel(t->zip) || !safe_rel(t->dest) || strlen(t->sha) != 64 || t->size <= 0))
        return 0;
    join(l->clang, sizeof l->clang, l->tools, t->clang);
    join(l->python, sizeof l->python, l->tools, t->python);
    return 1;
}

static int file_sha256(const char *p, char hex[65])
{
    SDL_IOStream *f = SDL_IOFromFile(p, "rb");
    XrSha256 s;
    uint8_t d[32];
    void *buf = malloc(1 << 20);
    size_t got;
    if (!f || !buf) {
        if (f)
            SDL_CloseIO(f);
        free(buf);
        return 0;
    }
    xr_sha256_init(&s);
    while ((got = SDL_ReadIO(f, buf, 1 << 20)) > 0)
        xr_sha256_update(&s, buf, got);
    SDL_CloseIO(f);
    free(buf);
    xr_sha256_final(&s, d);
    xr_sha256_hex(d, hex);
    return 1;
}

static int check_job(L *l, void *a)
{
    Toolchain *t = (Toolchain *)a;
    char hex[65];
    if (!file_sha256(t->zip, hex))
        return 1;
    lg(l, "compiler bundle sha256 %s (expected %s)", hex, t->sha);
    return SDL_strcasecmp(hex, t->sha) ? 2 : 0;
}

static double unpack_frac(void *a)
{
    return prog_frac(&((Toolchain *)a)->unpack);
}

static int unpack_tc_job(L *l, void *a)
{
    Toolchain *t = (Toolchain *)a;
    char tmp[LP], from[LP], to[LP], marker[LP];
    join(tmp, sizeof tmp, l->tools, "bundle/unpack");
    rm_tree(tmp);
    SDL_CreateDirectory(tmp);
    if (unzip_into(l, t->zip, tmp, &t->unpack, "compiler zip"))
        return 1;
    join(from, sizeof from, tmp, t->folder);
    join(to, sizeof to, l->tools, t->dest);
    rm_tree(to);
    if (!SDL_RenamePath(from, to)) {
        lg(l, "cannot move %s to %s: %s", from, to, SDL_GetError());
        return 1;
    }
    rm_tree(tmp);
    join(marker, sizeof marker, to, ".sha256");
    return write_text(marker, t->sha) ? 0 : 1;
}

static int ensure_toolchain(L *l)
{
    Toolchain t;
    char marker[LP], *have;
    if (!read_toolchain(l, &t)) {
        lg(l, "toolchain.txt missing, damaged or not format 2 in %s", l->tools);
        xr_setup_ui_message(l->ui, 1, PACKAGE_INCOMPLETE);
        return 0;
    }
    if (t.zip[0]) {
        char dest[LP], zrel[LP];
        join(dest, sizeof dest, l->tools, t.dest);
        join(marker, sizeof marker, dest, ".sha256");
        have = read_text(marker);
        if (have && !SDL_strcasecmp(have, t.sha) && is_file(l->clang) && is_file(l->python)) {
            SDL_free(have);
            return 1;
        }
        SDL_free(have);
        snprintf(zrel, sizeof zrel, "%s", t.zip);
        join(t.zip, sizeof t.zip, l->tools, zrel);
        if (file_size(t.zip) != t.size) {
            lg(l, "compiler bundle %s: size %lld, expected %lld", t.zip, (long long)file_size(t.zip),
               (long long)t.size);
            xr_setup_ui_message(l->ui, 1, PACKAGE_INCOMPLETE);
            return 0;
        }
        if (run_job(l, "Checking the compiler", 0.02, 0.02, check_job, &t, NULL)) {
            xr_setup_ui_message(l->ui, 1, PACKAGE_INCOMPLETE);
            return 0;
        }
        if (run_job(l, "Unpacking the compiler (once)", 0.04, 0.06, unpack_tc_job, &t, unpack_frac)) {
            xr_setup_ui_message(l->ui, 1, "The compiler could not be unpacked - see data\\launcher.log.");
            return 0;
        }
    }
    if (!is_file(l->clang) || !is_file(l->python)) {
        lg(l, "compiler %s or python %s missing", l->clang, l->python);
        xr_setup_ui_message(l->ui, 1, PACKAGE_INCOMPLETE);
        return 0;
    }
    return 1;
}

/* ── translate: lift.txt, each line "<name> <script> <args...>" run with the shipped Python ───────────── */

typedef struct { Prog p; const char *only; int skip_check; } LiftArg;

static int lift_job(L *l, void *a)
{
    LiftArg *la = (LiftArg *)a;
    char p[LP], *text, *line, *save = NULL, logp[LP];
    int lines = 0, rc = 0;
    join(p, sizeof p, l->tools, "lift.txt");
    if (!(text = read_text(p)))
        return 1;
    for (line = text; *line; line++)
        if (*line == '\n')
            lines++;
    la->p.total = lines ? lines : 1;
    join(logp, sizeof logp, l->build, "lift.log");
    for (line = SDL_strtok_r(text, "\r\n", &save); line && !rc; line = SDL_strtok_r(NULL, "\r\n", &save)) {
        static char exp[MAX_ARGS][LP];
        char *raw[MAX_ARGS], *argv[MAX_ARGS + 4];
        int n = split_tabs(line, raw, MAX_ARGS), i, k = 0;
        if (n < 2 || raw[0][0] == '#')
            continue;
        if (la->only && strcmp(raw[0], la->only))
            continue;
        if (!la->only && la->skip_check && !strcmp(raw[0], "check"))
            continue;
        argv[k++] = l->python;
        argv[k++] = "-I";
        argv[k++] = "-B";
        for (i = 1; i < n && k < MAX_ARGS + 2; i++) {
            expand(l, raw[i], exp[i], LP);
            argv[k++] = exp[i];
        }
        argv[k] = NULL;
        lg(l, "lift: %s", raw[0]);
        rc = run_proc(l, argv, l->tools, logp);
        if (rc)
            lg(l, "lift step %s: exit %d - see %s", raw[0], rc, logp);
        SDL_AddAtomicInt(&la->p.done_n, 1);
    }
    SDL_free(text);
    return rc;
}

/* ── compile + link: build.txt ─────────────────────────────────────────── */
/*
 *   cc    <arg>...           the compile line's arguments (before -c <src> -o <obj>)
 *   glob  <folder> <ext>     every file of that extension in the folder is compiled
 *   src   <file>             one more file to compile
 *   obj   <file>             a prebuilt object (the port's own code) - linked, not compiled
 *   link  <arg>...           the link line; {OBJECTS} = the compiled and the prebuilt objects
 */
#define MAX_UNITS 512
typedef struct {
    Prog p;
    char *cc[MAX_ARGS];
    int ncc;
    char src[MAX_UNITS][LP], objp[MAX_UNITS][LP];
    Sint64 srcsize[MAX_UNITS];
    int nsrc;
    char pre[32][LP];
    int npre;
    SDL_AtomicInt next, failed;
    char *link[MAX_ARGS];
    int nlink;
    int jobs;
    char *text;
    char argbuf[MAX_ARGS * 3][LP];
    int nargbuf;
} Build;

static char *bstore(L *l, Build *b, const char *raw)
{
    char *d;
    if (b->nargbuf >= MAX_ARGS * 3)
        return (char *)raw;
    d = b->argbuf[b->nargbuf++];
    expand(l, raw, d, LP);
    return d;
}

typedef struct { Build *b; const char *ext; } GlobCtx;

static SDL_EnumerationResult SDLCALL glob_cb(void *u, const char *dir, const char *name)
{
    GlobCtx *g = (GlobCtx *)u;
    size_t k = strlen(name), e = strlen(g->ext);
    if (k > e && !SDL_strcasecmp(name + k - e, g->ext) && g->b->nsrc < MAX_UNITS) {
        join(g->b->src[g->b->nsrc], LP, dir, name);
        g->b->srcsize[g->b->nsrc] = file_size(g->b->src[g->b->nsrc]);
        g->b->nsrc++;
    }
    return SDL_ENUM_CONTINUE;
}

static int read_build(L *l, Build *b)
{
    char p[LP], *line, *save = NULL;
    join(p, sizeof p, l->tools, "build.txt");
    if (!(b->text = read_text(p)))
        return 0;
    for (line = SDL_strtok_r(b->text, "\r\n", &save); line; line = SDL_strtok_r(NULL, "\r\n", &save)) {
        char *raw[MAX_ARGS], dir[LP];
        int n = split_tabs(line, raw, MAX_ARGS), i;
        if (n < 1 || raw[0][0] == '#')
            continue;
        if (!strcmp(raw[0], "cc")) {
            for (i = 1; i < n && b->ncc < MAX_ARGS - 1; i++)
                b->cc[b->ncc++] = bstore(l, b, raw[i]);
        } else if (!strcmp(raw[0], "glob") && n > 2) {
            GlobCtx g = { b, raw[2] };
            expand(l, raw[1], dir, LP);
            SDL_EnumerateDirectory(dir, glob_cb, &g);
        } else if (!strcmp(raw[0], "src") && n > 1 && b->nsrc < MAX_UNITS) {
            expand(l, raw[1], b->src[b->nsrc], LP);
            b->srcsize[b->nsrc] = file_size(b->src[b->nsrc]);
            b->nsrc++;
        } else if (!strcmp(raw[0], "obj") && n > 1 && b->npre < 32) {
            expand(l, raw[1], b->pre[b->npre++], LP);
        } else if (!strcmp(raw[0], "link")) {
            for (i = 1; i < n && b->nlink < MAX_ARGS - 1; i++)
                b->link[b->nlink++] = bstore(l, b, raw[i]);
        }
    }
    return b->ncc > 0 && b->nsrc > 0 && b->nlink > 0;
}

typedef struct { L *l; Build *b; int *order; } Worker;

static int SDLCALL compile_worker(void *p)
{
    Worker *w = (Worker *)p;
    Build *b = w->b;
    for (;;) {
        int n = SDL_AddAtomicInt(&b->next, 1), i, k = 0, j;
        char *argv[MAX_ARGS + 8], logp[LP];
        if (n >= b->nsrc)
            break;
        i = w->order[n];
        argv[k++] = w->l->clang;
        for (j = 0; j < b->ncc; j++)
            argv[k++] = b->cc[j];
        argv[k++] = "-c";
        argv[k++] = b->src[i];
        argv[k++] = "-o";
        argv[k++] = b->objp[i];
        argv[k] = NULL;
        snprintf(logp, sizeof logp, "%s.log", b->objp[i]);
        SDL_RemovePath(logp);
        if (run_proc(w->l, argv, w->l->root, logp) != 0) {
            lg(w->l, "compile failed: %s - see %s", b->src[i], logp);
            SDL_AddAtomicInt(&b->failed, 1);
        }
        SDL_AddAtomicInt(&b->p.done_n, 1);
    }
    return 0;
}

/* Compilers at once: the logical processors, but a compiler of the largest translated files takes 2 GB and more
 * - at most one per 2.5 GB of memory (port.py's rule); PS2_LAUNCHER_JOBS caps it further. */
static int compile_jobs(void)
{
    int n = SDL_GetNumLogicalCPUCores(), by_mem = (int)(SDL_GetSystemRAM() / 2560);
    const char *cap = SDL_getenv("PS2_LAUNCHER_JOBS");
    if (by_mem > 0 && by_mem < n)
        n = by_mem;
    if (cap && SDL_atoi(cap) > 0 && SDL_atoi(cap) < n)
        n = SDL_atoi(cap);
    return n < 1 ? 1 : n > 64 ? 64 : n;
}

static int compile_job(L *l, void *a)
{
    Build *b = (Build *)a;
    int n = b->jobs, i, j;
    static int order[MAX_UNITS];
    SDL_Thread *t[64];
    Worker w;
    if (n > b->nsrc)
        n = b->nsrc;
    b->p.total = b->nsrc;
    for (i = 0; i < b->nsrc; i++) {
        const char *s = strrchr(b->src[i], '/'), *bs = strrchr(b->src[i], '\\');
        char name[LP];
        if (bs > s)
            s = bs;
        snprintf(name, sizeof name, "%03d_%s.o", i, s ? s + 1 : b->src[i]);
        join(b->objp[i], LP, l->obj, name);
        order[i] = i;
    }
    /* the largest files first: the longest compiles start at once, the small ones fill in after */
    for (i = 1; i < b->nsrc; i++)
        for (j = i; j > 0 && b->srcsize[order[j]] > b->srcsize[order[j - 1]]; j--) {
            int x = order[j];
            order[j] = order[j - 1];
            order[j - 1] = x;
        }
    w.l = l; w.b = b; w.order = order;
    lg(l, "compiling %d files on %d threads", b->nsrc, n);
    for (i = 0; i < n; i++)
        t[i] = SDL_CreateThread(compile_worker, "compile", &w);
    for (i = 0; i < n; i++)
        if (t[i])
            SDL_WaitThread(t[i], NULL);
    return SDL_GetAtomicInt(&b->failed) ? 1 : 0;
}

static int link_job(L *l, void *a)
{
    Build *b = (Build *)a;
    static char *argv[MAX_UNITS + MAX_ARGS + 40];
    char logp[LP];
    int k = 0, i, j;
    join(logp, sizeof logp, l->build, "link.log");
    SDL_RemovePath(logp);
    argv[k++] = l->clang;
    for (i = 0; i < b->nlink; i++) {
        if (strcmp(b->link[i], "{OBJECTS}")) {
            argv[k++] = b->link[i];
            continue;
        }
        for (j = 0; j < b->npre; j++)
            argv[k++] = b->pre[j];
        for (j = 0; j < b->nsrc; j++)
            argv[k++] = b->objp[j];
    }
    argv[k] = NULL;
    return run_proc(l, argv, l->root, logp) != 0;
}

/* ── the game ──────────────────────────────────────────────────────────── */

static int start_game(L *l)
{
    char *argv[] = { l->out, NULL };
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_Process *proc;
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv);
    SDL_SetStringProperty(p, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, l->root);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(p, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetBooleanProperty(p, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    proc = SDL_CreateProcessWithProperties(p);
    SDL_DestroyProperties(p);
    if (!proc) {
        lg(l, "cannot start %s: %s", l->out, SDL_GetError());
        return 0;
    }
    lg(l, "started %s", l->out);
    SDL_DestroyProcess(proc);               /* the game keeps running; the launcher ends */
    return 1;
}

/* ── a CHD image (chdman createdvd / createcd): the game reads it itself (the engine's Disc); the build's tools
 * read an .iso - written for the build into data/build/work (removed after it), the player's image only read ── */

typedef struct { const char *from, *to; SDL_AtomicInt done_mb; Sint64 total; } ChdArg;

static int chd_job(L *l, void *a)
{
    ChdArg *c = (ChdArg *)a;
    static unsigned char block[512 * 2048];
    SDL_IOStream *out;
    Src d;
    Uint64 at;
    int rc = 1;
    if (!src_open(&d, c->from)) {
        lg(l, "cannot open the disc %s", c->from);
        return 1;
    }
    c->total = (Sint64)(d.sectors * 2048);
    out = SDL_IOFromFile(c->to, "wb");
    if (!out)
        goto done;
    for (at = 0; at < d.sectors; at += 512) {
        const Uint32 k = (Uint32)(d.sectors - at < 512 ? d.sectors - at : 512);
        if (!src_read(&d, at, k, block)) {
            lg(l, "the disc %s: sector %llu unreadable", c->from, (unsigned long long)at);
            goto done;
        }
        if (SDL_WriteIO(out, block, (size_t)k * 2048) != (size_t)k * 2048)
            goto done;
        SDL_SetAtomicInt(&c->done_mb, (int)(((at + k) * 2048) >> 20));
        if (l->ui && xr_setup_ui_cancelled(l->ui))
            goto done;
    }
    rc = 0;
done:
    if (out && !SDL_CloseIO(out))
        rc = 1;
    src_close(&d);
    if (rc)
        SDL_RemovePath(c->to);
    return rc;
}

static double chd_frac(void *a)
{
    ChdArg *c = (ChdArg *)a;
    const Sint64 mb = c->total >> 20;
    return mb > 0 ? (double)SDL_GetAtomicInt(&c->done_mb) / (double)mb : 0.0;
}

/* {ISO} for the build: the image itself, or a CHD's or an unpacked disc's .iso. 0: failed. */
static int lift_image(L *l)
{
    size_t k = strlen(l->iso);
    ChdArg c;
    l->lift_iso[0] = 0;
    if ((k < 4 || SDL_strcasecmp(l->iso + k - 4, ".chd")) && !is_disc_dir(l->iso))
        return 1;
    SDL_CreateDirectory(l->work);
    join(l->lift_iso, sizeof l->lift_iso, l->work, "disc_from_chd.iso");
    memset(&c, 0, sizeof c);
    c.from = l->iso;
    c.to = l->lift_iso;
    if (run_job(l, "Reading the disc", 0.10, 0.01, chd_job, &c, chd_frac)) {
        l->lift_iso[0] = 0;
        xr_setup_ui_message(l->ui, 1, "The disc could not be read - see data\\launcher.log.");
        return 0;
    }
    return 1;
}

/* ── the discs unpacked into game\ (the maintainer 2026-11-21: the images needed only once) ──────────── */

static void plain_dir(const char *in, char *out, size_t n);

typedef struct { char *argv[10]; char progress[LP]; char log[LP]; } UnpackDisc;

static double unpack_disc_frac(void *a)
{
    UnpackDisc *u = (UnpackDisc *)a;
    char *t = read_text(u->progress);
    double f = 0.0;
    if (t) {
        char *e;
        const double done = (double)SDL_strtoull(t, &e, 10), total = (double)SDL_strtoull(e, NULL, 10);
        if (total > 0)
            f = done / total;
        SDL_free(t);
    }
    return f;
}

static int unpack_disc_job(L *l, void *a)
{
    UnpackDisc *u = (UnpackDisc *)a;
    const int rc = run_proc(l, u->argv, l->root, u->log);
    SDL_RemovePath(u->progress);
    return rc;
}

/* `from` (an image or an unpacked folder) into `to` by the built game (--unpack-disc / --unpack-extra). 0: failed. */
static int unpack_disc(L *l, const char *what, const char *mode, const char *from, const char *to, const char *own,
                       double base)
{
    UnpackDisc u;
    int k = 0;
    memset(&u, 0, sizeof u);
    snprintf(u.progress, sizeof u.progress, "%s.progress", to);
    join(u.log, sizeof u.log, l->build, "unpack.log");
    u.argv[k++] = l->out;
    u.argv[k++] = (char *)mode;
    u.argv[k++] = (char *)from;
    u.argv[k++] = (char *)to;
    if (own) {
        u.argv[k++] = "--disc";
        u.argv[k++] = (char *)own;
    }
    u.argv[k] = NULL;
    if (run_job(l, what, base, 0.01, unpack_disc_job, &u, unpack_disc_frac)) {
        lg(l, "%s failed - see %s", what, u.log);
        return 0;
    }
    lg(l, "%s -> %s", from, to);
    return 1;
}

/* A backed-up game folder chosen as the disc (its game\ or the folder above it): its unpacked disc, and - when
 * those lines are empty - its Japanese disc and its subtitle pack (the maintainer 2026-11-21). A disc.idx chosen:
 * its folder. */
static void adopt_game_folder(L *l, char *extra, size_t extra_n, char *file, size_t file_n)
{
    char d[LP], g[LP], x[LP];
    size_t k = strlen(l->iso);
    if (k > 8 && !SDL_strcasecmp(l->iso + k - 8, "disc.idx")) {
        l->iso[k - 8] = 0;
        plain_dir(l->iso, d, sizeof d);
        snprintf(l->iso, sizeof l->iso, "%s", d);
    }
    if (is_file(l->iso) || is_disc_dir(l->iso))
        return;
    snprintf(g, sizeof g, "%s", l->iso);                    /* the game\ folder itself, or the folder above it */
    join(d, sizeof d, g, "disc");
    if (!is_disc_dir(d)) {
        join(g, sizeof g, l->iso, "game");
        join(d, sizeof d, g, "disc");
        if (!is_disc_dir(d))
            return;
    }
    snprintf(l->iso, sizeof l->iso, "%s", d);
    lg(l, "a game folder chosen: its disc %s", d);
    join(x, sizeof x, g, "disc_ja");
    if (extra && !extra[0] && l->cfg->extra_disc_key && is_disc_dir(x)) {
        snprintf(extra, extra_n, "%s", x);
        lg(l, "...its second disc %s", x);
    }
    if (file && !file[0] && l->cfg->extra_file_name) {
        join(x, sizeof x, g, l->cfg->extra_file_name);
        if (is_file(x)) {
            snprintf(file, file_n, "%s", x);
            lg(l, "...its %s", x);
        }
    }
}

/* ── the target folder (issue 12) ─────── */

/* A folder's path without the separators at its end, for comparing. */
static void plain_dir(const char *in, char *out, size_t n)
{
    size_t k;
    snprintf(out, n, "%s", in);
    k = strlen(out);
    while (k > 1 && (out[k - 1] == '/' || out[k - 1] == '\\'))
        out[--k] = 0;
}

static int same_dir(const char *a, const char *b)
{
    char x[LP], y[LP];
    size_t i;
    plain_dir(a, x, sizeof x);
    plain_dir(b, y, sizeof y);
    for (i = 0; x[i]; i++) if (x[i] == '\\') x[i] = '/';
    for (i = 0; y[i]; i++) if (y[i] == '\\') y[i] = '/';
#ifdef _WIN32
    return !SDL_strcasecmp(x, y);
#else
    return !strcmp(x, y);
#endif
}

static const char *base_name(const char *p)
{
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    const char *s = a > b ? a : b;
    return s ? s + 1 : p;
}

typedef struct { const char *from[3]; const char *to[3]; Sint64 total; SDL_AtomicInt done_mb; } CopyArg;

/* The files copied (never moved: the player's own image stays where it is - the maintainer's rule), chunk by
 * chunk; one already there with the same size is kept. */
static int copy_job(L *l, void *a)
{
    CopyArg *c = (CopyArg *)a;
    Sint64 done = 0;
    int i;
    char *buf = (char *)SDL_malloc(4 << 20);
    if (!buf)
        return 1;
    for (i = 0; i < 3 && c->from[i]; i++) {
        SDL_IOStream *in, *out;
        size_t got;
        char part[LP];
        if (file_size(c->to[i]) == file_size(c->from[i])) {
            done += file_size(c->from[i]);
            lg(l, "%s is there already", c->to[i]);
            continue;
        }
        snprintf(part, sizeof part, "%s.part", c->to[i]);
        in = SDL_IOFromFile(c->from[i], "rb");
        out = SDL_IOFromFile(part, "wb");
        if (!in || !out) {
            lg(l, "cannot copy %s to %s: %s", c->from[i], c->to[i], SDL_GetError());
            if (in) SDL_CloseIO(in);
            if (out) SDL_CloseIO(out);
            SDL_free(buf);
            return 1;
        }
        while ((got = SDL_ReadIO(in, buf, 4 << 20)) > 0) {
            if (SDL_WriteIO(out, buf, got) != got || (l->ui && xr_setup_ui_cancelled(l->ui))) {
                SDL_CloseIO(in);
                SDL_CloseIO(out);
                SDL_RemovePath(part);
                SDL_free(buf);
                return 1;
            }
            done += (Sint64)got;
            SDL_SetAtomicInt(&c->done_mb, (int)(done >> 20));
        }
        SDL_CloseIO(in);
        if (!SDL_CloseIO(out) || !SDL_RenamePath(part, c->to[i])) {
            lg(l, "cannot finish %s: %s", c->to[i], SDL_GetError());
            SDL_free(buf);
            return 1;
        }
        lg(l, "copied %s to %s", c->from[i], c->to[i]);
    }
    SDL_free(buf);
    return 0;
}

static double copy_frac(void *a)
{
    CopyArg *c = (CopyArg *)a;
    const Sint64 mb = c->total >> 20;
    return (double)SDL_GetAtomicInt(&c->done_mb) / (double)(mb > 0 ? mb : 1);
}


/* The port's own optional file (PsLauncherConfig.extra_file_*: ZOE's subtitle pack) copied into `folder` (the game's
 * game\, with the unpacked discs) under its name, where the game looks for it - the player's download stays where
 * it is. 0: failed. */
static int place_file(L *l, const char *folder, const char *file)
{
    char to[LP];
    CopyArg c;
    if (!l->cfg->extra_file_name || !file || !file[0])
        return 1;
    join(to, sizeof to, folder, l->cfg->extra_file_name);
    if (!strcmp(file, to))
        return 1;
    memset(&c, 0, sizeof c);
    c.from[0] = file;
    c.to[0] = to;
    c.total = file_size(file);
    SDL_RemovePath(to);                 /* (a newer one of the same size too) */
    if (run_job(l, "Copying the subtitles", 0.0, 0.01, copy_job, &c, copy_frac)) {
        xr_setup_ui_message(l->ui, 1, "Copying the file failed - see data\\launcher.log.");
        return 0;
    }
    lg(l, "%s copied to %s", file, to);
    return 1;
}

/* Another target folder than the exe's: this exe copied there and started there with the choices (--go), this one
 * ends. 1: started. */
static int move_to_target(L *l, const char *target, const char *extra, const char *file)
{
    char exe_to[LP], *argv[12];
    int k = 0;
    SDL_PropertiesID p;
    SDL_Process *proc;
    CopyArg c;
    if (!SDL_CreateDirectory(target)) {
        lg(l, "cannot make %s: %s", target, SDL_GetError());
        xr_setup_ui_message(l->ui, 1, "The target folder cannot be made - choose another one.");
        return 0;
    }
    join(exe_to, sizeof exe_to, target, base_name(l->exe));
    memset(&c, 0, sizeof c);
    c.from[0] = l->exe;
    c.to[0] = exe_to;
    c.total = file_size(l->exe);
    if (run_job(l, "Copying the game into the target folder", 0.0, 0.02, copy_job, &c, copy_frac)) {
        xr_setup_ui_message(l->ui, 1, "The game cannot be copied into the target folder - see data\\launcher.log.");
        return 0;
    }
#ifndef _WIN32
    chmod(exe_to, 0755);                /* (the copy a program again: the AppImage) */
#endif
    argv[k++] = exe_to;
    argv[k++] = "--go";
    argv[k++] = "--image";
    argv[k++] = l->iso;
    if (extra && extra[0]) {
        argv[k++] = "--extra";
        argv[k++] = (char *)extra;
    }
    if (file && file[0]) {
        argv[k++] = "--file";
        argv[k++] = (char *)file;
    }
    argv[k] = NULL;
    p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv);
    SDL_SetStringProperty(p, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, target);
    SDL_SetBooleanProperty(p, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    proc = SDL_CreateProcessWithProperties(p);
    SDL_DestroyProperties(p);
    if (!proc) {
        lg(l, "cannot start %s: %s", exe_to, SDL_GetError());
        return 0;
    }
    lg(l, "installing in %s: started %s", target, exe_to);
    SDL_DestroyProcess(proc);
    return 1;
}

/* ── main ──────────────────────────────────────────────────────────────── */

int ps_launcher_main(const PsLauncherConfig *cfg, int argc, char **argv)
{
    static L l;
    const char *image = NULL, *base;
    int repair = 0, no_start = 0, setup = 0, i, ok = 0, rc, found_image = 0, form = -1, go = 0, copy = 1;
    char extra[LP] = "", target[LP] = "", file[LP] = "";
    const char *extra_arg = NULL, *file_arg = NULL;
    char p[LP], stamp_want[LP + 128], *stamp_have, logp[LP];
    Uint64 payload_ofs = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--repair")) repair = 1;
        else if (!strcmp(argv[i], "--setup")) setup = 1;   /* the setup page even when built (a second disc added) */
        else if (!strcmp(argv[i], "--no-start")) no_start = 1;
        else if (!strcmp(argv[i], "--image") && i + 1 < argc) image = argv[++i];
        /* (the setup page's choices, given on by the exe it was copied from into the target folder) */
        else if (!strcmp(argv[i], "--go")) go = 1;
        else if (!strcmp(argv[i], "--copy")) copy = 2;
        else if (!strcmp(argv[i], "--extra") && i + 1 < argc) extra_arg = argv[++i];
        else if (!strcmp(argv[i], "--file") && i + 1 < argc) file_arg = argv[++i];
    }
    memset(&l, 0, sizeof l);
    l.cfg = cfg;
    SDL_Init(SDL_INIT_VIDEO);
    base = SDL_GetBasePath();
    snprintf(l.root, sizeof l.root, "%s", base ? base : "./");
    {
        char self[LP] = "";
        const char *exe_name = SDL_getenv("PS2_LAUNCHER_SELF");     /* tests: the exe the payload is on */
#ifdef _WIN32
        wchar_t w[LP];
        DWORD k = GetModuleFileNameW(NULL, w, LP);
        if (k > 0 && k < LP)
            WideCharToMultiByte(CP_UTF8, 0, w, -1, self, (int)sizeof self, NULL, NULL);
#else
        /* Linux: the AppImage is the one file the player has (the payload appended to it, as to the Windows exe) and
         * its folder is the game's - the launcher itself runs from the AppImage's read-only mount */
        const char *appimage = SDL_getenv("APPIMAGE");
        if (appimage && *appimage) {
            char *slash;
            snprintf(self, sizeof self, "%s", appimage);
            snprintf(l.root, sizeof l.root, "%s", appimage);
            slash = strrchr(l.root, '/');
            if (slash)
                slash[1] = 0;
            else
                snprintf(l.root, sizeof l.root, "./");
        } else {
            snprintf(self, sizeof self, "/proc/self/exe");
        }
#endif
        snprintf(l.exe, sizeof l.exe, "%s", exe_name && *exe_name ? exe_name : self);
    }
    join(l.out, sizeof l.out, l.root, cfg->game_exe);
#ifdef _WIN32
    {   /* the linker names it .exe anyway: the stamp's check must look for that file */
        size_t k = strlen(l.out);
        if (k < 4 || SDL_strcasecmp(l.out + k - 4, ".exe"))
            SDL_strlcat(l.out, ".exe", sizeof l.out);
    }
#endif
    join(l.data, sizeof l.data, l.root, "data");
    join(l.tools, sizeof l.tools, l.data, "tools");     /* in data since 2026-11-17 (the maintainer's folder layout) */
    join(l.build, sizeof l.build, l.data, "build");
    join(l.work, sizeof l.work, l.build, "work");
    join(l.obj, sizeof l.obj, l.build, "obj");
    snprintf(l.jobs, sizeof l.jobs, "%d", compile_jobs());
    SDL_CreateDirectory(l.data);
    l.log_mx = SDL_CreateMutex();
    join(logp, sizeof logp, l.data, "launcher.log");
    l.log = SDL_IOFromFile(logp, "wb");
    lg(&l, "%s launcher in %s", cfg->window_title, l.root);
    l.ui = xr_setup_ui_open(cfg->window_title);
    xr_setup_ui_icon(l.ui, cfg->icon_bmp, cfg->icon_bmp_size);

    if (!read_trailer(&l, &payload_ofs)) {
        xr_setup_ui_message(l.ui, 1, PACKAGE_INCOMPLETE);
        goto out;
    }
    lg(&l, "release %s", l.release);
    found_image = find_image(&l, image);
    if (cfg->extra_disc_key)
        settings_get(&l, cfg->extra_disc_key, extra, sizeof extra);
    if (extra_arg)
        snprintf(extra, sizeof extra, "%s", extra_arg);
    /* the port's own optional file (ZOE's subtitle pack): its line pre-filled with the copy beside the game */
    if (cfg->extra_file_label && cfg->extra_file_name) {
        char have[LP], game_dir[LP];
        join(game_dir, sizeof game_dir, l.root, "game");
        join(have, sizeof have, game_dir, cfg->extra_file_name);
        if (!is_file(have))
            join(have, sizeof have, l.root, cfg->extra_file_name);
        if (is_file(have))
            snprintf(file, sizeof file, "%s", have);
        if (file_arg)
            snprintf(file, sizeof file, "%s", file_arg);
        xr_setup_ui_file_line(l.ui, cfg->extra_file_label, cfg->extra_file_filter_name, cfg->extra_file_filter, file,
                              sizeof file);
    }
    if (cfg->extra_disc_label && cfg->extra_disc_key)   /* its line on every page, not only the first one's */
        xr_setup_ui_extra_line(l.ui, cfg->extra_disc_label);
    if (go && found_image) {        /* the choices made on the setup page of the exe this one was copied from */
        adopt_game_folder(&l, extra, sizeof extra, file, sizeof file);
        if (cfg->extra_disc_key)
            settings_set(&l, cfg->extra_disc_key, extra);
        setup = 1;                  /* (not straight into an old build: the page's Go means build) */
        form = 1;
    }
    if (found_image && !repair && !setup) {     /* built from this image already: straight into the game */
        stamp_text(&l, stamp_want, sizeof stamp_want);
        join(p, sizeof p, l.build, "stamp.txt");
        stamp_have = read_text(p);
        if (stamp_have && !strcmp(stamp_have, stamp_want) && is_file(l.out)) {
            SDL_free(stamp_have);
            lg(&l, "disc image %s - built already", l.iso);
            settings_set_disc(&l);      /* the same image found elsewhere (moved): the game reads it there now */
            ok = no_start || start_game(&l);
            goto out;
        }
        SDL_free(stamp_have);
        stamp_have = NULL;
    }
    /* to be built: the setup's first page (Windows: the Windows 95 window - the disc's line, the second disc's, the
       subtitles', the target folder, Go / Quit) */
    if (!go) {
        plain_dir(l.root, target, sizeof target);
        form = xr_setup_ui_form(l.ui, cfg->disc, l.iso, sizeof l.iso, found_image ? NULL : "No disc image was found yet.",
                                cfg->extra_disc_label, extra, sizeof extra, target, sizeof target, &copy);
    }
    if (form == 0)
        goto out;
    if (form > 0 && !go) {
        adopt_game_folder(&l, extra, sizeof extra, file, sizeof file);
        lg(&l, "disc chosen on the setup page: %s; target folder %s", l.iso, target);
        if (target[0] && !same_dir(target, l.root)) {     /* installed there: that copy of this exe goes on */
            ok = move_to_target(&l, target, extra, file);
            goto out;
        }
        if (cfg->extra_disc_key) {      /* the second disc: only remembered for the game (it reads it itself) */
            settings_set(&l, cfg->extra_disc_key, extra);
            lg(&l, "%s: %s", cfg->extra_disc_key, extra[0] ? extra : "(none)");
        }
    }
    if (form < 0 && !found_image) {
        for (;;) {
            int b;
            if (xr_no_dialogs())
                goto out;
            b = no_game_dialog(&l, "No disc image found.");
            if (b == B_CHOOSE && choose_image(&l, l.iso, sizeof l.iso))
                break;
            if (b == B_OPEN)
                open_folder(&l, l.root);
            if (b == B_AGAIN && find_image(&l, NULL))
                break;
            if (b == B_QUIT)
                goto out;
        }
    }
    lg(&l, "disc image %s (%lld bytes)", l.iso, (long long)file_size(l.iso));

    stamp_text(&l, stamp_want, sizeof stamp_want);
    join(p, sizeof p, l.build, "stamp.txt");
    stamp_have = read_text(p);
    if (repair) {
        lg(&l, "repair: building again");
        rm_tree(l.build);
        join(p, sizeof p, l.tools, "release.txt");
        SDL_RemovePath(p);
    } else if (stamp_have && !strcmp(stamp_have, stamp_want) && is_file(l.out)) {
        SDL_free(stamp_have);
        ok = no_start || start_game(&l);
        goto out;
    }
    SDL_free(stamp_have);

    /* the payload */
    {
        char *have;
        join(p, sizeof p, l.tools, "release.txt");
        have = read_text(p);
        if (!have || strcmp(have, l.release)) {
            UnpackArg u;
            memset(&u, 0, sizeof u);
            u.offset = payload_ofs;
            if (run_job(&l, "Unpacking the launcher's files", 0.0, 0.02, unpack_job, &u, prog_frac)) {
                SDL_free(have);
                xr_setup_ui_message(l.ui, 1, "The launcher's files could not be unpacked - see data\\launcher.log.");
                goto out;
            }
        }
        SDL_free(have);
    }
    if (!ensure_toolchain(&l))
        goto out;
    rm_tree(l.build);
    SDL_CreateDirectory(l.build);
    SDL_CreateDirectory(l.obj);
    /* the disc: the right game, the right version - else ask for another image */
    for (;;) {
        LiftArg la;
        if (!lift_image(&l))
            goto out;
        memset(&la, 0, sizeof la);
        la.only = "check";
        rc = run_job(&l, "Checking the disc image", 0.10, 0.01, lift_job, &la, NULL);
        if (rc == 0)
            break;
        if (rc != WRONG_DISC) {
            xr_setup_ui_message(l.ui, 1, "Checking the disc image failed - see data\\build\\lift.log.");
            goto out;
        }
        if (xr_no_dialogs())
            goto out;
        {
            char why[300];
            snprintf(why, sizeof why, "That image is not the %s disc.", cfg->disc);
            plain_dir(l.root, target, sizeof target);
            copy = 0;
            form = xr_setup_ui_form(l.ui, cfg->disc, l.iso, sizeof l.iso, why, cfg->extra_disc_label, extra,
                                    sizeof extra, target, sizeof target, &copy);
            if (form > 0) {
                adopt_game_folder(&l, extra, sizeof extra, file, sizeof file);
                if (cfg->extra_disc_key)
                    settings_set(&l, cfg->extra_disc_key, extra);
            }
            if (form == 0)
                goto out;
            if (form > 0) {
                stamp_text(&l, stamp_want, sizeof stamp_want);
                continue;
            }
        }
        {
            char why[LP + 200];
            int b;
            snprintf(why, sizeof why, "This is not an image of the %s disc:\n%s", cfg->disc, l.iso);
            b = no_game_dialog(&l, why);
            if (b == B_QUIT)
                goto out;
            if (b == B_OPEN)
                open_folder(&l, l.root);
            if (b == B_CHOOSE && !choose_image(&l, l.iso, sizeof l.iso))
                continue;
            if (b == B_AGAIN)
                find_image(&l, NULL);
        }
        stamp_text(&l, stamp_want, sizeof stamp_want);
    }
    {
        LiftArg la;
        memset(&la, 0, sizeof la);
        la.skip_check = 1;
        /* (no time estimates in the steps' texts: they only worry - the maintainer 2026-11-20; the window says once
           that it happens only once, the step its time so far) */
        if (run_job(&l, "Translating the game's code", 0.11, 0.17, lift_job, &la, prog_frac)) {
            xr_setup_ui_message(l.ui, 1, "Translating the game's code failed - see data\\build\\lift.log.");
            goto out;
        }
    }
    {
        static Build b;
        memset(&b, 0, sizeof b);
        b.jobs = SDL_atoi(l.jobs);
        if (!read_build(&l, &b)) {
            xr_setup_ui_message(l.ui, 1, "The launcher's build.txt is missing or damaged - start it with --repair.");
            goto out;
        }
        if (run_job(&l, "Compiling", 0.28, 0.70, compile_job, &b, prog_frac)) {
            xr_setup_ui_message(l.ui, 1, "Compiling failed - see data\\build\\obj\\*.log.");
            SDL_free(b.text);
            goto out;
        }
        if (run_job(&l, "Linking", 0.98, 0.02, link_job, &b, NULL)) {
            xr_setup_ui_message(l.ui, 1, "Linking failed - see data\\build\\link.log.");
            SDL_free(b.text);
            goto out;
        }
        SDL_free(b.text);
    }
    /* the translated sources and the objects are not needed to play: only the logs and the stamp stay */
    rm_tree(l.work);
    rm_tree(l.obj);
    /* and the unpacked compiler (~300 MB): its zip stays in data/tools/bundle, unpacked again in seconds when the game
     * is built again (a new release, --repair) - ensure_toolchain sees the marker gone */
    {
        static Toolchain t;
        if (read_toolchain(&l, &t) && t.zip[0]) {
            join(p, sizeof p, l.tools, t.dest);
            if (!rm_tree(p))
                lg(&l, "cannot remove %s", p);
        }
    }
    /* the discs unpacked into game\ (the maintainer 2026-11-21): the game's own disc whole, the second one as far as
       the port reads it - read from there from now on, the images needed no more (only read, never removed) */
    {
        char game_dir[LP], disc_dir[LP], extra_dir[LP];
        join(game_dir, sizeof game_dir, l.root, "game");
        SDL_CreateDirectory(game_dir);
        join(disc_dir, sizeof disc_dir, game_dir, "disc");
        join(extra_dir, sizeof extra_dir, game_dir, "disc_ja");
        if (!same_dir(l.iso, disc_dir)) {
            if (!unpack_disc(&l, "Unpacking the disc into the game folder", "--unpack-disc", l.iso, disc_dir, NULL, 0.98)) {
                xr_setup_ui_message(l.ui, 1, "Unpacking the disc failed - see data\\build\\unpack.log.");
                goto out;
            }
            snprintf(l.iso, sizeof l.iso, "%s", disc_dir);
        }
        if (cfg->extra_disc_key) {
            if (extra[0] && !same_dir(extra, extra_dir)) {
                if (unpack_disc(&l, "Unpacking the second disc", "--unpack-extra", extra, extra_dir, disc_dir, 0.99))
                    snprintf(extra, sizeof extra, "%s", extra_dir);
                else
                    xr_setup_ui_message(l.ui, 0, "The second disc could not be unpacked (see data\\build\\unpack.log) - "
                                        "the game reads it where it is.");
            }
            settings_set(&l, cfg->extra_disc_key, extra);
            lg(&l, "%s: %s", cfg->extra_disc_key, extra[0] ? extra : "(none)");
        }
        if (!place_file(&l, game_dir, file))
            goto out;
    }
    join(p, sizeof p, l.build, "stamp.txt");
    write_text(p, stamp_want);
    settings_set_disc(&l);
    lg(&l, "built %s", l.out);
    ok = no_start || start_game(&l);
out:
    /* a CHD's .iso for the build: ours, gone with it (rm_tree keeps disc images - a player's own - so by name) */
    if (l.lift_iso[0] && SDL_RemovePath(l.lift_iso))
        lg(&l, "removed %s", l.lift_iso);
    xr_setup_ui_close(l.ui);
    if (l.log)
        SDL_CloseIO(l.log);
    SDL_Quit();
    return ok ? 0 : 1;
}
