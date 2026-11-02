/*
 * mhtop - interactive process manager for the macOS terminal (htop rewrite).
 *
 * C99 + macOS system libraries only (libSystem, IOKit, CoreFoundation,
 * Security). No curses: raw termios, ANSI/VT escapes and a diffing cell
 * renderer. Build: make   Usage: ./mhtop [-d secs] [-s key] [-t] [-u user]
 */
#define _DARWIN_C_SOURCE 1

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <Security/Security.h>
#include <arpa/inet.h>
#include <errno.h>
#include <libproc.h>
#include <limits.h>
#include <locale.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <math.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/proc_info.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define APP "mhtop"
#define VERSION "1.0"

extern char **environ;
/* libsystem_kernel; no public header ships it. */
int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);

/* ------------------------------------------------------------------ */
/* utilities                                                           */
/* ------------------------------------------------------------------ */

static void term_restore(void);

static void die(const char *msg)
{
    term_restore();
    fprintf(stderr, APP ": %s\n", msg);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static char *xasprintf(const char *fmt, ...)
{
    va_list ap;
    char *s = NULL;
    va_start(ap, fmt);
    if (vasprintf(&s, fmt, ap) < 0 || !s) die("out of memory");
    va_end(ap);
    return s;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* "512K", "12.3M", "1.20G" */
static void fmt_mem(uint64_t b, char *o, size_t sz)
{
    static const char u[] = "KMGTP";
    double v = (double)b / 1024.0;
    int i = 0;
    if (b < 1024) { snprintf(o, sz, "%lluB", (unsigned long long)b); return; }
    while (v >= 1000.0 && i < 4) { v /= 1024.0; i++; }
    snprintf(o, sz, v < 10 ? "%.2f%c" : v < 100 ? "%.1f%c" : "%.0f%c", v, u[i]);
}

/* "1.2 MB/s" style, decimal-free under 1 KB */
static void fmt_rate(double bps, char *o, size_t sz)
{
    char t[24];
    if (bps < 0) { snprintf(o, sz, "-"); return; }
    if (bps < 1024) { snprintf(o, sz, "%.0fB/s", bps); return; }
    fmt_mem((uint64_t)bps, t, sizeof t);
    snprintf(o, sz, "%s/s", t);
}

static void fmt_power(double w, char *o, size_t sz)
{
    if (w < 0) snprintf(o, sz, "-");
    else if (w < 0.0005) snprintf(o, sz, "0");
    else if (w < 1.0) snprintf(o, sz, "%.0fmW", w * 1000.0);
    else if (w < 100.0) snprintf(o, sz, "%.2fW", w);
    else snprintf(o, sz, "%.0fW", w);
}

/* htop TIME+: m:ss.cc below an hour, then h:mm:ss, then days */
static void fmt_cputime(uint64_t ns, char *o, size_t sz)
{
    uint64_t cs = ns / 10000000ull, s = cs / 100;
    if (s < 3600) snprintf(o, sz, "%llu:%02llu.%02llu", s / 60, s % 60, cs % 100);
    else if (s < 86400 * 4) snprintf(o, sz, "%lluh%02llu:%02llu", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(o, sz, "%llud%02lluh", s / 86400, (s / 3600) % 24);
}

static void fmt_dur(double secs, char *o, size_t sz)
{
    long s = (long)secs;
    if (s < 60) snprintf(o, sz, "%lds", s);
    else if (s < 3600) snprintf(o, sz, "%ldm %02lds", s / 60, s % 60);
    else if (s < 86400) snprintf(o, sz, "%ldh %02ldm", s / 3600, (s / 60) % 60);
    else snprintf(o, sz, "%ldd %02ldh %02ldm", s / 86400, (s / 3600) % 24, (s / 60) % 60);
}

static void fmt_count(int64_t v, char *o, size_t sz)
{
    char t[32];
    int n = snprintf(t, sizeof t, "%lld", (long long)v), i;
    size_t j = 0;
    for (i = 0; i < n && j + 2 < sz; i++) {
        if (i > 0 && t[i - 1] != '-' && (n - i) % 3 == 0) o[j++] = ',';
        o[j++] = t[i];
    }
    o[j] = 0;
}

/* ------------------------------------------------------------------ */
/* growable byte buffer                                                */
/* ------------------------------------------------------------------ */

typedef struct { char *p; size_t n, cap; } Buf;

static void bput(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n + 1) c *= 2;
        b->p = xrealloc(b->p, c);
        b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }

static void bprintf(Buf *b, const char *fmt, ...)
{
    char t[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof t) n = (int)sizeof t - 1;
    bput(b, t, (size_t)n);
}

static void write_all(const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, p, n);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return;
        }
        p += w;
        n -= (size_t)w;
    }
}

/* ------------------------------------------------------------------ */
/* UTF-8 aware text measurement                                        */
/* ------------------------------------------------------------------ */

typedef struct { int n, w, bad; } Glyph;

static Glyph glyph(const char *s, size_t len)
{
    Glyph g = { 1, 1, 1 };
    unsigned char c = (unsigned char)*s;
    wchar_t wc;
    mbstate_t st;
    size_t r;
    int w;
    if (c < 0x80) {
        g.bad = c < 32 || c == 127;
        return g;
    }
    memset(&st, 0, sizeof st);
    r = mbrtowc(&wc, s, len, &st);
    if (r == (size_t)-1 || r == (size_t)-2 || r == 0) return g;
    g.n = (int)r;
    w = wcwidth(wc);
    if (w < 0 || (wc >= 0x80 && wc < 0xa0)) return g; /* printed as '?' */
    g.w = w;
    g.bad = 0;
    return g;
}

static int str_width(const char *s)
{
    size_t len = strlen(s), i = 0;
    int w = 0;
    while (i < len) {
        Glyph g = glyph(s + i, len - i);
        w += g.bad ? 1 : g.w;
        i += (size_t)g.n;
    }
    return w;
}

/* ------------------------------------------------------------------ */
/* cell screen with diffing flush                                      */
/* ------------------------------------------------------------------ */

#define CDEF 0xFFFF /* terminal default colour */
enum { A_BOLD = 1, A_DIM = 2, A_UL = 4, A_REV = 8, A_ITAL = 16, A_STRIKE = 32 };

typedef struct { uint16_t fg, bg; uint8_t at; } St;
typedef struct { uint16_t fg, bg; uint8_t at, n, w; char g[4]; } Cell; /* n == 0: wide-glyph tail */

static St ST(int fg, int bg, int at)
{
    St s;
    s.fg = (uint16_t)fg;
    s.bg = (uint16_t)bg;
    s.at = (uint8_t)at;
    return s;
}

static struct {
    int w, h, full;
    Cell *cur, *prev;
    Buf out;
} SC;

static void scr_resize(int w, int h)
{
    size_t n = (size_t)w * (size_t)h;
    SC.w = w;
    SC.h = h;
    SC.cur = xrealloc(SC.cur, n * sizeof(Cell));
    SC.prev = xrealloc(SC.prev, n * sizeof(Cell));
    SC.full = 1;
}

static void cell_blank(Cell *c, St s)
{
    c->fg = s.fg;
    c->bg = s.bg;
    c->at = s.at;
    c->n = 1;
    c->w = 1;
    c->g[0] = ' ';
}

static void scr_clear(void)
{
    size_t i, n = (size_t)SC.w * (size_t)SC.h;
    St d = ST(CDEF, CDEF, 0);
    for (i = 0; i < n; i++) cell_blank(&SC.cur[i], d);
}

static Cell *cell_at(int y, int x) { return &SC.cur[(size_t)y * (size_t)SC.w + (size_t)x]; }

/* Write one glyph; repairs wide glyphs that get partially overwritten. */
static void set_cell(int y, int x, St s, const char *g, int n, int w)
{
    Cell *c;
    if (y < 0 || y >= SC.h || x < 0 || x >= SC.w) return;
    c = cell_at(y, x);
    if (c->n == 0 && x > 0) cell_blank(cell_at(y, x - 1), ST(c->fg, c->bg, 0));
    if (c->w == 2 && x + 1 < SC.w) cell_blank(cell_at(y, x + 1), s);
    if (w == 2 && x + 1 >= SC.w) { g = " "; n = 1; w = 1; }
    c->fg = s.fg;
    c->bg = s.bg;
    c->at = s.at;
    c->n = (uint8_t)n;
    c->w = (uint8_t)w;
    memcpy(c->g, g, (size_t)n);
    if (w == 2) {
        Cell *t = cell_at(y, x + 1);
        if (t->w == 2 && x + 2 < SC.w) cell_blank(cell_at(y, x + 2), s);
        *t = *c;
        t->n = 0;
        t->w = 0;
    }
}

static void scr_fill(int y, int x, int w, St s)
{
    int i;
    for (i = 0; i < w; i++) set_cell(y, x + i, s, " ", 1, 1);
}

/* Puts at most maxw columns of s; ellipsis when `ell` and truncated.
 * Returns the number of columns used. */
static int scr_put2(int y, int x, int maxw, St s, const char *str, int ell)
{
    size_t len = strlen(str), i = 0;
    int used = 0, limit = maxw;
    if (maxw <= 0) return 0;
    if (ell && str_width(str) > maxw) limit = maxw - 1;
    while (i < len) {
        Glyph g = glyph(str + i, len - i);
        if (g.w == 0 && !g.bad) { i += (size_t)g.n; continue; }
        if (used + (g.bad ? 1 : g.w) > limit) break;
        if (g.bad) set_cell(y, x + used, s, "?", 1, 1);
        else set_cell(y, x + used, s, str + i, g.n, g.w);
        used += g.bad ? 1 : g.w;
        i += (size_t)g.n;
    }
    if (ell && i < len && used < maxw) {
        set_cell(y, x + used, s, "\xe2\x80\xa6", 3, 1);
        used++;
    }
    return used;
}

static int scr_put(int y, int x, int maxw, St s, const char *str) { return scr_put2(y, x, maxw, s, str, 1); }

static int scr_printf(int y, int x, int maxw, St s, const char *fmt, ...)
{
    char t[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    return scr_put(y, x, maxw, s, t);
}

/* Right-aligned within w columns. */
static void scr_rput(int y, int x, int w, St s, const char *str)
{
    int sw = str_width(str);
    if (sw > w) sw = w;
    scr_fill(y, x, w - sw, s);
    scr_put2(y, x + w - sw, sw, s, str, 0);
}

static void sgr(Buf *b, const Cell *c)
{
    bstr(b, "\x1b[0");
    if (c->at & A_BOLD) bstr(b, ";1");
    if (c->at & A_DIM) bstr(b, ";2");
    if (c->at & A_ITAL) bstr(b, ";3");
    if (c->at & A_UL) bstr(b, ";4");
    if (c->at & A_REV) bstr(b, ";7");
    if (c->at & A_STRIKE) bstr(b, ";9");
    if (c->fg != CDEF) bprintf(b, ";38;5;%u", c->fg);
    if (c->bg != CDEF) bprintf(b, ";48;5;%u", c->bg);
    bput(b, "m", 1);
}

static int same_cell(const Cell *a, const Cell *b)
{
    return a->fg == b->fg && a->bg == b->bg && a->at == b->at && a->n == b->n && a->w == b->w &&
           memcmp(a->g, b->g, a->n) == 0;
}

static void scr_flush(void)
{
    Buf *b = &SC.out;
    int y, x, cy = -1, cx = -1;
    Cell last;
    int have_last = 0;
    b->n = 0;
    bstr(b, "\x1b[?2026h");
    if (SC.full) bstr(b, "\x1b[0m\x1b[H\x1b[2J");
    for (y = 0; y < SC.h; y++) {
        for (x = 0; x < SC.w; x++) {
            size_t i = (size_t)y * (size_t)SC.w + (size_t)x;
            Cell *c = &SC.cur[i];
            if (c->n == 0) continue;
            if (!SC.full && same_cell(c, &SC.prev[i]) &&
                (c->w != 2 || x + 1 >= SC.w || same_cell(&SC.cur[i + 1], &SC.prev[i + 1])))
                continue;
            if (cy != y || cx != x) bprintf(b, "\x1b[%d;%dH", y + 1, x + 1);
            if (!have_last || c->fg != last.fg || c->bg != last.bg || c->at != last.at) {
                sgr(b, c);
                last = *c;
                have_last = 1;
            }
            bput(b, c->g, c->n);
            cy = y;
            cx = x + c->w;
        }
    }
    if (b->n > sizeof "\x1b[?2026h" - 1) {
        bstr(b, "\x1b[0m\x1b[?2026l");
        write_all(b->p, b->n);
    }
    memcpy(SC.prev, SC.cur, (size_t)SC.w * (size_t)SC.h * sizeof(Cell));
    SC.full = 0;
}

/* ------------------------------------------------------------------ */
/* terminal                                                            */
/* ------------------------------------------------------------------ */

static struct termios g_orig;
static volatile sig_atomic_t g_raw, g_winch;
static int g_mouse = 1;

#define ENTER_SEQ "\x1b[?1049h\x1b[?25l\x1b[2J"
#define MOUSE_ON "\x1b[?1000h\x1b[?1006h"
#define RESTORE_SEQ "\x1b[?1000l\x1b[?1006l\x1b[?2026l\x1b[0m\x1b[?25h\x1b[?1049l"

static void term_restore(void)
{
    if (!g_raw) return;
    write_all(RESTORE_SEQ, sizeof RESTORE_SEQ - 1);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    g_raw = 0;
}

static void on_fatal_signal(int sig)
{
    if (g_raw) {
        ssize_t r = write(STDOUT_FILENO, RESTORE_SEQ, sizeof RESTORE_SEQ - 1);
        (void)r;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    }
    _exit(128 + sig);
}

static void on_winch(int sig) { (void)sig; g_winch = 1; }

static void term_raw(void)
{
    struct termios raw;
    static int registered;
    if (tcgetattr(STDIN_FILENO, &g_orig) != 0) die("tcgetattr failed");
    raw = g_orig;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(tcflag_t)OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) die("tcsetattr failed");
    g_raw = 1;
    if (!registered) { atexit(term_restore); registered = 1; }
    write_all(ENTER_SEQ, sizeof ENTER_SEQ - 1);
    if (g_mouse) write_all(MOUSE_ON, sizeof MOUSE_ON - 1);
    SC.full = 1;
}

enum {
    K_NONE = 0, K_ESC = 27, K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_PGUP, K_PGDN,
    K_HOME, K_END, K_DEL, K_BTAB, K_MOUSE, K_F1 = 1100 /* .. K_F1 + 11 */
};
#define K_F(n) (K_F1 + (n) - 1)

/* Last mouse event (SGR 1006). b: button code, x/y: 0-based, press: M vs m. */
static struct { int b, x, y, press; } MS;

/* A byte read after a lone Esc that does not start a CSI/SS3 sequence. */
static int g_pending = -1;

static int read_key(int timeout_ms)
{
    struct pollfd p;
    unsigned char c, s[40];
    int n = 0, num = 0;
    p.fd = STDIN_FILENO;
    p.events = POLLIN;
    if (g_pending >= 0) {
        c = (unsigned char)g_pending;
        g_pending = -1;
        return c == 27 ? K_ESC : c;
    }
    if (poll(&p, 1, timeout_ms) <= 0) return K_NONE;
    if (read(STDIN_FILENO, &c, 1) != 1) return K_NONE;
    if (c != 27) return c;
    while (n < 39 && poll(&p, 1, 30) > 0 && read(STDIN_FILENO, &s[n], 1) == 1) {
        n++;
        if (s[0] != '[' && s[0] != 'O') break;
        if (n >= 2 && (s[0] == 'O' || (s[n - 1] >= 0x40 && s[n - 1] <= 0x7e && !(n == 2 && s[1] == '<'))))
            break;
    }
    s[n] = 0;
    if (n == 0) return K_ESC;
    if (s[0] != '[' && s[0] != 'O') {
        g_pending = s[0];
        return K_ESC;
    }
    if (n < 2) return K_NONE;
    if (s[0] == '[' && s[1] == '<') {
        int b, x, y;
        char fin;
        if (sscanf((char *)s + 2, "%d;%d;%d%c", &b, &x, &y, &fin) == 4) {
            MS.b = b;
            MS.x = x - 1;
            MS.y = y - 1;
            MS.press = fin == 'M';
            return K_MOUSE;
        }
        return K_NONE;
    }
    if (s[0] == 'O') {
        switch (s[1]) {
        case 'P': return K_F(1);
        case 'Q': return K_F(2);
        case 'R': return K_F(3);
        case 'S': return K_F(4);
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        }
        return K_NONE;
    }
    switch (s[n - 1]) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case 'Z': return K_BTAB;
    case 'P': return K_F(1);
    case 'Q': return K_F(2);
    case 'R': return K_F(3);
    case 'S': return K_F(4);
    case '~':
        num = atoi((char *)s + 1);
        switch (num) {
        case 1: case 7: return K_HOME;
        case 4: case 8: return K_END;
        case 3: return K_DEL;
        case 5: return K_PGUP;
        case 6: return K_PGDN;
        case 11: case 12: case 13: case 14: case 15: return K_F(num - 10);
        case 17: case 18: case 19: case 20: case 21: return K_F(num - 11);
        case 23: case 24: return K_F(num - 12);
        }
    }
    return K_NONE;
}

/* ------------------------------------------------------------------ */
/* process table                                                       */
/* ------------------------------------------------------------------ */

#define PID_CAP 100000 /* macOS PID_MAX is 99999 */
#define PHIST 60       /* per-process CPU history samples */
#define GHOST_SEC 1.6  /* exited processes linger (red) this long */
#define NEWBORN_SEC 2.0

typedef struct Proc {
    pid_t pid, ppid;
    uid_t uid;
    long start_s, start_us;
    char comm[MAXCOMLEN + 1];
    char name[96];
    char *path;  /* executable, NULL if unknown */
    char *args;  /* argv[1..] joined, NULL if unreadable */
    char state;
    int nice, prio, threads, nrun, pflag;
    int has_task, has_ru;
    /* cumulative counters: current and previous sample */
    uint64_t c_cpu, c_energy, c_gpu, c_disk, c_wake;
    uint64_t o_cpu, o_energy, o_gpu, o_disk, o_wake;
    double t_old;
    int have_old;
    uint64_t mem, res, virt, peak, disk_r, disk_w, pageins, instr, cycles, energy_total, gpu_total;
    /* rates (-1 = unknown) */
    double cpu, gpu, power, disk_bps, wake_ps;
    double dbar; /* eased bar fraction of the sort metric */
    float hist[PHIST];
    int hn;
    double born, died;
    unsigned gen;
    int collapsed, tagged;
    /* per-rebuild scratch */
    unsigned inc, emitted;
    int match;
    struct Proc *kid, *kidtail, *sib;
} Proc;

static Proc *bypid[PID_CAP];
static struct {
    Proc **v;
    size_t n, cap;
    unsigned gen;
    int first_done;
    double t_last;
    uint64_t tb_num, tb_den;
    char *argbuf;
    size_t argmax;
    char **av;
    size_t avcap;
} PT;

static uint64_t tk2ns(uint64_t t) { return t * PT.tb_num / PT.tb_den; }

static struct { uid_t uid; char name[32]; } g_users[128];
static int g_nusers;

static const char *user_name(uid_t uid)
{
    int i;
    struct passwd *pw;
    for (i = 0; i < g_nusers; i++)
        if (g_users[i].uid == uid) return g_users[i].name;
    if (g_nusers == (int)(sizeof g_users / sizeof g_users[0])) g_nusers = 0;
    g_users[g_nusers].uid = uid;
    pw = getpwuid(uid);
    if (pw) snprintf(g_users[g_nusers].name, sizeof g_users[0].name, "%s", pw->pw_name);
    else snprintf(g_users[g_nusers].name, sizeof g_users[0].name, "%u", (unsigned)uid);
    return g_users[g_nusers++].name;
}

/* KERN_PROCARGS2: argc, exec path, argv, envp. Pointers live in a shared
 * scratch buffer valid until the next call. */
typedef struct { const char *exe; char **argv; int argc; char **env; int nenv; } PArgs;

static int pargs_get(pid_t pid, PArgs *a)
{
    int mib[3] = { CTL_KERN, KERN_PROCARGS2, pid }, argc, i;
    size_t sz = PT.argmax, k = 0;
    char *p, *end;
    memset(a, 0, sizeof *a);
    if (pid <= 0 || sysctl(mib, 3, PT.argbuf, &sz, NULL, 0) != 0 || sz < sizeof argc) return 0;
    PT.argbuf[sz] = 0;
    memcpy(&argc, PT.argbuf, sizeof argc);
    p = PT.argbuf + sizeof argc;
    end = PT.argbuf + sz;
    a->exe = p;
    while (p < end && *p) p++;
    while (p < end && !*p) p++;
    for (i = 0;; i++) {
        if (k + 2 > PT.avcap) {
            PT.avcap = PT.avcap ? PT.avcap * 2 : 256;
            PT.av = xrealloc(PT.av, PT.avcap * sizeof *PT.av);
        }
        if (p >= end || (i >= argc && !*p)) break;
        PT.av[k++] = p;
        p += strlen(p) + 1;
    }
    PT.av[k] = NULL;
    a->argv = PT.av;
    a->argc = (int)k < argc ? (int)k : argc;
    a->env = PT.av + a->argc;
    a->nenv = (int)k - a->argc;
    return 1;
}

static char *join_args(char **v, int n)
{
    Buf b = { 0 };
    int i;
    for (i = 0; i < n; i++) {
        if (i) bput(&b, " ", 1);
        bstr(&b, v[i]);
    }
    if (!b.p) return xstrdup("");
    return b.p;
}

static void proc_free(Proc *p)
{
    free(p->path);
    free(p->args);
    free(p);
}

static Proc *proc_new(const struct kinfo_proc *k, double now)
{
    Proc *p = xcalloc(1, sizeof *p);
    char path[PROC_PIDPATHINFO_MAXSIZE];
    PArgs a;
    p->pid = k->kp_proc.p_pid;
    p->start_s = (long)k->kp_proc.p_starttime.tv_sec;
    p->start_us = (long)k->kp_proc.p_starttime.tv_usec;
    memcpy(p->comm, k->kp_proc.p_comm, MAXCOMLEN);
    p->comm[MAXCOMLEN] = 0;
    if (proc_pidpath(p->pid, path, sizeof path) > 0) {
        const char *b = strrchr(path, '/');
        p->path = xstrdup(path);
        snprintf(p->name, sizeof p->name, "%s", b ? b + 1 : path);
    } else {
        snprintf(p->name, sizeof p->name, "%s", p->comm[0] ? p->comm : "?");
    }
    if (pargs_get(p->pid, &a) && a.argc > 0) p->args = join_args(a.argv + 1, a.argc - 1);
    p->cpu = p->gpu = p->power = p->disk_bps = p->wake_ps = -1;
    p->born = PT.first_done ? now : 0;
    if (PT.n == PT.cap) {
        PT.cap = PT.cap ? PT.cap * 2 : 1024;
        PT.v = xrealloc(PT.v, PT.cap * sizeof *PT.v);
    }
    PT.v[PT.n++] = p;
    bypid[p->pid] = p;
    return p;
}

static void proc_mark_dead(Proc *p, double now)
{
    if (p->died > 0) return;
    p->died = now;
    p->state = 'X';
    if (bypid[p->pid] == p) bypid[p->pid] = NULL;
}

static void update_proc(const struct kinfo_proc *k, double now)
{
    pid_t pid = k->kp_proc.p_pid;
    Proc *p;
    struct proc_taskinfo ti;
    rusage_info_current ru;
    if (pid < 0 || pid >= PID_CAP) return;
    p = bypid[pid];
    if (p && (p->start_s != (long)k->kp_proc.p_starttime.tv_sec ||
              p->start_us != (long)k->kp_proc.p_starttime.tv_usec)) {
        proc_mark_dead(p, now);
        p = NULL;
    }
    if (!p) p = proc_new(k, now);
    p->gen = PT.gen;
    p->ppid = k->kp_eproc.e_ppid;
    p->uid = k->kp_eproc.e_ucred.cr_uid;
    p->nice = k->kp_proc.p_nice;
    p->pflag = k->kp_proc.p_flag;
    p->has_task = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof ti) == (int)sizeof ti;
    p->has_ru = proc_pid_rusage(pid, RUSAGE_INFO_CURRENT, (rusage_info_t *)&ru) == 0;
    if (p->has_task) {
        p->c_cpu = tk2ns(ti.pti_total_user + ti.pti_total_system);
        p->res = ti.pti_resident_size;
        p->virt = ti.pti_virtual_size;
        p->threads = ti.pti_threadnum;
        p->nrun = ti.pti_numrunning;
        p->prio = ti.pti_priority;
    }
    if (p->has_ru) {
        if (!p->has_task) p->c_cpu = tk2ns(ru.ri_user_time + ru.ri_system_time);
        p->c_energy = ru.ri_energy_nj;
        p->disk_r = ru.ri_diskio_bytesread;
        p->disk_w = ru.ri_diskio_byteswritten;
        p->c_disk = p->disk_r + p->disk_w;
        p->c_wake = ru.ri_pkg_idle_wkups + ru.ri_interrupt_wkups;
        p->peak = ru.ri_lifetime_max_phys_footprint;
        p->pageins = ru.ri_pageins;
        p->instr = ru.ri_instructions;
        p->cycles = ru.ri_cycles;
        p->energy_total = ru.ri_energy_nj;
        p->mem = ru.ri_phys_footprint;
    } else {
        p->mem = p->has_task ? p->res : 0;
    }
    switch (k->kp_proc.p_stat) {
    case SZOMB: p->state = 'Z'; break;
    case SSTOP: p->state = 'T'; break;
    case SIDL: p->state = 'I'; break;
    default: p->state = p->has_task ? (p->nrun > 0 ? 'R' : 'S') : '?';
    }
    p->c_gpu = 0;
}

/* ------------------------------------------------------------------ */
/* system statistics                                                   */
/* ------------------------------------------------------------------ */

#define HIST 512

typedef struct { pid_t pid; uint64_t ns; } GpuUse;

static struct {
    mach_port_t host;
    int ncpu, nperf, nlevels;
    char lvl_name[2][24];
    int lvl_n[2];
    uint32_t *pt_user, *pt_sys, *pt_idle; /* previous ticks */
    double *core_u, *core_s, *dcore_u, *dcore_s;
    double cpu_u, cpu_s;
    uint64_t mem_total, mem_used, mem_app, mem_wired, mem_comp, mem_cached;
    uint64_t swap_total, swap_used;
    int pressure;
    double load[3];
    time_t boot;
    char model[64], osver[32], host_name[64];
    /* gpu */
    int has_gpu;
    double gpu_util;
    uint64_t gpu_mem;
    char gpu_name[64];
    GpuUse *gu;
    size_t ngu, gucap;
    /* battery */
    io_service_t batt;
    int has_batt, batt_pct, charging, ac, full, cycles, time_rem, adapter_w;
    double sys_w, batt_temp, batt_health;
    /* io */
    uint64_t net_in, net_out, disk_rd, disk_wr;
    double net_in_bps, net_out_bps, disk_rd_bps, disk_wr_bps;
    double t_io;
    int have_io;
    /* totals */
    int nprocs, nthreads, nrunning, nlimited;
    double proc_w; /* sum of per-process power */
    /* history rings (fractions 0..1, or watts for power) */
    float h_cpu[HIST], h_gpu[HIST], h_mem[HIST], h_pwr[HIST];
    int hn, hpos;
    /* eased meter values */
    double d_mem, d_comp, d_wired, d_cached, d_swap, d_gpu, d_batt;
    double sample_t;
} S;

static long sysctl_long(const char *name)
{
    long long v = 0;
    size_t len = sizeof v;
    int i32;
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0) return -1;
    if (len == sizeof i32) {
        memcpy(&i32, &v, sizeof i32);
        return i32;
    }
    return (long)v;
}

static void sysctl_str(const char *name, char *o, size_t sz)
{
    size_t len = sz;
    o[0] = 0;
    if (sysctlbyname(name, o, &len, NULL, 0) != 0) o[0] = 0;
    o[sz - 1] = 0;
}

static void sample_cpu(void)
{
    natural_t n = 0;
    processor_info_array_t info;
    mach_msg_type_number_t cnt;
    uint64_t tu = 0, ts = 0, tt = 0;
    int i;
    if (host_processor_info(S.host, PROCESSOR_CPU_LOAD_INFO, &n, &info, &cnt) != KERN_SUCCESS) return;
    if ((int)n != S.ncpu) {
        S.ncpu = (int)n;
        S.pt_user = xcalloc(n, sizeof *S.pt_user);
        S.pt_sys = xcalloc(n, sizeof *S.pt_sys);
        S.pt_idle = xcalloc(n, sizeof *S.pt_idle);
        S.core_u = xcalloc(n, sizeof(double));
        S.core_s = xcalloc(n, sizeof(double));
        S.dcore_u = xcalloc(n, sizeof(double));
        S.dcore_s = xcalloc(n, sizeof(double));
    }
    for (i = 0; i < (int)n; i++) {
        integer_t *t = &info[i * CPU_STATE_MAX];
        uint32_t u = (uint32_t)t[CPU_STATE_USER] + (uint32_t)t[CPU_STATE_NICE];
        uint32_t s = (uint32_t)t[CPU_STATE_SYSTEM], id = (uint32_t)t[CPU_STATE_IDLE];
        uint32_t du = u - S.pt_user[i], ds = s - S.pt_sys[i], di = id - S.pt_idle[i];
        uint64_t tot = (uint64_t)du + ds + di;
        S.core_u[i] = tot ? (double)du / (double)tot : 0;
        S.core_s[i] = tot ? (double)ds / (double)tot : 0;
        tu += du;
        ts += ds;
        tt += tot;
        S.pt_user[i] = u;
        S.pt_sys[i] = s;
        S.pt_idle[i] = id;
    }
    S.cpu_u = tt ? (double)tu / (double)tt : 0;
    S.cpu_s = tt ? (double)ts / (double)tt : 0;
    vm_deallocate(mach_task_self(), (vm_address_t)info, cnt * sizeof(integer_t));
}

static void sample_mem(void)
{
    vm_statistics64_data_t vm;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    vm_size_t ps = 0;
    struct xsw_usage sw;
    size_t len = sizeof sw;
    int lvl = 0;
    size_t ll = sizeof lvl;
    if (host_page_size(S.host, &ps) != KERN_SUCCESS) ps = 16384;
    if (host_statistics64(S.host, HOST_VM_INFO64, (host_info64_t)&vm, &cnt) == KERN_SUCCESS) {
        uint64_t internal = vm.internal_page_count, purg = vm.purgeable_count;
        S.mem_app = (internal > purg ? internal - purg : 0) * ps;
        S.mem_wired = (uint64_t)vm.wire_count * ps;
        S.mem_comp = (uint64_t)vm.compressor_page_count * ps;
        S.mem_cached = ((uint64_t)vm.external_page_count + purg) * ps;
        S.mem_used = S.mem_app + S.mem_wired + S.mem_comp;
    }
    if (sysctlbyname("vm.swapusage", &sw, &len, NULL, 0) == 0) {
        S.swap_total = sw.xsu_total;
        S.swap_used = sw.xsu_used;
    }
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &lvl, &ll, NULL, 0) == 0) S.pressure = lvl;
    getloadavg(S.load, 3);
}

static int64_t cf_i64(CFTypeRef v, int64_t def)
{
    int64_t r = def;
    if (!v) return def;
    if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &r);
    else if (CFGetTypeID(v) == CFBooleanGetTypeID()) r = CFBooleanGetValue((CFBooleanRef)v);
    return r;
}

static int64_t dict_i64(CFDictionaryRef d, const char *key, int64_t def)
{
    CFStringRef k;
    int64_t r;
    if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID()) return def;
    k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    r = cf_i64(CFDictionaryGetValue(d, k), def);
    CFRelease(k);
    return r;
}

static int64_t reg_i64(io_registry_entry_t e, const char *key, int64_t def)
{
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
    int64_t r = cf_i64(v, def);
    CFRelease(k);
    if (v) CFRelease(v);
    return r;
}

static CFTypeRef reg_prop(io_registry_entry_t e, const char *key)
{
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
    CFRelease(k);
    return v;
}

static void cfstr_get(CFTypeRef s, char *o, size_t sz)
{
    o[0] = 0;
    if (s && CFGetTypeID(s) == CFStringGetTypeID() && !CFStringGetCString((CFStringRef)s, o, (CFIndex)sz, kCFStringEncodingUTF8))
        o[0] = 0;
}

static void gu_add(pid_t pid, uint64_t ns)
{
    if (S.ngu == S.gucap) {
        S.gucap = S.gucap ? S.gucap * 2 : 128;
        S.gu = xrealloc(S.gu, S.gucap * sizeof *S.gu);
    }
    S.gu[S.ngu].pid = pid;
    S.gu[S.ngu].ns = ns;
    S.ngu++;
}

/* GPU: device utilisation from IOAccelerator, per-process accumulated GPU
 * time from each accelerator user client's "AppUsage". */
static void sample_gpu(void)
{
    io_iterator_t it, kids;
    io_object_t acc, cl;
    double util = -1;
    S.ngu = 0;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS) return;
    while ((acc = IOIteratorNext(it))) {
        CFTypeRef ps = reg_prop(acc, "PerformanceStatistics");
        if (ps) {
            int64_t u = dict_i64(ps, "Device Utilization %", -1);
            if (u < 0) u = dict_i64(ps, "GPU Activity(%)", -1);
            if (u >= 0 && (double)u > util) util = (double)u;
            S.gpu_mem = (uint64_t)dict_i64(ps, "In use system memory", 0);
            CFRelease(ps);
        }
        if (!S.gpu_name[0]) {
            CFTypeRef m = reg_prop(acc, "model");
            if (m && CFGetTypeID(m) == CFStringGetTypeID()) cfstr_get(m, S.gpu_name, sizeof S.gpu_name);
            else if (m && CFGetTypeID(m) == CFDataGetTypeID()) {
                CFIndex l = CFDataGetLength(m);
                if (l >= (CFIndex)sizeof S.gpu_name) l = (CFIndex)sizeof S.gpu_name - 1;
                memcpy(S.gpu_name, CFDataGetBytePtr(m), (size_t)l);
                S.gpu_name[l] = 0;
            }
            if (m) CFRelease(m);
        }
        if (IORegistryEntryGetChildIterator(acc, kIOServicePlane, &kids) == KERN_SUCCESS) {
            while ((cl = IOIteratorNext(kids))) {
                CFTypeRef cr = reg_prop(cl, "IOUserClientCreator"), au;
                char who[128];
                int pid = -1;
                cfstr_get(cr, who, sizeof who);
                if (cr) CFRelease(cr);
                if (sscanf(who, "pid %d", &pid) == 1 && pid >= 0 && (au = reg_prop(cl, "AppUsage"))) {
                    if (CFGetTypeID(au) == CFArrayGetTypeID()) {
                        CFIndex i, n = CFArrayGetCount(au);
                        uint64_t ns = 0;
                        for (i = 0; i < n; i++)
                            ns += (uint64_t)dict_i64(CFArrayGetValueAtIndex(au, i), "accumulatedGPUTime", 0);
                        if (ns) gu_add(pid, ns);
                    }
                    CFRelease(au);
                }
                IOObjectRelease(cl);
            }
            IOObjectRelease(kids);
        }
        IOObjectRelease(acc);
    }
    IOObjectRelease(it);
    S.has_gpu = util >= 0;
    S.gpu_util = util >= 0 ? util / 100.0 : 0;
}

/* Battery keys move between macOS releases: some live at the top level of
 * AppleSmartBattery, others only inside a "BatteryData" dictionary on the
 * battery or on one of its child entries. */
static int64_t batt_entry_i64(io_registry_entry_t e, const char *key)
{
    int64_t r = reg_i64(e, key, INT64_MIN);
    CFTypeRef bd;
    if (r != INT64_MIN) return r;
    if ((bd = reg_prop(e, "BatteryData"))) {
        r = dict_i64(bd, key, INT64_MIN);
        CFRelease(bd);
    }
    return r;
}

static int64_t batt_i64(const char *key, int64_t def)
{
    int64_t r = batt_entry_i64(S.batt, key);
    io_iterator_t it;
    io_object_t c;
    if (r != INT64_MIN) return r;
    if (IORegistryEntryGetChildIterator(S.batt, kIOServicePlane, &it) != KERN_SUCCESS) return def;
    while (r == INT64_MIN && (c = IOIteratorNext(it))) {
        r = batt_entry_i64(c, key);
        IOObjectRelease(c);
    }
    IOObjectRelease(it);
    return r == INT64_MIN ? def : r;
}

static void sample_battery(void)
{
    CFTypeRef t;
    int64_t cur, max, v, a, raw, design;
    if (!S.batt) {
        S.batt = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("AppleSmartBattery"));
        if (!S.batt) return;
    }
    S.has_batt = reg_i64(S.batt, "BatteryInstalled", 1) != 0;
    cur = reg_i64(S.batt, "CurrentCapacity", -1);
    max = reg_i64(S.batt, "MaxCapacity", 100);
    S.batt_pct = cur >= 0 && max > 0 ? (int)(cur * 100 / max) : -1;
    S.charging = (int)reg_i64(S.batt, "IsCharging", 0);
    S.ac = (int)reg_i64(S.batt, "ExternalConnected", 0);
    S.full = (int)reg_i64(S.batt, "FullyCharged", 0);
    S.cycles = (int)batt_i64("CycleCount", -1);
    S.time_rem = (int)reg_i64(S.batt, "TimeRemaining", 65535);
    S.batt_temp = (double)batt_i64("Temperature", 0) / 100.0;
    raw = batt_i64("AppleRawMaxCapacity", -1);
    if (raw <= 0) raw = batt_i64("FullChargeCapacity", -1);
    design = batt_i64("DesignCapacity", -1);
    S.batt_health = raw > 0 && design > 0 ? clampd((double)raw / (double)design, 0, 1) : -1;
    v = reg_i64(S.batt, "Voltage", 0);
    a = reg_i64(S.batt, "Amperage", 0);
    S.sys_w = -1;
    if ((t = reg_prop(S.batt, "PowerTelemetryData"))) {
        int64_t load = dict_i64(t, "SystemLoad", -1);
        if (load > 0) S.sys_w = (double)load / 1000.0;
        CFRelease(t);
    }
    if (S.sys_w < 0 && !S.ac && v > 0 && a != 0) S.sys_w = fabs((double)v * (double)a) / 1e6;
    S.adapter_w = 0;
    if ((t = reg_prop(S.batt, "AdapterDetails"))) {
        S.adapter_w = (int)dict_i64(t, "Watts", 0);
        CFRelease(t);
    }
}

static void sample_net(uint64_t *in, uint64_t *out)
{
    int mib[6] = { CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0 };
    size_t len = 0;
    char *buf, *p, *end;
    *in = *out = 0;
    if (sysctl(mib, 6, NULL, &len, NULL, 0) != 0) return;
    buf = xmalloc(len);
    if (sysctl(mib, 6, buf, &len, NULL, 0) != 0) { free(buf); return; }
    for (p = buf, end = buf + len; p + sizeof(struct if_msghdr) <= end;) {
        struct if_msghdr *h = (struct if_msghdr *)p;
        if (h->ifm_msglen == 0) break;
        if (h->ifm_type == RTM_IFINFO2) {
            struct if_msghdr2 *h2 = (struct if_msghdr2 *)p;
            if (!(h2->ifm_flags & IFF_LOOPBACK)) {
                *in += h2->ifm_data.ifi_ibytes;
                *out += h2->ifm_data.ifi_obytes;
            }
        }
        p += h->ifm_msglen;
    }
    free(buf);
}

static void sample_disk(uint64_t *rd, uint64_t *wr)
{
    io_iterator_t it;
    io_object_t d;
    *rd = *wr = 0;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("IOBlockStorageDriver"), &it) != KERN_SUCCESS)
        return;
    while ((d = IOIteratorNext(it))) {
        CFTypeRef st = reg_prop(d, "Statistics");
        if (st) {
            *rd += (uint64_t)dict_i64(st, "Bytes (Read)", 0);
            *wr += (uint64_t)dict_i64(st, "Bytes (Write)", 0);
            CFRelease(st);
        }
        IOObjectRelease(d);
    }
    IOObjectRelease(it);
}

static void sample_io(double now)
{
    uint64_t ni, no, dr, dw;
    double dt = now - S.t_io;
    sample_net(&ni, &no);
    sample_disk(&dr, &dw);
    if (S.have_io && dt > 0) {
        S.net_in_bps = ni >= S.net_in ? (double)(ni - S.net_in) / dt : 0;
        S.net_out_bps = no >= S.net_out ? (double)(no - S.net_out) / dt : 0;
        S.disk_rd_bps = dr >= S.disk_rd ? (double)(dr - S.disk_rd) / dt : 0;
        S.disk_wr_bps = dw >= S.disk_wr ? (double)(dw - S.disk_wr) / dt : 0;
    }
    S.net_in = ni;
    S.net_out = no;
    S.disk_rd = dr;
    S.disk_wr = dw;
    S.t_io = now;
    S.have_io = 1;
}

static void sys_init(void)
{
    mach_timebase_info_data_t tb;
    struct timeval bt;
    size_t len = sizeof bt;
    int mib[2] = { CTL_KERN, KERN_BOOTTIME }, i;
    char *dot;
    S.host = mach_host_self();
    mach_timebase_info(&tb);
    PT.tb_num = tb.numer;
    PT.tb_den = tb.denom ? tb.denom : 1;
    PT.argmax = (size_t)sysctl_long("kern.argmax");
    if ((long)PT.argmax <= 0) PT.argmax = 1 << 20;
    PT.argbuf = xmalloc(PT.argmax + 1);
    if (sysctl(mib, 2, &bt, &len, NULL, 0) == 0) S.boot = bt.tv_sec;
    S.mem_total = (uint64_t)sysctl_long("hw.memsize");
    S.ncpu = 0;
    sysctl_str("machdep.cpu.brand_string", S.model, sizeof S.model);
    if (!S.model[0]) sysctl_str("hw.model", S.model, sizeof S.model);
    sysctl_str("kern.osproductversion", S.osver, sizeof S.osver);
    gethostname(S.host_name, sizeof S.host_name);
    S.host_name[sizeof S.host_name - 1] = 0;
    if ((dot = strstr(S.host_name, ".local"))) *dot = 0;
    S.nlevels = (int)sysctl_long("hw.nperflevels");
    for (i = 0; i < 2 && i < S.nlevels; i++) {
        char key[64];
        snprintf(key, sizeof key, "hw.perflevel%d.name", i);
        sysctl_str(key, S.lvl_name[i], sizeof S.lvl_name[i]);
        snprintf(key, sizeof key, "hw.perflevel%d.logicalcpu", i);
        S.lvl_n[i] = (int)sysctl_long(key);
    }
}

static void hist_push(float *h, float v) { h[S.hpos] = v; }

/* Takes one full sample. `progress` (may be NULL) is called while walking
 * the process list so the splash screen can draw a progress bar. */
static void sample_all(double now, void (*progress)(size_t done, size_t total))
{
    int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
    static struct kinfo_proc *kp;
    static size_t kpcap;
    size_t len, i, n = 0, k;
    double tw = 0;

    sample_cpu();
    sample_mem();
    sample_gpu();
    sample_battery();
    sample_io(now);

    for (;;) {
        len = 0;
        if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0) break;
        len += len / 8 + 64 * sizeof *kp;
        if (len > kpcap) {
            kpcap = len;
            kp = xrealloc(kp, kpcap);
        }
        len = kpcap;
        if (sysctl(mib, 3, kp, &len, NULL, 0) == 0) { n = len / sizeof *kp; break; }
        if (errno != ENOMEM) break;
    }
    PT.gen++;
    for (i = 0; i < n; i++) {
        update_proc(&kp[i], now);
        if (progress && (i & 15) == 0) progress(i, n);
    }
    for (i = 0; i < PT.n; i++) {
        Proc *p = PT.v[i];
        if (!p->died && p->gen != PT.gen) proc_mark_dead(p, now);
    }
    for (k = 0; k < S.ngu; k++) {
        Proc *p = S.gu[k].pid < PID_CAP ? bypid[S.gu[k].pid] : NULL;
        if (p) p->c_gpu += S.gu[k].ns;
    }

    S.nprocs = S.nthreads = S.nrunning = S.nlimited = 0;
    for (i = 0; i < PT.n; i++) {
        Proc *p = PT.v[i];
        double dt = now - p->t_old;
        if (p->died) continue;
        S.nprocs++;
        S.nthreads += p->threads;
        if (p->state == 'R') S.nrunning++;
        if (!p->has_task) S.nlimited++;
        p->gpu_total = p->c_gpu;
        if (p->have_old && dt > 0.05) {
            double dns = dt * 1e9;
            int known = p->has_task || p->has_ru;
            p->cpu = known ? (p->c_cpu >= p->o_cpu ? (double)(p->c_cpu - p->o_cpu) / dns * 100.0 : 0) : -1;
            p->gpu = p->c_gpu >= p->o_gpu ? (double)(p->c_gpu - p->o_gpu) / dns * 100.0 : 0;
            p->power = p->has_ru ? (p->c_energy >= p->o_energy ? (double)(p->c_energy - p->o_energy) / dns : 0) : -1;
            p->disk_bps = p->has_ru ? (p->c_disk >= p->o_disk ? (double)(p->c_disk - p->o_disk) / dt : 0) : -1;
            p->wake_ps = p->has_ru ? (p->c_wake >= p->o_wake ? (double)(p->c_wake - p->o_wake) / dt : 0) : -1;
            if (p->power > 0) tw += p->power;
            p->hist[p->hn % PHIST] = (float)(p->cpu < 0 ? 0 : p->cpu);
            p->hn++;
        }
        if (!p->has_task && !p->has_ru) p->cpu = -1;
        p->o_cpu = p->c_cpu;
        p->o_energy = p->c_energy;
        p->o_gpu = p->c_gpu;
        p->o_disk = p->c_disk;
        p->o_wake = p->c_wake;
        p->t_old = now;
        p->have_old = 1;
    }
    S.proc_w = tw;

    S.hpos = (S.hpos + 1) % HIST;
    hist_push(S.h_cpu, (float)(S.cpu_u + S.cpu_s));
    hist_push(S.h_gpu, (float)S.gpu_util);
    hist_push(S.h_mem, S.mem_total ? (float)((double)S.mem_used / (double)S.mem_total) : 0);
    hist_push(S.h_pwr, (float)(S.sys_w >= 0 ? S.sys_w : S.proc_w));
    if (S.hn < HIST) S.hn++;
    S.sample_t = now;
    PT.t_last = now;
    PT.first_done = 1;
}

/* Drops exited processes whose fade-out finished. Returns 1 if any. */
static int procs_expire(double now)
{
    size_t i = 0;
    int any = 0;
    while (i < PT.n) {
        Proc *p = PT.v[i];
        if (p->died > 0 && now - p->died > GHOST_SEC) {
            PT.v[i] = PT.v[--PT.n];
            proc_free(p);
            any = 1;
        } else {
            i++;
        }
    }
    return any;
}

/* ------------------------------------------------------------------ */
/* UI state, sorting, filtering, tree                                  */
/* ------------------------------------------------------------------ */

enum { SK_CPU, SK_GPU, SK_MEM, SK_POWER, SK_DISK, SK_TIME, SK_THR, SK_PRI, SK_NI, SK_PID, SK_USER, SK_NAME, SK_N };
static const char *SK_LABEL[SK_N] = { "CPU", "GPU", "Memory", "Energy", "Disk", "CPU time", "Threads",
                                      "Priority", "Nice", "PID", "User", "Name" };
static const char *SK_ARG[SK_N] = { "cpu", "gpu", "mem", "power", "disk", "time", "threads",
                                    "pri", "nice", "pid", "user", "name" };
#define NTABS 5 /* Activity Monitor style tabs: CPU GPU Memory Energy Disk */

enum { COL_PID, COL_USER, COL_PRI, COL_NI, COL_THR, COL_S, COL_CPU, COL_GPU, COL_MEM, COL_POWER,
       COL_DISK, COL_TIME, COL_CMD, NCOLS };
static const struct { const char *name; int w, sk, drop; } COLDEF[NCOLS] = {
    { "PID", 6, SK_PID, 99 },    { "USER", 9, SK_USER, 6 },  { "PRI", 3, SK_PRI, 1 },
    { "NI", 3, SK_NI, 4 },       { "THR", 4, SK_THR, 2 },    { "S", 1, -1, 3 },
    { "CPU%", 6, SK_CPU, 99 },   { "GPU%", 5, SK_GPU, 8 },   { "MEM", 7, SK_MEM, 99 },
    { "POWER", 7, SK_POWER, 7 }, { "DISK/s", 8, SK_DISK, 5 }, { "TIME+", 9, SK_TIME, 3 },
    { "Command", 0, SK_NAME, 99 }
};

typedef struct { Proc *p; uint64_t lines; uint16_t depth; uint8_t last, kids; } Row;

static struct {
    int sort, desc, tree, only_user, fullpath, paused, compact, help;
    uid_t user_uid, me;
    int root;
    double interval, next_sample;
    char filter[128];
    int input_mode; /* 0 none, 1 filter, 2 search */
    char input[128];
    Row *rows;
    size_t nrows, rcap, sel, top;
    pid_t sel_pid;
    unsigned rgen;
    double anim_t0, sel_t, last_frame, pulse_t;
    char msg[256];
    double msg_t;
    int msg_err;
    /* layout from the last draw, used for mouse hit-testing */
    int list_y, list_h, tab_y, colhdr_y, tab_x[NTABS + 1];
    int col_on[NCOLS], col_x[NCOLS], col_w[NCOLS], barw;
    double bar_max;
    double click_t;
    size_t click_row;
    int argc;
    char **argv;
    int nmenu; /* modal dialogs open */
} U;

static void set_msg(int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(U.msg, sizeof U.msg, fmt, ap);
    va_end(ap);
    U.msg_t = now_s();
    U.msg_err = err;
}

static int sk_desc_default(int k) { return k != SK_PID && k != SK_USER && k != SK_NAME && k != SK_NI; }
static int sk_metric(int k) { return k != SK_USER && k != SK_NAME; }

static double sval(const Proc *p, int k)
{
    switch (k) {
    case SK_CPU: return p->cpu;
    case SK_GPU: return p->gpu;
    case SK_MEM: return (p->has_task || p->has_ru) ? (double)p->mem : -1;
    case SK_POWER: return p->power;
    case SK_DISK: return p->disk_bps;
    case SK_TIME: return (p->has_task || p->has_ru) ? (double)p->c_cpu : -1;
    case SK_THR: return p->has_task ? p->threads : -1;
    case SK_PRI: return p->has_task ? p->prio : -1;
    case SK_NI: return p->nice;
    default: return p->pid;
    }
}

static int cmp_proc(const void *x, const void *y)
{
    const Proc *a = *(Proc *const *)x, *b = *(Proc *const *)y;
    int r;
    if (U.sort == SK_NAME) {
        r = strcasecmp(a->name, b->name);
    } else if (U.sort == SK_USER) {
        r = strcmp(user_name(a->uid), user_name(b->uid));
    } else {
        double va = sval(a, U.sort), vb = sval(b, U.sort);
        if (U.sort != SK_NI && (va < 0) != (vb < 0)) return va < 0 ? 1 : -1; /* unknown last */
        r = va < vb ? -1 : va > vb;
    }
    if (U.desc) r = -r;
    if (!r) r = a->pid < b->pid ? -1 : a->pid > b->pid;
    return r;
}

/* Case-insensitive match on "name args", the executable path or the exact PID. */
static int cmd_matches(const Proc *p, const char *q)
{
    char line[4096];
    snprintf(line, sizeof line, "%d", p->pid);
    if (!strcmp(line, q)) return 1;
    snprintf(line, sizeof line, "%s%s%s", p->name, p->args && p->args[0] ? " " : "", p->args ? p->args : "");
    return strcasestr(line, q) || (p->path && strcasestr(p->path, q));
}

static int proc_match(const Proc *p)
{
    if (U.only_user && p->uid != U.user_uid) return 0;
    if (!U.filter[0]) return 1;
    return cmd_matches(p, U.filter) || strcasestr(user_name(p->uid), U.filter);
}

static Proc *parent_of(const Proc *p)
{
    if (p->ppid <= 0 || p->ppid == p->pid || p->ppid >= PID_CAP) return NULL;
    return bypid[p->ppid];
}

static Proc *tparent(const Proc *p)
{
    Proc *q = parent_of(p);
    return q && q != p && q->inc == U.rgen ? q : NULL;
}

static void row_push(Proc *p, int depth, uint64_t lines, int last)
{
    Row *r;
    if (U.nrows == U.rcap) {
        U.rcap = U.rcap ? U.rcap * 2 : 1024;
        U.rows = xrealloc(U.rows, U.rcap * sizeof *U.rows);
    }
    r = &U.rows[U.nrows++];
    r->p = p;
    r->depth = (uint16_t)depth;
    r->lines = lines;
    r->last = (uint8_t)last;
    r->kids = p->kid != NULL;
    p->emitted = U.rgen;
}

static void tree_emit(Proc *p, int depth, uint64_t lines, int last)
{
    Proc *c;
    uint64_t sub = lines;
    row_push(p, depth, lines, last);
    if (p->collapsed || depth >= 60) return;
    if (depth > 0 && !last) sub |= 1ull << (depth - 1);
    for (c = p->kid; c; c = c->sib)
        if (c->emitted != U.rgen) tree_emit(c, depth + 1, sub, c->sib == NULL);
}

static size_t sort_scratch_n;
static Proc **sort_scratch;

static void rebuild(void)
{
    size_t i, n = 0;
    Proc **cand;
    U.rgen++;
    if (sort_scratch_n < PT.n) {
        sort_scratch_n = PT.n + 256;
        sort_scratch = xrealloc(sort_scratch, sort_scratch_n * sizeof *sort_scratch);
    }
    cand = sort_scratch;
    for (i = 0; i < PT.n; i++) {
        Proc *p = PT.v[i];
        p->match = proc_match(p);
        p->kid = p->kidtail = p->sib = NULL;
    }
    for (i = 0; i < PT.n; i++) {
        Proc *p = PT.v[i], *q;
        int guard = 0;
        if (!p->match) continue;
        p->inc = U.rgen;
        if (!U.tree) continue;
        for (q = parent_of(p); q && q->inc != U.rgen && guard < 256; q = parent_of(q), guard++)
            q->inc = U.rgen;
    }
    for (i = 0; i < PT.n; i++)
        if (PT.v[i]->inc == U.rgen) cand[n++] = PT.v[i];
    qsort(cand, n, sizeof *cand, cmp_proc);
    U.nrows = 0;
    U.bar_max = 0;
    for (i = 0; i < n; i++) {
        double v = sval(cand[i], U.sort);
        if (sk_metric(U.sort) && v > U.bar_max) U.bar_max = v;
    }
    if (!U.tree) {
        for (i = 0; i < n; i++) row_push(cand[i], 0, 0, 0);
    } else {
        for (i = 0; i < n; i++) {
            Proc *p = cand[i], *par = tparent(p);
            if (!par) continue;
            if (par->kidtail) par->kidtail->sib = p;
            else par->kid = p;
            par->kidtail = p;
        }
        for (i = 0; i < n; i++)
            if (!tparent(cand[i]) && cand[i]->emitted != U.rgen) tree_emit(cand[i], 0, 0, 1);
        for (i = 0; i < n; i++) /* parent cycles (pid reuse races) */
            if (cand[i]->emitted != U.rgen) tree_emit(cand[i], 0, 0, 1);
    }
    if (U.sel_pid >= 0) {
        size_t best = (size_t)-1;
        for (i = 0; i < U.nrows; i++) {
            if (U.rows[i].p->pid != U.sel_pid) continue;
            best = i;
            if (!U.rows[i].p->died) break;
        }
        if (best != (size_t)-1) U.sel = best;
    }
    if (U.sel >= U.nrows) U.sel = U.nrows ? U.nrows - 1 : 0;
    U.sel_pid = U.nrows ? U.rows[U.sel].p->pid : -1;
}

static Proc *sel_proc(void) { return U.nrows ? U.rows[U.sel].p : NULL; }

static void set_sel(size_t i)
{
    if (!U.nrows) return;
    if (i >= U.nrows) i = U.nrows - 1;
    if (i != U.sel) U.sel_t = now_s();
    U.sel = i;
    U.sel_pid = U.rows[i].p->pid;
}

static void clamp_scroll(void)
{
    size_t h = U.list_h > 0 ? (size_t)U.list_h : 1;
    if (!U.nrows) { U.sel = U.top = 0; return; }
    if (U.sel >= U.nrows) U.sel = U.nrows - 1;
    if (U.sel < U.top) U.top = U.sel;
    if (U.sel >= U.top + h) U.top = U.sel - h + 1;
    if (U.top + h > U.nrows) U.top = U.nrows > h ? U.nrows - h : 0;
}

static void set_sort(int k)
{
    if (k == U.sort) U.desc = !U.desc;
    else { U.sort = k; U.desc = sk_desc_default(k); }
    U.anim_t0 = now_s();
    rebuild();
}

/* Target bar fraction for a process under the current sort metric. */
static double bar_frac(const Proc *p)
{
    double v = sval(p, U.sort);
    if (v <= 0) return 0;
    if (U.sort == SK_CPU || U.sort == SK_GPU) return clampd(v / 100.0, 0, 1);
    return U.bar_max > 0 ? clampd(v / U.bar_max, 0, 1) : 0;
}

static int sk_bar(int k) { return k <= SK_THR; }

/* ------------------------------------------------------------------ */
/* drawing primitives                                                  */
/* ------------------------------------------------------------------ */

static const char *PART[9] = {
    " ", "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d", "\xe2\x96\x8c",
    "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89", "\xe2\x96\x88"
};
static const char *LEVEL[9] = {
    " ", "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
    "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"
};
static const char *SPIN[10] = {
    "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8", "\xe2\xa0\xbc",
    "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87", "\xe2\xa0\x8f"
};
/* Palette: greys carry the information, one soft blue is the accent, and
 * muted warm tones appear only for warnings. */
enum {
    P_BRIGHT = 255, P_TEXT = 252, P_SOFT = 248, P_DIM = 245, P_FAINT = 241, P_GUIDE = 238,
    P_PANEL = 235, P_BOX = 234, P_SEL = 237,
    P_ACCENT = 110, /* soft blue: focus, headings, active tab */
    P_ACC_BG = 24,  /* deep blue background behind accented labels */
    P_BAR = 67,     /* steel blue: primary fill */
    P_BAR2 = 96,    /* muted mauve: system CPU, compressed memory */
    P_BAR3 = 101,   /* olive grey: wired memory */
    P_OK = 108,     /* sage */
    P_WARN = 180,   /* sand */
    P_HOT = 174     /* dusty rose */
};
/* calm blue ramp, used only by the splash animation */
static const int GRAD[] = { 60, 61, 67, 68, 74, 110, 111, 153 };
#define NGRAD ((int)(sizeof GRAD / sizeof GRAD[0]))
#define BAR_BG CDEF

/* Fill colour for a 0..1 load: blue, turning rose only near saturation. */
static int grad_col(double f) { return f >= 0.9 ? P_HOT : P_BAR; }

typedef struct { double v; int col; } Seg;

/* Multi-segment eighth-block bar; partial cells blend into the next segment. */
static void seg_bar(int y, int x, int w, const Seg *sg, int ns, int bg)
{
    long e[8], lo;
    double acc = 0;
    int i, s;
    if (ns > 8) ns = 8;
    for (s = 0; s < ns; s++) {
        acc += clampd(sg[s].v, 0, 1);
        e[s] = (long)(clampd(acc, 0, 1) * w * 8 + 0.5);
    }
    for (i = 0; i < w; i++) {
        lo = (long)i * 8;
        for (s = 0; s < ns && e[s] <= lo; s++)
            ;
        if (s == ns) {
            set_cell(y, x + i, ST(P_GUIDE, bg, 0), "\xe2\x94\x80", 3, 1);
            continue;
        }
        if (e[s] >= lo + 8) {
            set_cell(y, x + i, ST(sg[s].col, bg, 0), PART[8], 3, 1);
        } else {
            int nb = bg, k;
            for (k = s + 1; k < ns; k++)
                if (e[k] > e[s]) { nb = sg[k].col; break; }
            set_cell(y, x + i, ST(sg[s].col, nb, 0), PART[e[s] - lo], 3, 1);
        }
    }
}

/* Single-colour bar; gleam >= 0 runs a soft shine across the filled part. */
static void heat_bar(int y, int x, int w, double frac, double gleam, int bg)
{
    long e8 = (long)(clampd(frac, 0, 1) * w * 8 + 0.5), cells = (e8 + 7) / 8;
    int i, gpos = -100, base = grad_col(frac);
    if (gleam >= 0 && cells > 0) gpos = (int)fmod(gleam * 30.0, (double)(cells + 14)) - 4;
    for (i = 0; i < w; i++) {
        long c = e8 - (long)i * 8;
        int col = base, d = abs(i - gpos);
        if (c <= 0) { set_cell(y, x + i, ST(CDEF, bg, 0), " ", 1, 1); continue; }
        if (d == 0) col = 153;
        else if (d == 1) col = P_ACCENT;
        set_cell(y, x + i, ST(col, bg, 0), PART[c >= 8 ? 8 : c], 3, 1);
    }
}

/* label + bar + right-aligned value text */
static void meter(int y, int x, int w, const char *label, int lw, const Seg *sg, int ns,
                  const char *val, int vcol)
{
    int vw = str_width(val), bw;
    if (vw < 6) vw = 6;
    bw = w - lw - vw - 2;
    scr_put(y, x, lw, ST(P_DIM, CDEF, A_BOLD), label);
    if (bw < 3) return;
    seg_bar(y, x + lw + 1, bw, sg, ns, BAR_BG);
    scr_rput(y, x + lw + 1 + bw + 1, vw, ST(vcol, CDEF, 0), val);
}

/* Ring-buffer sparkline, newest sample on the right; h rows tall. */
static void spark(int y, int x, int w, int h, const float *ring, double vmax)
{
    int i, r;
    for (i = 0; i < w; i++) {
        int age = w - 1 - i;
        double v = 0, lv;
        if (age < S.hn) v = ring[(S.hpos - age + HIST) % HIST];
        v = vmax > 0 ? clampd(v / vmax, 0, 1) : 0;
        for (r = 0; r < h; r++) {
            if (age >= S.hn) {
                set_cell(y + r, x + i, ST(P_GUIDE, CDEF, 0), r == h - 1 ? "\xc2\xb7" : " ", r == h - 1 ? 2 : 1, 1);
                continue;
            }
            lv = clampd(v * h * 8 - (double)(h - 1 - r) * 8, 0, 8);
            set_cell(y + r, x + i, ST(P_BAR, CDEF, 0), LEVEL[(int)(lv + 0.5)], (int)(lv + 0.5) ? 3 : 1, 1);
        }
    }
}

static double ease_out(double t)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    t = 1 - t;
    return 1 - t * t * t;
}

#define ROW_ANIM 0.40
#define ROW_STAGGER 0.012

static double row_ease(double now, int row) { return ease_out((now - U.anim_t0 - row * ROW_STAGGER) / ROW_ANIM); }

/* Box with rounded border; returns nothing, content drawn by caller. */
static void draw_box(int y, int x, int w, int h, int col, const char *title)
{
    int i, j;
    St b = ST(col, P_BOX, 0);
    if (w < 4 || h < 2) return;
    for (j = 0; j < h; j++) scr_fill(y + j, x, w, ST(P_TEXT, P_BOX, 0));
    set_cell(y, x, b, "\xe2\x95\xad", 3, 1);
    set_cell(y, x + w - 1, b, "\xe2\x95\xae", 3, 1);
    set_cell(y + h - 1, x, b, "\xe2\x95\xb0", 3, 1);
    set_cell(y + h - 1, x + w - 1, b, "\xe2\x95\xaf", 3, 1);
    for (i = 1; i < w - 1; i++) {
        set_cell(y, x + i, b, "\xe2\x94\x80", 3, 1);
        set_cell(y + h - 1, x + i, b, "\xe2\x94\x80", 3, 1);
    }
    for (j = 1; j < h - 1; j++) {
        set_cell(y + j, x, b, "\xe2\x94\x82", 3, 1);
        set_cell(y + j, x + w - 1, b, "\xe2\x94\x82", 3, 1);
    }
    if (title && w > 8) {
        set_cell(y, x + 2, b, " ", 1, 1);
        i = scr_put(y, x + 3, w - 8, ST(P_TEXT, P_BOX, A_BOLD), title);
        set_cell(y, x + 3 + i, b, " ", 1, 1);
    }
}

/* ------------------------------------------------------------------ */
/* main screen                                                         */
/* ------------------------------------------------------------------ */

static int cpu_val_col(double v)
{
    if (v < 0) return P_FAINT;
    if (v >= 90) return P_HOT;
    if (v >= 50) return P_WARN;
    if (v >= 0.05) return P_TEXT;
    return P_FAINT;
}

static void draw_title(int y, double now)
{
    char line[256], up[32], clk[16];
    time_t t = time(NULL);
    struct tm tm;
    int x, rw;
    localtime_r(&t, &tm);
    strftime(clk, sizeof clk, "%H:%M:%S", &tm);
    fmt_dur((double)(t - S.boot), up, sizeof up);
    scr_fill(y, 0, SC.w, ST(P_SOFT, P_PANEL, 0));
    x = scr_put(y, 0, SC.w, ST(P_BRIGHT, P_ACC_BG, A_BOLD), " " APP " ");
    snprintf(line, sizeof line, " %s \xc2\xb7 %s \xc2\xb7 macOS %s \xc2\xb7 up %s \xc2\xb7 load %.2f %.2f %.2f",
             S.host_name, S.model, S.osver, up, S.load[0], S.load[1], S.load[2]);
    rw = 22;
    if (!U.root && S.nlimited > 0) rw += 22;
    x += scr_put(y, x, SC.w - x - rw, ST(P_SOFT, P_PANEL, 0), line);
    x = SC.w - 11;
    scr_put(y, x + 1, 10, ST(P_TEXT, P_PANEL, A_BOLD), clk);
    x -= 9;
    if (U.paused) {
        scr_put(y, x, 9, ST(16, P_WARN, A_BOLD), " PAUSED ");
    } else {
        double pf = clampd((now - U.pulse_t) / 0.8, 0, 1);
        int col = pf < 0.3 ? 153 : pf < 0.6 ? P_ACCENT : P_BAR;
        scr_put(y, x, 2, ST(col, P_PANEL, A_BOLD), "\xe2\x97\x8f");
        scr_printf(y, x + 2, 7, ST(P_DIM, P_PANEL, 0), "%.1fs", U.interval);
    }
    if (!U.root && S.nlimited > 0) {
        x -= 22;
        scr_printf(y, x, 21, ST(P_WARN, P_PANEL, 0), "\xe2\x9a\xa0 %d hidden \xc2\xb7 S=sudo", S.nlimited);
    }
}

static int draw_cpu_meters(int y, int maxrows)
{
    int ncol, rows, i, cw;
    char v[16];
    if (S.ncpu <= 0) return y;
    ncol = SC.w / 30;
    if (ncol < 1) ncol = 1;
    if (ncol > 8) ncol = 8;
    if (ncol > S.ncpu) ncol = S.ncpu;
    rows = (S.ncpu + ncol - 1) / ncol;
    if (U.compact || rows > maxrows) {
        /* one line: a vertical level glyph per core */
        double tot = S.cpu_u + S.cpu_s;
        int x = 0, bw = S.ncpu * 2 <= SC.w - 20 ? 2 : 1;
        scr_put(y, 0, 4, ST(P_DIM, CDEF, A_BOLD), "CPU");
        x = 4;
        for (i = 0; i < S.ncpu && x + bw < SC.w - 12; i++) {
            double f = S.dcore_u[i] + S.dcore_s[i];
            int lv = (int)(clampd(f, 0, 1) * 8 + 0.5);
            set_cell(y, x, ST(grad_col(f), BAR_BG, 0), LEVEL[lv], lv ? 3 : 1, 1);
            if (bw == 2) set_cell(y, x + 1, ST(CDEF, CDEF, 0), " ", 1, 1);
            x += bw;
        }
        scr_printf(y, x + 1, 12, ST(cpu_val_col(tot * 100), CDEF, A_BOLD), "%5.1f%%", tot * 100);
        return y + 1;
    }
    cw = SC.w / ncol;
    for (i = 0; i < S.ncpu; i++) {
        int r = i % rows, c = i / rows;
        Seg sg[2];
        char lab[8];
        double f = S.dcore_u[i] + S.dcore_s[i];
        sg[0].v = S.dcore_u[i];
        sg[0].col = P_BAR;
        sg[1].v = S.dcore_s[i];
        sg[1].col = P_BAR2;
        snprintf(lab, sizeof lab, "%2d", i);
        snprintf(v, sizeof v, "%.1f%%", (S.core_u[i] + S.core_s[i]) * 100);
        meter(y + r, c * cw, cw - 1, lab, 2, sg, 2, v, cpu_val_col(f * 100));
    }
    return y + rows;
}

static void draw_sys_meters(int y, int two)
{
    char v[64], a[16], b[16];
    Seg sg[4];
    int cw = two ? SC.w / 2 : SC.w, x2 = two ? cw : 0, y2 = two ? y : y + 1;
    sg[0].v = S.d_mem;
    sg[0].col = P_BAR;
    sg[1].v = S.d_wired;
    sg[1].col = P_BAR3;
    sg[2].v = S.d_comp;
    sg[2].col = P_BAR2;
    sg[3].v = S.d_cached;
    sg[3].col = 239;
    fmt_mem(S.mem_used, a, sizeof a);
    fmt_mem(S.mem_total, b, sizeof b);
    snprintf(v, sizeof v, "%s/%s", a, b);
    meter(y, 0, cw - 1, "Mem", 3, sg, 4, v,
          S.pressure >= 4 ? P_HOT : S.pressure >= 2 ? P_WARN : P_TEXT);
    sg[0].v = S.d_swap;
    sg[0].col = P_BAR2;
    fmt_mem(S.swap_used, a, sizeof a);
    fmt_mem(S.swap_total, b, sizeof b);
    snprintf(v, sizeof v, "%s/%s", a, b);
    meter(y2, x2, cw - 1, "Swp", 3, sg, 1, v, S.swap_used ? P_TEXT : P_DIM);

    y = two ? y + 1 : y + 2;
    y2 = two ? y : y + 1;
    sg[0].v = S.d_gpu;
    sg[0].col = grad_col(S.d_gpu);
    if (S.has_gpu) {
        fmt_mem(S.gpu_mem, a, sizeof a);
        snprintf(v, sizeof v, "%3.0f%% %s", S.gpu_util * 100, a);
    } else {
        snprintf(v, sizeof v, "n/a");
    }
    meter(y, 0, cw - 1, "GPU", 3, sg, 1, v, cpu_val_col(S.gpu_util * 100));
    if (S.has_batt && S.batt_pct >= 0) {
        const char *st = S.charging ? "\xe2\x9a\xa1" : S.ac ? "\xe2\x8f\xbb" : "\xe2\x96\xbc";
        sg[0].v = S.d_batt;
        sg[0].col = S.batt_pct <= 15 ? P_HOT : S.batt_pct <= 35 ? P_WARN : P_BAR;
        snprintf(v, sizeof v, "%d%% %s", S.batt_pct, st);
        meter(y2, x2, cw - 1, "Bat", 3, sg, 1, v, S.batt_pct <= 35 ? sg[0].col : P_TEXT);
    } else {
        double mx = 0;
        int i;
        for (i = 0; i < S.hn; i++)
            if (S.h_pwr[(S.hpos - i + HIST) % HIST] > mx) mx = S.h_pwr[(S.hpos - i + HIST) % HIST];
        sg[0].v = mx > 0 ? S.proc_w / mx : 0;
        sg[0].col = P_BAR;
        fmt_power(S.proc_w, v, sizeof v);
        meter(y2, x2, cw - 1, "Pwr", 3, sg, 1, v, P_TEXT);
    }
}

static void draw_graphs(int y, int h)
{
    int gw = (SC.w - 2) / 3, i;
    double pmax = 1;
    char lab[48];
    for (i = 0; i < S.hn; i++) {
        float v = S.h_pwr[(S.hpos - i + HIST) % HIST];
        if (v > pmax) pmax = v;
    }
    for (i = 0; i < 3; i++) {
        int x = i * (gw + 1), lw;
        double cur;
        if (i == 0) { cur = S.cpu_u + S.cpu_s; snprintf(lab, sizeof lab, "CPU %.0f%%", cur * 100); }
        else if (i == 1) { cur = S.gpu_util; snprintf(lab, sizeof lab, "GPU %.0f%%", cur * 100); }
        else {
            char pw[16];
            fmt_power(S.sys_w >= 0 ? S.sys_w : S.proc_w, pw, sizeof pw);
            snprintf(lab, sizeof lab, "%s %s", S.sys_w >= 0 ? "System" : "Procs", pw);
        }
        lw = str_width(lab) + 1;
        if (lw > 16) lw = 16;
        scr_put(y + h - 1, x, lw, ST(P_DIM, CDEF, A_BOLD), lab);
        spark(y, x + lw, gw - lw, h, i == 0 ? S.h_cpu : i == 1 ? S.h_gpu : S.h_pwr, i == 2 ? pmax : 1.0);
    }
}

static void draw_info(int y)
{
    char line[512], a[24], b[24], c[24], d[24], thr[24];
    int x;
    fmt_rate(S.net_in_bps, a, sizeof a);
    fmt_rate(S.net_out_bps, b, sizeof b);
    fmt_rate(S.disk_rd_bps, c, sizeof c);
    fmt_rate(S.disk_wr_bps, d, sizeof d);
    fmt_count(S.nthreads, thr, sizeof thr);
    snprintf(line, sizeof line,
             "Tasks %d, %s thr, %d running \xe2\x94\x82 Net \xe2\x86\x93%s \xe2\x86\x91%s \xe2\x94\x82 Disk R %s W %s",
             S.nprocs, thr, S.nrunning, a, b, c, d);
    x = scr_put(y, 0, SC.w, ST(P_DIM, CDEF, 0), line);
    if (S.has_batt && x + 20 < SC.w) {
        char pw[16], st[48];
        Buf b = { 0 };
        fmt_power(S.sys_w, pw, sizeof pw);
        if (!S.ac && S.time_rem > 0 && S.time_rem < 65535)
            snprintf(st, sizeof st, "on battery, %d:%02d left", S.time_rem / 60, S.time_rem % 60);
        else if (S.ac && S.adapter_w > 0)
            snprintf(st, sizeof st, "%s, %dW adapter", S.charging ? "charging" : S.full ? "full" : "on AC", S.adapter_w);
        else
            snprintf(st, sizeof st, "%s", S.charging ? "charging" : S.ac ? "on AC" : "on battery");
        bprintf(&b, " \xe2\x94\x82 %s", st);
        if (S.sys_w >= 0) bprintf(&b, " \xc2\xb7 system %s", pw);
        if (S.cycles >= 0) bprintf(&b, " \xc2\xb7 %d cycles", S.cycles);
        if (S.batt_health >= 0) bprintf(&b, " \xc2\xb7 health %.0f%%", S.batt_health * 100);
        if (S.batt_temp > 0) bprintf(&b, " \xc2\xb7 %.1f\xc2\xb0" "C", S.batt_temp);
        scr_put(y, x, SC.w - x, ST(!S.ac && S.batt_pct <= 20 ? P_WARN : P_DIM, CDEF, 0), b.p);
        free(b.p);
    }
}

static int n_tagged(void);

static void draw_tabs(int y)
{
    int x = 0, i, cx = SC.w, n = 0, cols[4];
    char chips[4][160];
    scr_fill(y, 0, SC.w, ST(P_DIM, P_PANEL, 0));
    for (i = 0; i < NTABS; i++) {
        int on = U.sort == i;
        char t[32];
        snprintf(t, sizeof t, " %s%s ", SK_LABEL[i], on ? (U.desc ? " \xe2\x96\xbc" : " \xe2\x96\xb2") : "");
        U.tab_x[i] = x;
        x += scr_put(y, x, SC.w - x, on ? ST(P_BRIGHT, P_ACC_BG, A_BOLD) : ST(P_DIM, P_PANEL, 0), t);
        set_cell(y, x++, ST(P_FAINT, P_PANEL, 0), " ", 1, 1);
    }
    U.tab_x[NTABS] = x;
    if (U.sort >= NTABS)
        x += scr_printf(y, x, SC.w - x, ST(P_BRIGHT, P_ACC_BG, A_BOLD), " sort: %s %s ", SK_LABEL[U.sort],
                        U.desc ? "\xe2\x96\xbc" : "\xe2\x96\xb2") + 1;
    if (U.tree) { snprintf(chips[n], sizeof chips[n], " tree "); cols[n++] = P_SEL; }
    if (U.only_user) { snprintf(chips[n], sizeof chips[n], " user: %s ", user_name(U.user_uid)); cols[n++] = P_SEL; }
    if (U.filter[0]) { snprintf(chips[n], sizeof chips[n], " filter: %s ", U.filter); cols[n++] = P_SEL; }
    if ((i = n_tagged())) { snprintf(chips[n], sizeof chips[n], " %d tagged ", i); cols[n++] = P_SEL; }
    for (i = n - 1; i >= 0; i--) {
        int w = str_width(chips[i]);
        if (cx - w - 1 <= x) break;
        cx -= w + 1;
        scr_put(y, cx, w, ST(P_TEXT, cols[i], 0), chips[i]);
    }
}

static void layout_cols(int W)
{
    int i, x, need, best;
    for (i = 0; i < NCOLS; i++) U.col_on[i] = 1;
    U.barw = sk_bar(U.sort) ? (W >= 160 ? 14 : W >= 130 ? 10 : W >= 105 ? 6 : 0) : 0;
    for (;;) {
        need = 2 + 24 + (U.barw ? U.barw + 1 : 0);
        for (i = 0; i < NCOLS - 1; i++)
            if (U.col_on[i]) need += COLDEF[i].w + 1;
        if (need <= W) break;
        best = -1;
        for (i = 0; i < NCOLS - 1; i++)
            if (U.col_on[i] && COLDEF[i].drop < 99 && COLDEF[i].sk != U.sort &&
                (best < 0 || COLDEF[i].drop < COLDEF[best].drop))
                best = i;
        if (best < 0) {
            if (U.barw) { U.barw = 0; continue; }
            break;
        }
        U.col_on[best] = 0;
    }
    x = 2;
    for (i = 0; i < NCOLS - 1; i++) {
        U.col_w[i] = COLDEF[i].w;
        U.col_x[i] = x;
        if (!U.col_on[i]) continue;
        x += COLDEF[i].w + 1;
        if (COLDEF[i].sk == U.sort && U.barw) x += U.barw + 1;
    }
    U.col_x[COL_CMD] = x;
    U.col_w[COL_CMD] = W - x;
}

static void draw_colhdr(int y)
{
    int i;
    scr_fill(y, 0, SC.w, ST(P_DIM, P_PANEL, 0));
    for (i = 0; i < NCOLS; i++) {
        int on = COLDEF[i].sk == U.sort, w = U.col_w[i];
        char t[32];
        St s = on ? ST(P_BRIGHT, P_ACC_BG, A_BOLD) : ST(P_DIM, P_PANEL, A_BOLD);
        if (!U.col_on[i]) continue;
        if (on) {
            snprintf(t, sizeof t, "%s%s", COLDEF[i].name, U.desc ? "\xe2\x96\xbc" : "\xe2\x96\xb2");
            if (U.barw && i != COL_CMD) w += U.barw + 1;
        } else {
            snprintf(t, sizeof t, "%s", COLDEF[i].name);
        }
        if (i == COL_USER || i == COL_CMD || i == COL_S || (on && U.barw)) {
            scr_fill(y, U.col_x[i], w, s);
            scr_put(y, U.col_x[i], w, s, t);
        } else {
            scr_rput(y, U.col_x[i], w, s, t);
        }
    }
}

static void draw_row(int y, const Row *r, int selected, int ri, double now)
{
    const Proc *p = r->p;
    int bg = selected ? P_SEL : CDEF, i, x, w, dim = p->died > 0 || (U.tree && !p->match);
    int base = dim ? P_FAINT : P_SOFT;
    char t[64];
    double gleam = selected && now - U.sel_t < 1.4 ? now : -1;
    if (p->died > 0) {
        bg = selected ? 88 : 52;
    } else if (p->born > 0 && now - p->born < NEWBORN_SEC) {
        bg = selected ? 29 : now - p->born < NEWBORN_SEC * 0.6 ? 22 : 235;
    }
    scr_fill(y, 0, SC.w, ST(base, bg, 0));
    if (selected) scr_put(y, 0, 2, ST(P_ACCENT, bg, A_BOLD), "\xe2\x96\xb6");
    else if (p->tagged) scr_put(y, 0, 2, ST(P_WARN, bg, A_BOLD), "\xe2\x97\x8f");
    for (i = 0; i < NCOLS - 1; i++) {
        int col = base, at = 0;
        double v;
        if (!U.col_on[i]) continue;
        x = U.col_x[i];
        w = U.col_w[i];
        t[0] = 0;
        switch (i) {
        case COL_PID: snprintf(t, sizeof t, "%d", p->pid); col = P_DIM; break;
        case COL_USER:
            snprintf(t, sizeof t, "%s", user_name(p->uid));
            col = p->uid == U.me ? P_SOFT : P_DIM;
            break;
        case COL_PRI: if (p->has_task) snprintf(t, sizeof t, "%d", p->prio); else strcpy(t, "-"); col = P_DIM; break;
        case COL_NI:
            snprintf(t, sizeof t, "%d", p->nice);
            col = p->nice < 0 ? P_WARN : p->nice > 0 ? P_ACCENT : P_DIM;
            break;
        case COL_THR: if (p->has_task) snprintf(t, sizeof t, "%d", p->threads); else strcpy(t, "-"); col = P_DIM; break;
        case COL_S:
            t[0] = p->state;
            t[1] = 0;
            col = p->state == 'R' ? P_TEXT : p->state == 'Z' || p->state == 'X' ? P_HOT : p->state == 'T' ? P_WARN : P_FAINT;
            break;
        case COL_CPU:
            v = p->cpu;
            if (v < 0) strcpy(t, "-");
            else snprintf(t, sizeof t, v < 100 ? "%.1f" : "%.0f", v);
            col = cpu_val_col(v);
            at = v >= 50 ? A_BOLD : 0;
            break;
        case COL_GPU:
            v = p->gpu;
            if (v < 0) strcpy(t, "-");
            else snprintf(t, sizeof t, v < 100 ? "%.1f" : "%.0f", v);
            col = cpu_val_col(v);
            break;
        case COL_MEM:
            if (!p->has_task && !p->has_ru) { strcpy(t, "-"); col = P_FAINT; break; }
            fmt_mem(p->mem, t, sizeof t);
            col = p->mem >= (1ull << 30) ? P_TEXT : p->mem >= (1ull << 20) ? P_SOFT : P_DIM;
            break;
        case COL_POWER:
            fmt_power(p->power, t, sizeof t);
            col = p->power < 0.0005 ? P_FAINT : p->power >= 2 ? P_WARN : P_SOFT;
            break;
        case COL_DISK:
            if (p->disk_bps < 0) strcpy(t, "-");
            else if (p->disk_bps < 1) strcpy(t, "0");
            else fmt_rate(p->disk_bps, t, sizeof t);
            col = p->disk_bps >= 1 ? P_SOFT : P_FAINT;
            break;
        case COL_TIME:
            if (p->has_task || p->has_ru) fmt_cputime(p->c_cpu, t, sizeof t);
            else strcpy(t, "-");
            col = P_DIM;
            break;
        }
        if (dim) col = P_FAINT;
        if (i == COL_USER || i == COL_S) scr_put(y, x, w, ST(col, bg, at | (p->died ? A_STRIKE : 0)), t);
        else scr_rput(y, x, w, ST(col, bg, at), t);
        if (COLDEF[i].sk == U.sort && U.barw) {
            double f = p->dbar * row_ease(now, ri);
            heat_bar(y, x + w + 1, U.barw, f, gleam, bg);
        }
    }
    /* command */
    x = U.col_x[COL_CMD];
    w = U.col_w[COL_CMD];
    if (U.tree && r->depth > 0) {
        int d;
        for (d = 0; d < r->depth - 1 && w > 4; d++) {
            scr_put(y, x, 2, ST(P_GUIDE, bg, 0), (r->lines >> d) & 1 ? "\xe2\x94\x82 " : "  ");
            x += 2;
            w -= 2;
        }
        if (w > 4) {
            scr_put(y, x, 2, ST(P_GUIDE, bg, 0), r->last ? "\xe2\x94\x94\xe2\x94\x80" : "\xe2\x94\x9c\xe2\x94\x80");
            x += 2;
            w -= 2;
        }
    }
    if (U.tree && w > 4) {
        const char *m = r->kids ? (p->collapsed ? "\xe2\x8a\x95" : "\xe2\x94\xac") : "\xe2\x94\x80";
        if (r->depth > 0 || r->kids) {
            scr_put(y, x, 1, ST(p->collapsed && r->kids ? P_ACCENT : P_GUIDE, bg, A_BOLD), m);
            x += 1;
            w -= 1;
        }
        set_cell(y, x, ST(base, bg, 0), " ", 1, 1);
        x++;
        w--;
    }
    if (w <= 0) return;
    {
        int used, ncol = p->tagged ? P_WARN : dim ? P_DIM : p->uid == U.me ? P_BRIGHT : P_TEXT;
        const char *nm = U.fullpath && p->path ? p->path : p->name;
        used = scr_put(y, x, w, ST(ncol, bg, (dim ? 0 : A_BOLD) | (p->died ? A_STRIKE : 0)), nm);
        if (p->args && p->args[0] && used + 2 < w)
            scr_put(y, x + used + 1, w - used - 1, ST(P_FAINT, bg, 0), p->args);
    }
}

/* htop's F1..F10, bound to the plain digits 1..9, 0 (F-keys work too) */
static const struct { const char *k, *label; int key; } FOOT[] = {
    { "1", "Help", K_F(1) },   { "2", "Inspect", K_F(2) }, { "3", "Search", K_F(3) },
    { "4", "Filter", K_F(4) }, { "5", "Tree", K_F(5) },    { "6", "SortBy", K_F(6) },
    { "7", "Nice-", K_F(7) },  { "8", "Nice+", K_F(8) },   { "9", "Kill", K_F(9) },
    { "0", "Quit", K_F(10) }
};
#define NFOOT ((int)(sizeof FOOT / sizeof FOOT[0]))
static int g_foot_x[NFOOT + 1];

static void draw_footer(int y)
{
    int x = 0, i, total = 0, tight;
    scr_fill(y, 0, SC.w, ST(P_DIM, P_PANEL, 0));
    for (i = 0; i < NFOOT; i++) total += str_width(FOOT[i].k) + str_width(FOOT[i].label) + 3;
    tight = total > SC.w; /* narrow terminal: drop the padding so every key stays visible */
    for (i = 0; i < NFOOT; i++) {
        int w = str_width(FOOT[i].k) + str_width(FOOT[i].label) + (tight ? 1 : 3);
        g_foot_x[i] = x;
        if (x + w > SC.w) { g_foot_x[i] = SC.w; continue; }
        x += scr_printf(y, x, SC.w - x, ST(P_BRIGHT, 239, A_BOLD), tight ? "%s" : " %s ", FOOT[i].k);
        x += scr_put(y, x, SC.w - x, ST(P_SOFT, P_PANEL, 0), FOOT[i].label);
        if (x < SC.w) x++;
    }
    g_foot_x[NFOOT] = x;
}

static void draw_status(int y, double now)
{
    Proc *p = sel_proc();
    scr_fill(y, 0, SC.w, ST(CDEF, CDEF, 0));
    if (U.input_mode) {
        int x = scr_put(y, 0, SC.w, ST(P_BRIGHT, P_ACC_BG, A_BOLD), U.input_mode == 1 ? " Filter: " : " Search: ");
        x += scr_put(y, x + 1, SC.w - x - 2, ST(P_BRIGHT, CDEF, A_BOLD), U.input) + 1;
        set_cell(y, x, ST(16, fmod(now, 1.0) < 0.6 ? P_TEXT : P_FAINT, 0), " ", 1, 1);
        scr_put(y, x + 2, SC.w - x - 2, ST(P_FAINT, CDEF, 0),
                U.input_mode == 1 ? "  Enter keep \xc2\xb7 Esc clear" : "  \xe2\x86\x93 next match \xc2\xb7 Enter/Esc done");
    } else if (U.msg[0] && now - U.msg_t < 5.0) {
        scr_put(y, 1, SC.w - 1, ST(U.msg_err ? P_HOT : P_WARN, CDEF, 0), U.msg);
    } else if (p) {
        char line[2048];
        snprintf(line, sizeof line, "%s%s%s", p->path ? p->path : p->name, p->args && p->args[0] ? " " : "",
                 p->args ? p->args : "");
        scr_put(y, 1, SC.w - 1, ST(P_FAINT, CDEF, 0), line);
    }
}

static void draw_inspect(double now);

static void draw_main(double now)
{
    int y = 0, avail, two = SC.w >= 90, gh, i;
    if (SC.w < 50 || SC.h < 14) {
        scr_put(0, 0, SC.w, ST(P_WARN, CDEF, A_BOLD), "Terminal too small for " APP);
        return;
    }
    draw_title(y++, now);
    avail = SC.h - 1 - (two ? 2 : 4) - 1 - 2 - 2 - 8; /* keep >= 8 list rows */
    y = draw_cpu_meters(y, avail > 1 ? (avail < 8 ? avail : 8) : 0);
    draw_sys_meters(y, two);
    y += two ? 2 : 4;
    gh = U.compact ? 0 : SC.h - y >= 26 ? 2 : SC.h - y >= 20 ? 1 : 0;
    if (gh) {
        draw_graphs(y, gh);
        y += gh;
    }
    draw_info(y++);
    U.tab_y = y;
    draw_tabs(y++);
    layout_cols(SC.w);
    U.colhdr_y = y;
    draw_colhdr(y++);
    U.list_y = y;
    U.list_h = SC.h - 2 - y;
    if (U.list_h < 1) U.list_h = 1;
    clamp_scroll();
    for (i = 0; i < U.list_h; i++) {
        size_t idx = U.top + (size_t)i;
        if (idx < U.nrows) draw_row(y + i, &U.rows[idx], idx == U.sel, i, now);
        else if (i == 0 && !U.nrows)
            scr_put(y, 2, SC.w - 2, ST(242, CDEF, A_ITAL), U.filter[0] ? "(no process matches the filter)" : "(no processes)");
    }
    /* scrollbar */
    if (U.nrows > (size_t)U.list_h) {
        int th = (int)((double)U.list_h * U.list_h / (double)U.nrows), ty;
        if (th < 1) th = 1;
        ty = (int)((double)U.top / (double)(U.nrows - (size_t)U.list_h) * (U.list_h - th) + 0.5);
        for (i = 0; i < U.list_h; i++)
            set_cell(y + i, SC.w - 1, ST(i >= ty && i < ty + th ? 244 : 237, CDEF, 0),
                     i >= ty && i < ty + th ? "\xe2\x94\x83" : "\xe2\x94\x82", 3, 1);
    }
    draw_status(SC.h - 2, now);
    draw_footer(SC.h - 1);
    draw_inspect(now);
}

/* ------------------------------------------------------------------ */
/* inspect view: line model                                            */
/* ------------------------------------------------------------------ */

enum { L_HEAD, L_KV, L_TEXT, L_SPARK, L_WARN, L_GAP };
typedef struct { int kind, col; char *k, *v; } ILine;
typedef struct { ILine *v; int n, cap; } ILines;

static void il_add(ILines *L, int kind, int col, const char *k, const char *fmt, ...)
{
    va_list ap;
    ILine *l;
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 64;
        L->v = xrealloc(L->v, (size_t)L->cap * sizeof *L->v);
    }
    l = &L->v[L->n++];
    l->kind = kind;
    l->col = col;
    l->k = k ? xstrdup(k) : NULL;
    l->v = NULL;
    if (fmt) {
        va_start(ap, fmt);
        if (vasprintf(&l->v, fmt, ap) < 0) l->v = NULL;
        va_end(ap);
    }
}

static void il_free(ILines *L)
{
    int i;
    for (i = 0; i < L->n; i++) {
        free(L->v[i].k);
        free(L->v[i].v);
    }
    free(L->v);
    memset(L, 0, sizeof *L);
}

enum { IT_OVERVIEW, IT_FILES, IT_ENV, IT_SIGN, IT_N };
static const char *IT_LABEL[IT_N] = { "Overview", "Files & Network", "Arguments & Env", "Code Signing" };

static struct {
    int on, tab, scroll, total, view_h;
    pid_t pid;
    long start_s, start_us;
    double t0;
    char name[96];
    ILines lines;
    unsigned built_gen;
    int built_tab;
    int tab_x[IT_N + 1], tab_y;
    pthread_mutex_t mu;
    unsigned cs_gen;
    int cs_state; /* 0 idle, 1 running, 2 done */
    double cs_t0;
    ILines cs;
} IN = { .mu = PTHREAD_MUTEX_INITIALIZER };

/* The inspected process (live or fading); NULL once fully gone. */
static Proc *in_proc(void)
{
    size_t i;
    Proc *p = IN.pid >= 0 && IN.pid < PID_CAP ? bypid[IN.pid] : NULL;
    if (p && p->start_s == IN.start_s && p->start_us == IN.start_us) return p;
    for (i = 0; i < PT.n; i++) {
        p = PT.v[i];
        if (p->pid == IN.pid && p->start_s == IN.start_s && p->start_us == IN.start_us) return p;
    }
    return NULL;
}

static const char *state_name(char s)
{
    switch (s) {
    case 'R': return "running";
    case 'S': return "sleeping";
    case 'T': return "stopped (paused)";
    case 'Z': return "zombie (exited, waiting for parent)";
    case 'I': return "starting";
    case 'X': return "exited";
    default: return "unknown (not permitted)";
    }
}

static void build_overview(ILines *L, Proc *p)
{
    char a[64], b[64], c[64], tbuf[64];
    struct proc_vnodepathinfo vp;
    time_t st = (time_t)p->start_s;
    struct tm tm;
    double age = difftime(time(NULL), st);
    Proc *q, *chain[64];
    int n = 0, i, bg;
    size_t k;
    const char *app;
    Buf bb = { 0 };

    il_add(L, L_HEAD, 0, NULL, "Process");
    il_add(L, L_KV, 255, "Name", "%s", p->name);
    il_add(L, L_KV, 252, "PID", "%d", p->pid);
    q = parent_of(p);
    il_add(L, L_KV, 252, "Parent", "%d  %s", p->ppid, q ? q->name : p->ppid == 0 ? "(kernel)" : "?");
    il_add(L, L_KV, P_TEXT, "User", "%s (uid %u)", user_name(p->uid), (unsigned)p->uid);
    il_add(L, L_KV, p->state == 'Z' || p->state == 'X' ? P_HOT : p->state == 'T' ? P_WARN : P_TEXT, "State", "%s",
           state_name(p->state));
    localtime_r(&st, &tm);
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", &tm);
    fmt_dur(age, a, sizeof a);
    il_add(L, L_KV, 252, "Started", "%s  (%s ago)", tbuf, a);
    il_add(L, L_KV, P_TEXT, "Executable", "%s", p->path ? p->path : "(unknown)");
    if (p->path && (app = strstr(p->path, ".app/"))) {
        il_add(L, L_KV, P_TEXT, "App bundle", "%.*s", (int)(app - p->path) + 4, p->path);
    }
    il_add(L, L_KV, 245, "Arguments", "%s", p->args ? (p->args[0] ? p->args : "(none)") : "(not permitted)");
    if (proc_pidinfo(p->pid, PROC_PIDVNODEPATHINFO, 0, &vp, sizeof vp) == (int)sizeof vp)
        il_add(L, L_KV, 252, "Working dir", "%s", vp.pvi_cdir.vip_path);

    il_add(L, L_GAP, 0, NULL, NULL);
    il_add(L, L_HEAD, 0, NULL, "Resources");
    if (p->cpu >= 0) {
        fmt_cputime(p->c_cpu, a, sizeof a);
        il_add(L, L_KV, cpu_val_col(p->cpu), "CPU", "%.1f%%   (total CPU time %s)", p->cpu, a);
        il_add(L, L_SPARK, 0, "CPU history", NULL);
    } else {
        il_add(L, L_KV, 240, "CPU", "not permitted (run with sudo to see)");
    }
    if (p->has_task) il_add(L, L_KV, 252, "Threads", "%d  (%d running)", p->threads, p->nrun);
    errno = 0;
    bg = getpriority(PRIO_DARWIN_PROCESS, p->pid);
    il_add(L, L_KV, p->nice < 0 ? P_WARN : P_TEXT, "Priority", "%s%d  (nice %d)%s",
           p->has_task ? "" : "?", p->has_task ? p->prio : 0, p->nice,
           errno == 0 && bg > 0 ? "   \xc2\xb7 background (Darwin BG) throttled" : "");
    if (p->has_ru) {
        fmt_mem(p->mem, a, sizeof a);
        fmt_mem(p->peak, b, sizeof b);
        il_add(L, L_KV, 252, "Memory", "%s footprint   (peak %s)", a, b);
    }
    if (p->has_task) {
        fmt_mem(p->res, a, sizeof a);
        fmt_mem(p->virt, b, sizeof b);
        il_add(L, L_KV, 252, "Resident", "%s   (virtual %s)", a, b);
    }
    fmt_cputime(p->gpu_total, a, sizeof a);
    il_add(L, L_KV, cpu_val_col(p->gpu), "GPU", "%.1f%%   (total GPU time %s)", p->gpu < 0 ? 0 : p->gpu, a);
    if (p->has_ru) {
        fmt_power(p->power, a, sizeof a);
        fmt_power(age > 0 ? (double)p->energy_total / 1e9 / age : 0, b, sizeof b);
        il_add(L, L_KV, P_TEXT, "Power (CPU)", "%s now   (avg %s since launch, %.1f J total)", a, b,
               (double)p->energy_total / 1e9);
        fmt_mem(p->disk_r, a, sizeof a);
        fmt_mem(p->disk_w, b, sizeof b);
        fmt_rate(p->disk_bps, c, sizeof c);
        il_add(L, L_KV, 252, "Disk I/O", "read %s \xc2\xb7 written %s \xc2\xb7 now %s", a, b, c);
        il_add(L, L_KV, 252, "Wakeups", "%.1f / s", p->wake_ps < 0 ? 0 : p->wake_ps);
        if (p->cycles) il_add(L, L_KV, 252, "IPC", "%.2f instructions/cycle", (double)p->instr / (double)p->cycles);
        il_add(L, L_KV, 252, "Page-ins", "%llu", (unsigned long long)p->pageins);
    } else {
        il_add(L, L_WARN, P_WARN, NULL, "Memory, power, disk and wakeup details of other users' processes need root (press S).");
    }

    il_add(L, L_GAP, 0, NULL, NULL);
    il_add(L, L_HEAD, 0, NULL, "Ancestry");
    for (q = p; q && n < 64; q = parent_of(q)) {
        chain[n++] = q;
        if (q->pid <= 1) break;
    }
    for (i = n - 1; i >= 0; i--) {
        bprintf(&bb, "%s (%d)", chain[i]->name, chain[i]->pid);
        if (i) bstr(&bb, "  \xe2\x86\x92  ");
    }
    il_add(L, L_TEXT, 252, NULL, "%s", bb.p ? bb.p : "");
    free(bb.p);

    for (n = 0, k = 0; k < PT.n; k++)
        if (!PT.v[k]->died && PT.v[k]->ppid == p->pid && PT.v[k] != p) n++;
    il_add(L, L_GAP, 0, NULL, NULL);
    il_add(L, L_HEAD, 0, NULL, "Children (%d)", n);
    for (k = 0; k < PT.n; k++) {
        q = PT.v[k];
        if (q->died || q->ppid != p->pid || q == p) continue;
        if (q->cpu >= 0) snprintf(a, sizeof a, "%5.1f%% CPU", q->cpu);
        else snprintf(a, sizeof a, "      -    ");
        il_add(L, L_TEXT, 250, NULL, "%7d  %s  %s", q->pid, a, q->name);
    }
}

static const char *TCPST[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "CLOSE_WAIT",
                               "FIN_WAIT_1", "CLOSING", "LAST_ACK", "FIN_WAIT_2", "TIME_WAIT" };

static void fmt_inaddr(const struct in_sockinfo *in, int local, char *o, size_t sz)
{
    char a[INET6_ADDRSTRLEN] = "?";
    int port = ntohs((uint16_t)(local ? in->insi_lport : in->insi_fport));
    if (in->insi_vflag & INI_IPV4)
        inet_ntop(AF_INET, local ? (const void *)&in->insi_laddr.ina_46.i46a_addr4 : (const void *)&in->insi_faddr.ina_46.i46a_addr4, a, sizeof a);
    else
        inet_ntop(AF_INET6, local ? (const void *)&in->insi_laddr.ina_6 : (const void *)&in->insi_faddr.ina_6, a, sizeof a);
    if (in->insi_vflag & INI_IPV4) snprintf(o, sz, "%s:%d", a, port);
    else snprintf(o, sz, "[%s]:%d", a, port);
    if (!port && !local) snprintf(o, sz, "*");
}

static void build_files(ILines *L, pid_t pid)
{
    int sz = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, NULL, 0), n, i;
    struct proc_fdinfo *fds;
    ILines net = { 0 }, files = { 0 }, other = { 0 };
    if (sz <= 0) {
        il_add(L, L_WARN, P_WARN, NULL, "Open files are only visible for your own processes (or as root: press S).");
        return;
    }
    sz += 32 * (int)sizeof *fds;
    fds = xmalloc((size_t)sz);
    n = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds, sz) / (int)sizeof *fds;
    for (i = 0; i < n; i++) {
        int fd = fds[i].proc_fd;
        switch (fds[i].proc_fdtype) {
        case PROX_FDTYPE_VNODE: {
            struct vnode_fdinfowithpath vi;
            if (proc_pidfdinfo(pid, fd, PROC_PIDFDVNODEPATHINFO, &vi, sizeof vi) == (int)sizeof vi) {
                char m[3] = "--";
                if (vi.pfi.fi_openflags & FREAD) m[0] = 'r';
                if (vi.pfi.fi_openflags & FWRITE) m[1] = 'w';
                il_add(&files, L_TEXT, 252, NULL, "%5d %s  %s", fd, m, vi.pvip.vip_path[0] ? vi.pvip.vip_path : "(anonymous)");
            }
            break;
        }
        case PROX_FDTYPE_SOCKET: {
            struct socket_fdinfo si;
            char la[96], fa[96];
            if (proc_pidfdinfo(pid, fd, PROC_PIDFDSOCKETINFO, &si, sizeof si) != (int)sizeof si) break;
            if (si.psi.soi_kind == SOCKINFO_TCP) {
                const struct in_sockinfo *in = &si.psi.soi_proto.pri_tcp.tcpsi_ini;
                int s = si.psi.soi_proto.pri_tcp.tcpsi_state;
                fmt_inaddr(in, 1, la, sizeof la);
                fmt_inaddr(in, 0, fa, sizeof fa);
                il_add(&net, L_TEXT, s == 4 ? P_TEXT : P_DIM, NULL, "%5d TCP  %-28s \xe2\x86\x92 %-28s %s", fd, la,
                       s == 1 ? "" : fa, s >= 0 && s <= 10 ? TCPST[s] : "?");
            } else if (si.psi.soi_kind == SOCKINFO_IN) {
                const struct in_sockinfo *in = &si.psi.soi_proto.pri_in;
                fmt_inaddr(in, 1, la, sizeof la);
                fmt_inaddr(in, 0, fa, sizeof fa);
                il_add(&net, L_TEXT, P_DIM, NULL, "%5d UDP  %-28s \xe2\x86\x92 %s", fd, la, fa);
            } else if (si.psi.soi_kind == SOCKINFO_UN) {
                const char *path = si.psi.soi_proto.pri_un.unsi_addr.ua_sun.sun_path;
                il_add(&other, L_TEXT, 245, NULL, "%5d unix %s", fd, path[0] ? path : "(unnamed)");
            } else {
                il_add(&other, L_TEXT, 245, NULL, "%5d socket (family %d)", fd, si.psi.soi_family);
            }
            break;
        }
        case PROX_FDTYPE_PIPE: il_add(&other, L_TEXT, 245, NULL, "%5d pipe", fd); break;
        case PROX_FDTYPE_KQUEUE: il_add(&other, L_TEXT, 245, NULL, "%5d kqueue", fd); break;
        case PROX_FDTYPE_PSHM: il_add(&other, L_TEXT, 245, NULL, "%5d POSIX shared memory", fd); break;
        case PROX_FDTYPE_PSEM: il_add(&other, L_TEXT, 245, NULL, "%5d POSIX semaphore", fd); break;
        case PROX_FDTYPE_FSEVENTS: il_add(&other, L_TEXT, 245, NULL, "%5d fsevents", fd); break;
        default: il_add(&other, L_TEXT, 245, NULL, "%5d type %u", fd, fds[i].proc_fdtype); break;
        }
    }
    free(fds);
    il_add(L, L_HEAD, 0, NULL, "Network (%d)", net.n);
    for (i = 0; i < net.n; i++) il_add(L, net.v[i].kind, net.v[i].col, NULL, "%s", net.v[i].v);
    il_add(L, L_GAP, 0, NULL, NULL);
    il_add(L, L_HEAD, 0, NULL, "Files (%d)", files.n);
    for (i = 0; i < files.n; i++) il_add(L, files.v[i].kind, files.v[i].col, NULL, "%s", files.v[i].v);
    il_add(L, L_GAP, 0, NULL, NULL);
    il_add(L, L_HEAD, 0, NULL, "Other descriptors (%d)", other.n);
    for (i = 0; i < other.n; i++) il_add(L, other.v[i].kind, other.v[i].col, NULL, "%s", other.v[i].v);
    il_free(&net);
    il_free(&files);
    il_free(&other);
}

static void build_env(ILines *L, pid_t pid)
{
    PArgs a;
    int i;
    if (!pargs_get(pid, &a)) {
        il_add(L, L_WARN, P_WARN, NULL, "macOS only exposes arguments and environment of your own processes (or as root: press S).");
        return;
    }
    il_add(L, L_KV, P_TEXT, "exec path", "%s", a.exe);
    il_add(L, L_GAP, 0, NULL, NULL);
    il_add(L, L_HEAD, 0, NULL, "Arguments (%d)", a.argc);
    for (i = 0; i < a.argc; i++) {
        char k[16];
        snprintf(k, sizeof k, "argv[%d]", i);
        il_add(L, L_KV, 252, k, "%s", a.argv[i]);
    }
    il_add(L, L_GAP, 0, NULL, NULL);
    il_add(L, L_HEAD, 0, NULL, "Environment (%d)", a.nenv);
    for (i = 0; i < a.nenv; i++) {
        const char *eq = strchr(a.env[i], '=');
        char k[64];
        if (eq && eq - a.env[i] < (long)sizeof k) {
            snprintf(k, sizeof k, "%.*s", (int)(eq - a.env[i]), a.env[i]);
            il_add(L, L_KV, 250, k, "%s", eq + 1);
        } else {
            il_add(L, L_TEXT, 250, NULL, "%s", a.env[i]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* code signing (worker thread)                                        */
/* ------------------------------------------------------------------ */

enum { CS_OPS_STATUS = 0, CS_OPS_CDHASH = 5, CS_OPS_IDENTITY = 11, CS_OPS_TEAMID = 14 };
static const struct { uint32_t f; const char *n; } CSF[] = {
    { 0x1, "valid" }, { 0x2, "adhoc" }, { 0x4, "get-task-allow" }, { 0x8, "installer" },
    { 0x10, "forced-lv" }, { 0x20, "invalid-allowed" }, { 0x100, "hard" }, { 0x200, "kill" },
    { 0x400, "check-expiration" }, { 0x800, "restrict" }, { 0x1000, "enforcement" },
    { 0x2000, "require-lv" }, { 0x4000, "entitlements-validated" }, { 0x8000, "nvram-unrestricted" },
    { 0x10000, "runtime" }, { 0x20000, "linker-signed" }, { 0x1000000, "killed" },
    { 0x2000000, "dyld-platform" }, { 0x4000000, "platform-binary" }, { 0x8000000, "platform-path" },
    { 0x10000000, "debugged" }, { 0x20000000, "signed" }, { 0x40000000, "dev-code" },
    { 0x80000000u, "datavault-controller" }
};

static void flags_str(uint32_t f, char *o, size_t sz)
{
    size_t i;
    Buf b = { 0 };
    for (i = 0; i < sizeof CSF / sizeof CSF[0]; i++)
        if (f & CSF[i].f) {
            if (b.n) bstr(&b, ", ");
            bstr(&b, CSF[i].n);
        }
    snprintf(o, sz, "0x%08x  %s", f, b.p ? b.p : "(none)");
    free(b.p);
}

static void csops_str(pid_t pid, unsigned op, char *o, size_t sz)
{
    char buf[1100];
    uint32_t len;
    o[0] = 0;
    memset(buf, 0, sizeof buf);
    if (csops(pid, op, buf, sizeof buf - 1) != 0) return;
    memcpy(&len, buf + 4, 4);
    len = ntohl(len);
    if (len <= 8 || len > sizeof buf - 1) return;
    snprintf(o, sz, "%s", buf + 8);
}

static void cf_desc(CFTypeRef v, char *o, size_t sz)
{
    CFTypeID t;
    o[0] = 0;
    if (!v) return;
    t = CFGetTypeID(v);
    if (t == CFBooleanGetTypeID()) snprintf(o, sz, "%s", CFBooleanGetValue(v) ? "true" : "false");
    else if (t == CFStringGetTypeID()) cfstr_get(v, o, sz);
    else if (t == CFNumberGetTypeID()) snprintf(o, sz, "%lld", (long long)cf_i64(v, 0));
    else if (t == CFArrayGetTypeID()) {
        CFIndex i, n = CFArrayGetCount(v);
        size_t used = 0;
        used += (size_t)snprintf(o, sz, "[");
        for (i = 0; i < n && used + 8 < sz; i++) {
            char e[256];
            cf_desc(CFArrayGetValueAtIndex(v, i), e, sizeof e);
            used += (size_t)snprintf(o + used, sz - used, "%s%s", i ? ", " : "", e);
        }
        if (used + 2 < sz) snprintf(o + used, sz - used, "]");
    } else if (t == CFDictionaryGetTypeID()) snprintf(o, sz, "{%ld keys}", (long)CFDictionaryGetCount(v));
    else if (t == CFDataGetTypeID()) {
        CFIndex i, n = CFDataGetLength(v);
        const UInt8 *d = CFDataGetBytePtr(v);
        size_t used = 0;
        for (i = 0; i < n && used + 3 < sz; i++) used += (size_t)snprintf(o + used, sz - used, "%02x", d[i]);
    } else if (t == CFDateGetTypeID()) {
        time_t tt = (time_t)(CFDateGetAbsoluteTime(v) + kCFAbsoluteTimeIntervalSince1970);
        struct tm tm;
        localtime_r(&tt, &tm);
        strftime(o, sz, "%Y-%m-%d %H:%M:%S", &tm);
    }
}

static int req_ok(SecStaticCodeRef code, const char *req)
{
    CFStringRef s = CFStringCreateWithCString(NULL, req, kCFStringEncodingUTF8);
    SecRequirementRef r = NULL;
    int ok = 0;
    if (SecRequirementCreateWithString(s, kSecCSDefaultFlags, &r) == errSecSuccess) {
        ok = SecStaticCodeCheckValidity(code, kSecCSBasicValidateOnly, r) == errSecSuccess;
        CFRelease(r);
    }
    CFRelease(s);
    return ok;
}

typedef struct { unsigned gen; pid_t pid; char path[PATH_MAX]; } CsJob;

static void ent_add(const void *key, const void *val, void *ctx)
{
    char k[256], v[512];
    cfstr_get(key, k, sizeof k);
    cf_desc(val, v, sizeof v);
    il_add(ctx, L_TEXT, 250, NULL, "%s = %s", k, v);
}

static void build_codesign(ILines *L, pid_t pid, const char *path)
{
    uint32_t fl = 0;
    char s[1024], t[1024];
    unsigned char cd[20];
    SecStaticCodeRef code = NULL;
    CFDictionaryRef info = NULL;
    CFURLRef url;
    OSStatus st;
    int i;

    il_add(L, L_HEAD, 0, NULL, "Running process (kernel view)");
    if (csops(pid, CS_OPS_STATUS, &fl, sizeof fl) == 0) {
        il_add(L, L_KV, (fl & 0x1) ? P_OK : P_HOT, "Status", "%s%s", (fl & 0x1) ? "valid" : "INVALID",
               (fl & 0x20000000) ? "" : " (unsigned)");
        flags_str(fl, s, sizeof s);
        il_add(L, L_KV, P_DIM, "Flags", "%s", s);
        il_add(L, L_KV, P_TEXT, "Platform", "%s",
               (fl & 0x4000000) ? "yes - part of macOS (platform binary)" : "no - third-party code");
        il_add(L, L_KV, (fl & 0x10000) ? P_TEXT : P_WARN, "Hardened rt", "%s", (fl & 0x10000) ? "yes" : "no");
        il_add(L, L_KV, (fl & 0x4) ? P_WARN : P_TEXT, "Debuggable", "%s", (fl & 0x4) ? "yes (get-task-allow)" : "no");
        csops_str(pid, CS_OPS_IDENTITY, s, sizeof s);
        if (s[0]) il_add(L, L_KV, 255, "Identifier", "%s", s);
        csops_str(pid, CS_OPS_TEAMID, s, sizeof s);
        il_add(L, L_KV, 252, "Team ID", "%s", s[0] ? s : "(none)");
        if (csops(pid, CS_OPS_CDHASH, cd, sizeof cd) == 0) {
            for (i = 0; i < 20; i++) snprintf(s + i * 2, 3, "%02x", cd[i]);
            il_add(L, L_KV, 245, "CDHash", "%s", s);
        }
    } else {
        il_add(L, L_WARN, P_WARN, NULL, "csops failed: %s", strerror(errno));
    }

    il_add(L, L_GAP, 0, NULL, NULL);
    il_add(L, L_HEAD, 0, NULL, "Signature on disk");
    if (!path || !path[0]) {
        il_add(L, L_WARN, P_WARN, NULL, "Executable path unknown.");
        return;
    }
    il_add(L, L_KV, P_TEXT, "Path", "%s", path);
    url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    st = url ? SecStaticCodeCreateWithPath(url, kSecCSDefaultFlags, &code) : errSecParam;
    if (url) CFRelease(url);
    if (st != errSecSuccess || !code) {
        il_add(L, L_WARN, P_HOT, NULL, "Cannot open code object (OSStatus %d).", (int)st);
        return;
    }
    st = SecCodeCopySigningInformation(code, kSecCSSigningInformation | kSecCSRequirementInformation, &info);
    if (st == errSecSuccess && info) {
        CFTypeRef v;
        CFArrayRef certs;
        CFDictionaryRef ents, pl;
        int adhoc;
        const char *kind;
        int col;
        cf_desc(CFDictionaryGetValue(info, kSecCodeInfoIdentifier), s, sizeof s);
        if (!s[0]) {
            il_add(L, L_KV, P_HOT, "Signed", "NO - this executable is not code-signed");
        } else {
            uint32_t sflags = (uint32_t)cf_i64(CFDictionaryGetValue(info, kSecCodeInfoFlags), 0);
            adhoc = (sflags & 0x2) != 0;
            if (req_ok(code, "anchor apple")) { kind = "Apple (system software)"; col = 78; }
            else if (req_ok(code, "anchor apple generic and certificate leaf[field.1.2.840.113635.100.6.1.9]")) { kind = "Mac App Store"; col = 78; }
            else if (req_ok(code, "anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.6] and certificate leaf[field.1.2.840.113635.100.6.1.13]")) { kind = "Developer ID"; col = 78; }
            else if (req_ok(code, "anchor apple generic")) { kind = "Apple-issued development certificate"; col = 221; }
            else if (adhoc) { kind = "ad-hoc (no identity, local build)"; col = 221; }
            else { kind = "untrusted / self-signed certificate"; col = 203; }
            il_add(L, L_KV, col, "Signed by", "%s", kind);
            il_add(L, L_KV, 255, "Identifier", "%s", s);
            cf_desc(CFDictionaryGetValue(info, kSecCodeInfoTeamIdentifier), t, sizeof t);
            il_add(L, L_KV, 252, "Team ID", "%s", t[0] ? t : "(none)");
            cf_desc(CFDictionaryGetValue(info, kSecCodeInfoFormat), t, sizeof t);
            il_add(L, L_KV, 252, "Format", "%s", t);
            certs = CFDictionaryGetValue(info, kSecCodeInfoCertificates);
            if (certs && CFGetTypeID(certs) == CFArrayGetTypeID()) {
                CFIndex n = CFArrayGetCount(certs), j;
                for (j = 0; j < n; j++) {
                    CFStringRef sum = SecCertificateCopySubjectSummary((SecCertificateRef)CFArrayGetValueAtIndex(certs, j));
                    cfstr_get(sum, t, sizeof t);
                    if (sum) CFRelease(sum);
                    il_add(L, L_KV, 252, j == 0 ? "Authority" : "", "%s%s", j ? "\xe2\x86\xb3 " : "", t);
                }
            }
            if ((v = CFDictionaryGetValue(info, kSecCodeInfoTimestamp)) || (v = CFDictionaryGetValue(info, kSecCodeInfoTime))) {
                cf_desc(v, t, sizeof t);
                il_add(L, L_KV, 252, "Signed at", "%s", t);
            }
            cf_desc(CFDictionaryGetValue(info, kSecCodeInfoUnique), t, sizeof t);
            if (t[0]) il_add(L, L_KV, 245, "CDHash", "%s", t);
            flags_str(sflags, t, sizeof t);
            il_add(L, L_KV, 250, "Sig flags", "%s", t);
            if ((v = CFDictionaryGetValue(info, kSecCodeInfoDesignatedRequirement))) {
                CFStringRef rs = NULL;
                if (SecRequirementCopyString((SecRequirementRef)v, kSecCSDefaultFlags, &rs) == errSecSuccess && rs) {
                    cfstr_get(rs, t, sizeof t);
                    CFRelease(rs);
                    il_add(L, L_KV, 245, "Requirement", "%s", t);
                }
            }
            pl = CFDictionaryGetValue(info, kSecCodeInfoPList);
            if (pl && CFGetTypeID(pl) == CFDictionaryGetTypeID()) {
                char ver[128], bv[128];
                cf_desc(CFDictionaryGetValue(pl, CFSTR("CFBundleIdentifier")), t, sizeof t);
                cf_desc(CFDictionaryGetValue(pl, CFSTR("CFBundleShortVersionString")), ver, sizeof ver);
                cf_desc(CFDictionaryGetValue(pl, CFSTR("CFBundleVersion")), bv, sizeof bv);
                if (t[0]) il_add(L, L_KV, P_TEXT, "Bundle", "%s  %s%s%s%s", t, ver, bv[0] ? " (build " : "", bv, bv[0] ? ")" : "");
            }
            st = SecStaticCodeCheckValidity(code, kSecCSDoNotValidateResources, NULL);
            if (st == errSecSuccess) {
                il_add(L, L_KV, P_OK, "Verification", "valid on disk (executable hashes match)");
            } else {
                CFStringRef em = SecCopyErrorMessageString(st, NULL);
                cfstr_get(em, t, sizeof t);
                if (em) CFRelease(em);
                il_add(L, L_KV, P_HOT, "Verification", "FAILED: %s (%d)", t, (int)st);
            }
            if (!strcmp(kind, "Developer ID"))
                il_add(L, L_KV, req_ok(code, "notarized") ? P_OK : P_WARN, "Notarized", "%s",
                       req_ok(code, "notarized") ? "yes" : "no (or ticket not available offline)");
            ents = CFDictionaryGetValue(info, kSecCodeInfoEntitlementsDict);
            il_add(L, L_GAP, 0, NULL, NULL);
            il_add(L, L_HEAD, 0, NULL, "Entitlements (%ld)",
                   ents && CFGetTypeID(ents) == CFDictionaryGetTypeID() ? (long)CFDictionaryGetCount(ents) : 0L);
            if (ents && CFGetTypeID(ents) == CFDictionaryGetTypeID()) CFDictionaryApplyFunction(ents, ent_add, L);
        }
        CFRelease(info);
    } else {
        il_add(L, L_WARN, P_HOT, NULL, "Cannot read signing information (OSStatus %d).", (int)st);
    }
    CFRelease(code);
}

static void *cs_thread(void *arg)
{
    CsJob *j = arg;
    ILines L = { 0 };
    build_codesign(&L, j->pid, j->path);
    pthread_mutex_lock(&IN.mu);
    if (j->gen == IN.cs_gen) {
        il_free(&IN.cs);
        IN.cs = L;
        IN.cs_state = 2;
    } else {
        il_free(&L);
    }
    pthread_mutex_unlock(&IN.mu);
    free(j);
    return NULL;
}

static void cs_start(Proc *p)
{
    CsJob *j = xcalloc(1, sizeof *j);
    pthread_t th;
    pthread_attr_t at;
    pthread_mutex_lock(&IN.mu);
    j->gen = ++IN.cs_gen;
    il_free(&IN.cs);
    IN.cs_state = 1;
    IN.cs_t0 = now_s();
    pthread_mutex_unlock(&IN.mu);
    j->pid = p->pid;
    if (p->path) snprintf(j->path, sizeof j->path, "%s", p->path);
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, cs_thread, j) != 0) {
        free(j);
        IN.cs_state = 0;
    }
    pthread_attr_destroy(&at);
}

/* ------------------------------------------------------------------ */
/* inspect view: drawing                                               */
/* ------------------------------------------------------------------ */

static void inspect_open(Proc *p)
{
    if (!p) return;
    memset(&IN.lines, 0, sizeof IN.lines);
    IN.on = 1;
    IN.pid = p->pid;
    IN.start_s = p->start_s;
    IN.start_us = p->start_us;
    snprintf(IN.name, sizeof IN.name, "%s", p->name);
    IN.t0 = now_s();
    IN.tab = IT_OVERVIEW;
    IN.scroll = 0;
    IN.built_gen = 0;
    IN.built_tab = -1;
    cs_start(p);
}

static void inspect_close(void)
{
    IN.on = 0;
    il_free(&IN.lines);
    pthread_mutex_lock(&IN.mu);
    IN.cs_gen++;
    il_free(&IN.cs);
    IN.cs_state = 0;
    pthread_mutex_unlock(&IN.mu);
}

static void inspect_build(void)
{
    Proc *p = in_proc();
    if (IN.tab == IT_SIGN) return;
    if (IN.built_tab == IN.tab && (IN.tab == IT_ENV || IN.built_gen == PT.gen || !p || p->died)) return;
    if (!p && IN.built_tab == IN.tab) return;
    il_free(&IN.lines);
    IN.built_tab = IN.tab;
    IN.built_gen = PT.gen;
    if (!p) {
        il_add(&IN.lines, L_WARN, P_HOT, NULL, "Process %d has exited.", IN.pid);
        return;
    }
    if (IN.tab == IT_OVERVIEW) build_overview(&IN.lines, p);
    else if (IN.tab == IT_FILES) build_files(&IN.lines, p->pid);
    else build_env(&IN.lines, p->pid);
}

/* Byte offset after skipping `cols` display columns. */
static size_t skip_cols(const char *s, int cols)
{
    size_t len = strlen(s), i = 0;
    int used = 0;
    while (i < len) {
        Glyph g = glyph(s + i, len - i);
        int w = g.bad ? 1 : g.w;
        if (used + w > cols) break;
        used += w;
        i += (size_t)g.n;
    }
    return i;
}

static int line_rows(const ILine *l, int w, int kw)
{
    int avail = l->kind == L_KV ? w - kw : w - 2, sw;
    if (l->kind == L_SPARK) return 3;
    if (!l->v || avail <= 4 || l->kind == L_HEAD || l->kind == L_GAP) return 1;
    sw = str_width(l->v);
    sw = (sw + avail - 1) / avail;
    if (sw < 1) sw = 1;
    return sw > 8 ? 8 : sw;
}

static void draw_iline(int y, int x, int w, int kw, const ILine *l, int sub, Proc *p)
{
    const char *v = l->v ? l->v : "";
    int avail;
    switch (l->kind) {
    case L_HEAD: {
        int u = scr_put(y, x, w, ST(P_ACCENT, P_BOX, A_BOLD), v), i;
        for (i = u + 1; i < w; i++) set_cell(y, x + i, ST(P_GUIDE, P_BOX, 0), "\xe2\x94\x80", 3, 1);
        return;
    }
    case L_GAP: return;
    case L_SPARK: {
        int i, hw = w - kw, r;
        double mx = 100;
        if (sub == 0) scr_put(y, x, kw, ST(245, 234, 0), l->k);
        if (!p || hw < 4) return;
        for (i = 0; i < PHIST && i < p->hn; i++)
            if (p->hist[i] > mx) mx = p->hist[i];
        for (i = 0; i < hw; i++) {
            int age = hw - 1 - i;
            double f = age < p->hn && age < PHIST ? p->hist[(p->hn - 1 - age) % PHIST] / mx : -1;
            double lv = f < 0 ? 0 : clampd(f * 24 - (double)(2 - sub) * 8, 0, 8);
            r = (int)(lv + 0.5);
            if (f < 0) set_cell(y, x + kw + i, ST(238, 234, 0), sub == 2 ? "\xc2\xb7" : " ", sub == 2 ? 2 : 1, 1);
            else set_cell(y, x + kw + i, ST(grad_col(f), 234, 0), LEVEL[r], r ? 3 : 1, 1);
        }
        return;
    }
    case L_KV:
        if (sub == 0 && l->k) scr_put(y, x, kw - 1, ST(245, 234, 0), l->k);
        x += kw;
        avail = w - kw;
        break;
    case L_WARN:
        x += 2;
        avail = w - 2;
        break;
    default:
        x += 2;
        avail = w - 2;
    }
    if (avail <= 0) return;
    v += skip_cols(v, avail * sub);
    scr_put2(y, x, avail, ST(l->col ? l->col : 252, 234, l->kind == L_WARN ? A_ITAL : 0), v,
             sub == 7);
}

static void draw_inspect(double now)
{
    int bx = 0, by = 1, bw = SC.w, bh = SC.h - 2, w, h, x, y, i, row, kw = 15;
    double e;
    char title[160];
    Proc *p;
    ILines *L;
    int locked = 0;
    if (!IN.on) return;
    e = ease_out((now - IN.t0) / 0.22);
    w = (int)(24 + (bw - 24) * e);
    h = (int)(5 + (bh - 5) * e);
    x = bx + (bw - w) / 2;
    y = by + (bh - h) / 2;
    p = in_proc();
    snprintf(title, sizeof title, "Inspect \xc2\xb7 %s (%d)", IN.name, IN.pid);
    draw_box(y, x, w, h, P_ACCENT, title);
    if (e < 1 || h < 8 || w < 40) return;
    /* status chip */
    {
        const char *st = !p ? " gone " : p->died ? " exited " : p->state == 'T' ? " paused " : " live ";
        int col = !p || p->died ? P_HOT : p->state == 'T' ? P_WARN : P_OK, sw = str_width(st);
        scr_put(y, x + w - sw - 3, sw, ST(col, P_BOX, A_BOLD), st);
    }
    /* tabs */
    IN.tab_y = y + 1;
    {
        int tx = x + 2;
        for (i = 0; i < IT_N; i++) {
            char t[48];
            snprintf(t, sizeof t, " %s ", IT_LABEL[i]);
            IN.tab_x[i] = tx;
            tx += scr_put(y + 1, tx, x + w - 2 - tx, i == IN.tab ? ST(P_BRIGHT, P_ACC_BG, A_BOLD) : ST(P_DIM, P_SEL, 0), t) + 1;
        }
        IN.tab_x[IT_N] = tx;
    }
    inspect_build();
    if (IN.tab == IT_SIGN) {
        pthread_mutex_lock(&IN.mu);
        locked = 1;
        L = &IN.cs;
        if (IN.cs_state == 1) {
            int cy = y + 4, cw = w - 8;
            double t = now - IN.cs_t0;
            scr_printf(cy, x + 3, w - 6, ST(P_ACCENT, P_BOX, A_BOLD), "%s Verifying code signature\xe2\x80\xa6",
                       SPIN[(int)(now * 12) % 10]);
            for (i = 0; i < cw; i++) {
                double ph = fmod(t * 0.9, 1.6) - 0.3, d = fabs((double)i / cw - ph);
                int col = d < 0.04 ? 153 : d < 0.10 ? P_ACCENT : d < 0.18 ? P_BAR : P_GUIDE;
                set_cell(cy + 2, x + 4 + i, ST(col, 234, 0), "\xe2\x96\x80", 3, 1);
            }
            pthread_mutex_unlock(&IN.mu);
            locked = 0;
            L = NULL;
        }
    } else {
        L = &IN.lines;
    }
    /* content */
    IN.view_h = h - 5;
    if (L) {
        int cw = w - 4, total = 0, skip;
        for (i = 0; i < L->n; i++) total += line_rows(&L->v[i], cw, kw);
        IN.total = total;
        if (IN.scroll > total - IN.view_h) IN.scroll = total - IN.view_h;
        if (IN.scroll < 0) IN.scroll = 0;
        skip = IN.scroll;
        row = 0;
        for (i = 0; i < L->n && row < IN.view_h; i++) {
            int nr = line_rows(&L->v[i], cw, kw), s;
            for (s = 0; s < nr && row < IN.view_h; s++) {
                if (skip > 0) { skip--; continue; }
                draw_iline(y + 3 + row, x + 2, cw, kw, &L->v[i], s, p);
                row++;
            }
        }
        if (total > IN.view_h) {
            int th = IN.view_h * IN.view_h / total, ty;
            if (th < 1) th = 1;
            ty = (int)((double)IN.scroll / (double)(total - IN.view_h) * (IN.view_h - th) + 0.5);
            for (i = 0; i < IN.view_h; i++)
                set_cell(y + 3 + i, x + w - 2, ST(i >= ty && i < ty + th ? 75 : 237, 234, 0),
                         i >= ty && i < ty + th ? "\xe2\x94\x83" : "\xe2\x94\x82", 3, 1);
        }
    }
    if (locked) pthread_mutex_unlock(&IN.mu);
    scr_put(y + h - 2, x + 2, w - 4, ST(P_FAINT, P_BOX, 0),
            "\xe2\x86\x90\xe2\x86\x92 Tab switch tab \xc2\xb7 \xe2\x86\x91\xe2\x86\x93 scroll \xc2\xb7 9/k signal \xc2\xb7 n priority "
            "\xc2\xb7 7/8 nice \xc2\xb7 o reveal in Finder \xc2\xb7 y copy path \xc2\xb7 r refresh \xc2\xb7 Esc/2 close");
}

/* ------------------------------------------------------------------ */
/* frame loop                                                          */
/* ------------------------------------------------------------------ */

static int g_anim, g_soon;

static void update_size(void)
{
    struct winsize ws;
    int w = 80, h = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        w = ws.ws_col;
        h = ws.ws_row;
    }
    if (w != SC.w || h != SC.h || !SC.cur) scr_resize(w, h);
}

static void ease(double *d, double target, double k)
{
    *d += (target - *d) * k;
    if (fabs(target - *d) < 0.002) *d = target;
    else g_anim = 1;
}

static void tick(double now)
{
    double dt = U.last_frame > 0 ? now - U.last_frame : 1, k = 1 - exp(-clampd(dt, 0, 1) * 11.0);
    double mt = S.mem_total ? (double)S.mem_total : 1;
    size_t i;
    int j;
    U.last_frame = now;
    g_anim = g_soon = 0;
    if (!U.paused && now >= U.next_sample) {
        sample_all(now, NULL);
        U.next_sample = now + U.interval;
        U.pulse_t = now;
        rebuild();
    }
    if (procs_expire(now)) rebuild();
    for (j = 0; j < S.ncpu; j++) {
        ease(&S.dcore_u[j], S.core_u[j], k);
        ease(&S.dcore_s[j], S.core_s[j], k);
    }
    ease(&S.d_mem, (double)S.mem_app / mt, k);
    ease(&S.d_wired, (double)S.mem_wired / mt, k);
    ease(&S.d_comp, (double)S.mem_comp / mt, k);
    ease(&S.d_cached, (double)S.mem_cached / mt, k);
    ease(&S.d_swap, S.swap_total ? (double)S.swap_used / (double)S.swap_total : 0, k);
    ease(&S.d_gpu, S.gpu_util, k);
    ease(&S.d_batt, S.batt_pct > 0 ? S.batt_pct / 100.0 : 0, k);
    for (i = U.top; i < U.nrows && i < U.top + (size_t)(U.list_h > 0 ? U.list_h : 1); i++) {
        Proc *p = U.rows[i].p;
        ease(&p->dbar, bar_frac(p), k);
        if (p->died > 0 || (p->born > 0 && now - p->born < NEWBORN_SEC)) g_soon = 1;
    }
    if (now - U.anim_t0 < ROW_ANIM + ROW_STAGGER * U.list_h + 0.05 || now - U.sel_t < 1.4 ||
        now - U.pulse_t < 0.8 || (IN.on && now - IN.t0 < 0.3) || (IN.on && IN.cs_state == 1) || U.input_mode)
        g_anim = 1;
}

static void draw_help(void);

static void frame(void (*overlay)(void *), void *ctx)
{
    double now = now_s();
    if (g_winch) {
        g_winch = 0;
        update_size();
    }
    tick(now);
    scr_clear();
    draw_main(now);
    if (U.help) draw_help();
    if (overlay) overlay(ctx);
    scr_flush();
}

static int next_key(void)
{
    double now = now_s(), wait;
    struct timeval tv;
    if (g_anim) return read_key(16);
    gettimeofday(&tv, NULL);
    wait = (1e6 - tv.tv_usec) / 1e6 + 0.005; /* clock tick */
    if (!U.paused && U.next_sample - now < wait) wait = U.next_sample - now;
    if (g_soon && wait > 0.1) wait = 0.1;
    return read_key(wait <= 0 ? 0 : (int)(wait * 1000));
}

/* ------------------------------------------------------------------ */
/* modal dialogs                                                       */
/* ------------------------------------------------------------------ */

typedef struct { const char *label, *desc; } MItem;
typedef struct {
    const char *title;
    const char **pre;
    int npre;
    const int *precol;
    const MItem *it;
    int n, sel, col;
    const char *toggle;
    int tog;
    const char *foot;
    int bx, by, bw, iy; /* layout for mouse */
} Menu;

static void menu_draw(void *ctx)
{
    Menu *m = ctx;
    int w = str_width(m->title) + 10, h, x, y, i, lw = 0, row;
    for (i = 0; i < m->npre; i++)
        if (str_width(m->pre[i]) + 6 > w) w = str_width(m->pre[i]) + 6;
    for (i = 0; i < m->n; i++)
        if (str_width(m->it[i].label) > lw) lw = str_width(m->it[i].label);
    for (i = 0; i < m->n; i++) {
        int iw = 10 + lw + (m->it[i].desc ? str_width(m->it[i].desc) + 2 : 0);
        if (iw > w) w = iw;
    }
    if (m->foot && str_width(m->foot) + 6 > w) w = str_width(m->foot) + 6;
    if (w > SC.w - 4) w = SC.w - 4;
    h = 2 + m->npre + (m->npre ? 1 : 0) + m->n + (m->toggle ? 2 : 0) + (m->foot ? 2 : 0);
    if (h > SC.h - 2) h = SC.h - 2;
    x = (SC.w - w) / 2;
    y = (SC.h - h) / 2;
    draw_box(y, x, w, h, m->col, m->title);
    m->bx = x;
    m->by = y;
    m->bw = w;
    row = y + 1;
    for (i = 0; i < m->npre && row < y + h - 1; i++, row++)
        scr_put(row, x + 2, w - 4, ST(m->precol ? m->precol[i] : 252, 234, i == 0 ? A_BOLD : 0), m->pre[i]);
    if (m->npre) row++;
    m->iy = row;
    for (i = 0; i < m->n && row < y + h - 1; i++, row++) {
        int on = i == m->sel, u;
        St s = on ? ST(P_BRIGHT, P_ACC_BG, A_BOLD) : ST(P_TEXT, P_BOX, 0);
        scr_fill(row, x + 1, w - 2, on ? ST(P_BRIGHT, P_ACC_BG, 0) : ST(P_TEXT, P_BOX, 0));
        scr_printf(row, x + 2, 4, on ? s : ST(242, 234, 0), "%c", i < 9 ? '1' + i : ' ');
        u = scr_put(row, x + 4, lw + 1, s, m->it[i].label);
        (void)u;
        if (m->it[i].desc)
            scr_put(row, x + 6 + lw, w - 8 - lw, on ? ST(P_SOFT, P_ACC_BG, 0) : ST(P_DIM, P_BOX, 0), m->it[i].desc);
    }
    if (m->toggle) {
        row++;
        scr_printf(row, x + 2, w - 4, ST(m->tog ? P_WARN : P_DIM, P_BOX, m->tog ? A_BOLD : 0), "[%s] %s",
                   m->tog ? "x" : " ", m->toggle);
        row++;
    }
    if (m->foot) scr_put(y + h - 2, x + 2, w - 4, ST(242, 234, 0), m->foot);
}

static int menu_run(Menu *m)
{
    int k, r = -1;
    U.nmenu++;
    for (;;) {
        frame(menu_draw, m);
        k = next_key();
        if (k == K_NONE) continue;
        if (k == K_UP) m->sel = (m->sel + m->n - 1) % m->n;
        else if (k == K_DOWN || k == '\t') m->sel = (m->sel + 1) % m->n;
        else if (k == K_HOME) m->sel = 0;
        else if (k == K_END) m->sel = m->n - 1;
        else if (k >= '1' && k <= '9' && k - '1' < m->n) { r = k - '1'; m->sel = r; break; }
        else if (k == '\r' || k == '\n') { r = m->sel; break; }
        else if (k == 'c' && m->toggle) m->tog = !m->tog;
        else if (k == K_MOUSE) {
            int i = MS.y - m->iy;
            if (MS.b == 64) m->sel = (m->sel + m->n - 1) % m->n;
            else if (MS.b == 65) m->sel = (m->sel + 1) % m->n;
            else if (MS.press && MS.b == 0 && MS.x > m->bx && MS.x < m->bx + m->bw - 1 && i >= 0 && i < m->n) {
                r = m->sel = i;
                break;
            } else if (MS.press && MS.b == 0 && (MS.x < m->bx || MS.x >= m->bx + m->bw)) break;
        } else if (k == K_ESC || k == 'q' || k == 3 || k == K_F(10)) break;
    }
    U.nmenu--;
    return r;
}

typedef struct { const char *title; const char **lines; int n, col; } Confirm;

static void confirm_draw(void *ctx)
{
    Confirm *c = ctx;
    int w = str_width(c->title) + 10, h = c->n + 4, x, y, i;
    for (i = 0; i < c->n; i++)
        if (str_width(c->lines[i]) + 6 > w) w = str_width(c->lines[i]) + 6;
    if (w < 44) w = 44;
    if (w > SC.w - 4) w = SC.w - 4;
    x = (SC.w - w) / 2;
    y = (SC.h - h) / 2;
    draw_box(y, x, w, h, c->col, c->title);
    for (i = 0; i < c->n; i++) scr_put(y + 1 + i, x + 2, w - 4, ST(252, 234, i == 0 ? A_BOLD : 0), c->lines[i]);
    scr_put(y + h - 2, x + 2, w - 4, ST(c->col, 234, A_BOLD), "[y] yes    [any other key] no");
}

static int confirm(const char *title, const char **lines, int n, int col)
{
    Confirm c;
    int k;
    c.title = title;
    c.lines = lines;
    c.n = n;
    c.col = col;
    U.nmenu++;
    for (;;) {
        frame(confirm_draw, &c);
        k = next_key();
        if (k == K_MOUSE && !MS.press) continue;
        if (k != K_NONE) break;
    }
    U.nmenu--;
    return k == 'y' || k == 'Y';
}

static void draw_help(void)
{
    static const char *lines[] = {
        "\xe2\x86\x91\xe2\x86\x93 PgUp PgDn Home End   move          Space   tag (U untag all, c tag with children)",
        "Enter 2 i          inspect: info, files & sockets, args & env, code signature",
        "Tab Shift-Tab      sort tab: CPU GPU Memory Energy Disk   6 all sort keys   I invert",
        "P M G B D N T      sort by CPU, Memory, GPU, Battery/energy, Disk, PID, Time",
        "5 t                tree view (parent + children); \xe2\x86\x90/\xe2\x86\x92 or -/+ collapse/expand, * all",
        "4 / \\              filter (name, args, path, user, pid)    3  search and jump",
        "9 k Del            send signal (terminate, kill, pause, resume, \xe2\x80\xa6), optionally to the whole tree",
        "n                  priority menu: nice level or Darwin background (throttled) mode",
        "7 ]  /  8 [        raise / lower priority one nice step (raising needs root: sudo is offered)",
        "u                  only my processes \xe2\x86\x94 everyone      p   show full executable path",
        "o / y              reveal executable in Finder / copy its path",
        "z                  pause updates     , .  slower / faster refresh     m  compact header",
        "S                  restart mhtop with sudo (see every process)",
        "Mouse              click select, double-click inspect, right-click actions, wheel scroll,",
        "                   click tabs / column headers to sort, click the bottom key bar",
        "q 0                quit           (digits 1-9, 0 = htop's F1-F10; F-keys work too)",
        "",
        "MEM is the physical footprint (as Activity Monitor). POWER is measured CPU energy (Apple",
        "silicon, ri_energy_nj). GPU% comes from per-client GPU time. Other users' processes need root",
        "for CPU/memory/power. Press any key to close."
    };
    int n = (int)(sizeof lines / sizeof lines[0]), w = 0, i, x, y;
    for (i = 0; i < n; i++)
        if (str_width(lines[i]) > w) w = str_width(lines[i]);
    w += 4;
    if (w > SC.w - 2) w = SC.w - 2;
    if (n + 2 > SC.h) n = SC.h - 2;
    x = (SC.w - w) / 2;
    y = (SC.h - n - 2) / 2;
    draw_box(y, x, w, n + 2, P_ACCENT, APP " " VERSION " \xc2\xb7 help");
    for (i = 0; i < n; i++) scr_put(y + 1 + i, x + 2, w - 4, ST(i >= n - 4 ? 245 : 252, 234, 0), lines[i]);
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */

/* Runs argv on the real terminal (cooked mode, outside the UI). */
static int run_tty(char *const argv[], const char *banner)
{
    pid_t pid;
    int st = -1, i;
    posix_spawnattr_t at;
    sigset_t def;
    struct sigaction ign, oi, oq, ot;
    term_restore();
    if (banner) fprintf(stderr, "%s\n", banner);
    fprintf(stderr, APP ": running:");
    for (i = 0; argv[i]; i++) fprintf(stderr, " %s", argv[i]);
    fprintf(stderr, "\n");
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ign, &oi);
    sigaction(SIGQUIT, &ign, &oq);
    sigaction(SIGTERM, &ign, &ot);
    posix_spawnattr_init(&at);
    sigemptyset(&def);
    sigaddset(&def, SIGINT);
    sigaddset(&def, SIGQUIT);
    sigaddset(&def, SIGTERM);
    posix_spawnattr_setsigdefault(&at, &def);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGDEF);
    if (posix_spawn(&pid, argv[0], NULL, &at, argv, environ) == 0) {
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
            ;
    }
    posix_spawnattr_destroy(&at);
    sigaction(SIGINT, &oi, NULL);
    sigaction(SIGQUIT, &oq, NULL);
    sigaction(SIGTERM, &ot, NULL);
    term_raw();
    update_size();
    return st != -1 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int run_sudo(char **cmd)
{
    char *av[64];
    int n = 0, i;
    av[n++] = "/usr/bin/sudo";
    av[n++] = "-p";
    av[n++] = "[" APP "] administrator password for %u: ";
    for (i = 0; cmd[i] && n < 63; i++) av[n++] = cmd[i];
    av[n] = NULL;
    return run_tty(av, NULL);
}

static void relaunch_sudo(void)
{
    char exe[PATH_MAX];
    uint32_t sz = sizeof exe;
    char **av;
    int i, rc;
    const char *lines[] = { "Restart mhtop as root via sudo?",
                            "Root can read CPU, memory, power, files and code signing",
                            "details of every process, and change any priority." };
    if (U.root) { set_msg(0, "Already running as root"); return; }
    if (!confirm("Restart with sudo", lines, 3, P_WARN)) return;
    if (_NSGetExecutablePath(exe, &sz) != 0) { set_msg(1, "Cannot determine executable path"); return; }
    av = xcalloc((size_t)U.argc + 2, sizeof *av);
    av[0] = exe;
    for (i = 1; i < U.argc; i++) av[i] = U.argv[i];
    rc = run_sudo(av);
    free(av);
    if (rc == 0) exit(0); /* the root instance ran and quit normally */
    set_msg(1, "sudo failed or was cancelled (exit %d)", rc);
}

typedef struct { pid_t *v; int n, cap; } PidList;

static int pl_has(const PidList *l, pid_t p)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (l->v[i] == p) return 1;
    return 0;
}

static void pl_add(PidList *l, pid_t p)
{
    if (pl_has(l, p)) return;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = xrealloc(l->v, (size_t)l->cap * sizeof *l->v);
    }
    l->v[l->n++] = p;
}

static int n_tagged(void)
{
    size_t i;
    int n = 0;
    for (i = 0; i < PT.n; i++)
        if (PT.v[i]->tagged && !PT.v[i]->died) n++;
    return n;
}

/* single != NULL: that process only; otherwise tagged ones or the selection. */
static void collect_targets(PidList *l, Proc *single, int with_kids)
{
    size_t i;
    int grew;
    if (single) {
        pl_add(l, single->pid);
    } else if (n_tagged()) {
        for (i = 0; i < PT.n; i++)
            if (PT.v[i]->tagged && !PT.v[i]->died) pl_add(l, PT.v[i]->pid);
    } else if (sel_proc() && !sel_proc()->died) {
        pl_add(l, sel_proc()->pid);
    }
    if (!with_kids) return;
    do {
        grew = 0;
        for (i = 0; i < PT.n; i++) {
            Proc *p = PT.v[i];
            if (!p->died && p->pid > 1 && !pl_has(l, p->pid) && pl_has(l, p->ppid)) {
                pl_add(l, p->pid);
                grew = 1;
            }
        }
    } while (grew);
}

static const char *pname(pid_t pid)
{
    Proc *p = pid >= 0 && pid < PID_CAP ? bypid[pid] : NULL;
    return p ? p->name : "?";
}

static void target_desc(const PidList *l, char *o, size_t sz)
{
    if (l->n == 1) snprintf(o, sz, "PID %d \xc2\xb7 %s", l->v[0], pname(l->v[0]));
    else snprintf(o, sz, "%d processes (%s, %s%s)", l->n, pname(l->v[0]), pname(l->v[1]), l->n > 2 ? ", \xe2\x80\xa6" : "");
}

/* Removes pids that must never be signalled from here. */
static void pl_guard(PidList *l)
{
    int i, j = 0;
    for (i = 0; i < l->n; i++)
        if (l->v[i] > 1 && l->v[i] != getpid()) l->v[j++] = l->v[i];
    if (j != l->n) set_msg(1, "Skipped kernel_task / launchd / mhtop itself");
    l->n = j;
}

static const struct { int sig; const char *name, *desc; } SIGS[] = {
    { SIGTERM, "SIGTERM", "Terminate - ask the process to quit cleanly" },
    { SIGKILL, "SIGKILL", "Kill - force quit immediately (cannot be caught)" },
    { SIGINT, "SIGINT", "Interrupt - like pressing Ctrl-C" },
    { SIGHUP, "SIGHUP", "Hang up - many daemons reload their config" },
    { SIGSTOP, "SIGSTOP", "Pause - freeze the process" },
    { SIGCONT, "SIGCONT", "Resume - continue a paused process" },
    { SIGQUIT, "SIGQUIT", "Quit and dump core" },
    { SIGUSR1, "SIGUSR1", "User signal 1 (app-defined)" },
    { SIGUSR2, "SIGUSR2", "User signal 2 (app-defined)" },
};
#define NSIGS ((int)(sizeof SIGS / sizeof SIGS[0]))

static void send_signal(PidList *l, int si)
{
    int i, ok = 0, denied = 0, gone = 0;
    PidList fail = { 0 };
    pl_guard(l);
    for (i = l->n - 1; i >= 0; i--) { /* children before parents */
        if (kill(l->v[i], SIGS[si].sig) == 0) ok++;
        else if (errno == EPERM) { denied++; pl_add(&fail, l->v[i]); }
        else gone++;
    }
    if (denied) {
        char line[160], sig[16];
        const char *lines[3];
        lines[0] = "Permission denied.";
        snprintf(line, sizeof line, "%d process(es) belong to another user or are protected.", denied);
        lines[1] = line;
        lines[2] = "Retry with administrator rights (sudo kill)?";
        if (confirm("Send signal", lines, 3, P_HOT)) {
            char **cmd = xcalloc((size_t)fail.n + 4, sizeof *cmd);
            int n = 0, rc;
            snprintf(sig, sizeof sig, "-%d", SIGS[si].sig);
            cmd[n++] = "/bin/kill";
            cmd[n++] = sig;
            for (i = 0; i < fail.n; i++) cmd[n++] = xasprintf("%d", fail.v[i]);
            rc = run_sudo(cmd);
            for (i = 2; i < n; i++) free(cmd[i]);
            free(cmd);
            if (rc == 0) { ok += denied; denied = 0; }
        }
    }
    free(fail.v);
    if (denied) set_msg(1, "%s: %d sent, %d denied (System Integrity Protection or another user)", SIGS[si].name, ok, denied);
    else if (ok) set_msg(0, "Sent %s to %d process%s%s", SIGS[si].name, ok, ok == 1 ? "" : "es", gone ? " (some already gone)" : "");
    else if (gone) set_msg(1, "Process already exited");
    U.next_sample = now_s() + 0.2; /* show the result quickly */
}

static void kill_menu(Proc *single)
{
    PidList l = { 0 };
    MItem it[NSIGS];
    Menu m;
    char desc[200];
    const char *pre[3];
    int precol[3] = { P_BRIGHT, P_DIM, P_HOT }, i, r, np = 1;
    Proc *p;
    collect_targets(&l, single, 0);
    if (!l.n) return;
    for (i = 0; i < NSIGS; i++) {
        it[i].label = SIGS[i].name;
        it[i].desc = SIGS[i].desc;
    }
    target_desc(&l, desc, sizeof desc);
    pre[0] = desc;
    p = bypid[l.v[0]];
    if (l.n == 1 && p) {
        pre[np++] = p->path ? p->path : p->name;
    }
    if (p && (p->uid == 0 || (p->path && !strncmp(p->path, "/System/", 8))))
        pre[np++] = "\xe2\x9a\xa0 system process - stopping it may destabilise macOS";
    else
        precol[np] = 245;
    memset(&m, 0, sizeof m);
    m.title = "Send signal";
    m.pre = pre;
    m.npre = np;
    m.precol = precol;
    m.it = it;
    m.n = NSIGS;
    m.col = P_HOT;
    m.toggle = "also signal all child processes (whole tree)  - press c";
    m.foot = "Enter send \xc2\xb7 1-9 quick pick \xc2\xb7 Esc cancel";
    r = menu_run(&m);
    if (r >= 0) {
        if (m.tog) {
            free(l.v);
            memset(&l, 0, sizeof l);
            collect_targets(&l, single, 1);
        }
        send_signal(&l, r);
    }
    free(l.v);
}

static int is_perm(int e) { return e == EPERM || e == EACCES; }

/* nice[i] for l->v[i]; falls back to sudo renice when not permitted. */
static void apply_nice(PidList *l, const int *nice)
{
    int i, ok = 0, denied = 0;
    PidList fail = { 0 };
    int fnice[256];
    for (i = 0; i < l->n; i++) {
        if (setpriority(PRIO_PROCESS, l->v[i], nice[i]) == 0) ok++;
        else if (is_perm(errno) && fail.n < 256) { fnice[fail.n] = nice[i]; pl_add(&fail, l->v[i]); denied++; }
    }
    if (denied) {
        const char *lines[3];
        char line[160];
        lines[0] = "Permission denied.";
        snprintf(line, sizeof line, "Raising priority (lower nice) or touching another user's process needs root.");
        lines[1] = line;
        lines[2] = "Retry with administrator rights (sudo renice)?";
        if (confirm("Change priority", lines, 3, P_WARN)) {
            for (i = 0; i < fail.n; i++) {
                char nb[16], pb[16];
                char *cmd[] = { "/usr/bin/renice", nb, "-p", pb, NULL };
                snprintf(nb, sizeof nb, "%d", fnice[i]);
                snprintf(pb, sizeof pb, "%d", fail.v[i]);
                if (run_sudo(cmd) == 0) { ok++; denied--; }
            }
        }
    }
    free(fail.v);
    if (denied) set_msg(1, "Priority: %d changed, %d denied", ok, denied);
    else if (ok == 1 && l->n == 1) set_msg(0, "PID %d (%s) is now nice %d", l->v[0], pname(l->v[0]), nice[0]);
    else if (ok) set_msg(0, "Priority changed for %d processes", ok);
    U.next_sample = now_s() + 0.15;
}

static void apply_background(PidList *l, int on)
{
    int i, ok = 0, denied = 0;
    PidList fail = { 0 };
    for (i = 0; i < l->n; i++) {
        if (setpriority(PRIO_DARWIN_PROCESS, l->v[i], on ? PRIO_DARWIN_BG : 0) == 0) ok++;
        else if (is_perm(errno)) { pl_add(&fail, l->v[i]); denied++; }
    }
    if (denied) {
        const char *lines[] = { "Permission denied.", "Changing another user's process needs root.",
                                "Retry with administrator rights (sudo taskpolicy)?" };
        if (confirm("Background mode", lines, 3, P_WARN)) {
            for (i = 0; i < fail.n; i++) {
                char pb[16];
                char *cmd[] = { "/usr/sbin/taskpolicy", on ? "-b" : "-B", "-p", pb, NULL };
                snprintf(pb, sizeof pb, "%d", fail.v[i]);
                if (run_sudo(cmd) == 0) { ok++; denied--; }
            }
        }
    }
    free(fail.v);
    if (denied) set_msg(1, "Background mode: %d changed, %d denied", ok, denied);
    else if (ok) set_msg(0, "%s background (throttled) mode for %d process%s", on ? "Enabled" : "Disabled", ok, ok == 1 ? "" : "es");
}

static void priority_menu(Proc *single)
{
    static const int NV[] = { -20, -10, -5, 0, 5, 10, 20 };
    static const MItem it[] = {
        { "Highest", "nice -20  (needs root)" },
        { "High", "nice -10  (needs root)" },
        { "Above normal", "nice -5   (needs root)" },
        { "Normal", "nice 0" },
        { "Below normal", "nice 5" },
        { "Low", "nice 10" },
        { "Lowest", "nice 20" },
        { "Background", "Darwin BG: throttle CPU, disk and network (taskpolicy -b)" },
        { "Foreground", "leave Darwin background mode" },
    };
    PidList l = { 0 };
    Menu m;
    char desc[200], cur[160];
    const char *pre[2];
    int i, r, best = 3, nice[256];
    Proc *p;
    collect_targets(&l, single, 0);
    if (!l.n) return;
    p = bypid[l.v[0]];
    target_desc(&l, desc, sizeof desc);
    pre[0] = desc;
    if (p) {
        int bg;
        errno = 0;
        bg = getpriority(PRIO_DARWIN_PROCESS, p->pid);
        snprintf(cur, sizeof cur, "current: nice %d%s%s", p->nice,
                 p->has_task ? ", scheduler priority " : "", errno == 0 && bg > 0 ? ", background" : "");
        if (p->has_task) snprintf(cur + strlen(cur), sizeof cur - strlen(cur), "%d", p->prio);
        for (i = 0; i < 7; i++)
            if (abs(NV[i] - p->nice) < abs(NV[best] - p->nice)) best = i;
    } else {
        snprintf(cur, sizeof cur, " ");
    }
    pre[1] = cur;
    memset(&m, 0, sizeof m);
    m.title = "Process priority";
    m.pre = pre;
    m.npre = 2;
    m.it = it;
    m.n = (int)(sizeof it / sizeof it[0]);
    m.sel = best;
    m.col = P_ACCENT;
    m.foot = "Lower nice = more CPU time. Enter apply \xc2\xb7 Esc cancel";
    r = menu_run(&m);
    if (r >= 0 && r < 7) {
        for (i = 0; i < l.n && i < 256; i++) nice[i] = NV[r];
        if (l.n > 256) l.n = 256;
        apply_nice(&l, nice);
    } else if (r >= 7) {
        apply_background(&l, r == 7);
    }
    free(l.v);
}

static void nice_step(int d)
{
    PidList l = { 0 };
    int nice[256], i;
    collect_targets(&l, IN.on ? in_proc() : NULL, 0);
    if (l.n > 256) l.n = 256;
    for (i = 0; i < l.n; i++) {
        Proc *p = bypid[l.v[i]];
        int v = (p ? p->nice : 0) + d;
        nice[i] = v < PRIO_MIN ? PRIO_MIN : v > PRIO_MAX ? PRIO_MAX : v;
    }
    if (l.n) apply_nice(&l, nice);
    free(l.v);
}

static void reveal(Proc *p)
{
    pid_t pid;
    int st;
    char *av[] = { "/usr/bin/open", "-R", NULL, NULL };
    if (!p || !p->path) { set_msg(1, "Executable path unknown"); return; }
    av[2] = p->path;
    if (posix_spawn(&pid, av[0], NULL, NULL, av, environ) == 0) {
        waitpid(pid, &st, 0);
        set_msg(0, "Revealed %s in Finder", p->path);
    }
}

static void copy_path(Proc *p)
{
    FILE *f;
    if (!p || !p->path) { set_msg(1, "Executable path unknown"); return; }
    if (!(f = popen("/usr/bin/pbcopy", "w"))) return;
    fputs(p->path, f);
    if (pclose(f) == 0) set_msg(0, "Copied %s", p->path);
}

static void sort_menu(void)
{
    MItem it[SK_N];
    Menu m;
    int i, r;
    for (i = 0; i < SK_N; i++) {
        it[i].label = SK_LABEL[i];
        it[i].desc = NULL;
    }
    memset(&m, 0, sizeof m);
    m.title = "Sort by";
    m.it = it;
    m.n = SK_N;
    m.sel = U.sort;
    m.col = P_ACCENT;
    m.foot = "Enter select (again = invert) \xc2\xb7 Esc";
    r = menu_run(&m);
    if (r >= 0) set_sort(r);
}

static void set_all_collapsed(int c)
{
    size_t i;
    for (i = 0; i < PT.n; i++) PT.v[i]->collapsed = c;
    rebuild();
}

static void context_menu(void)
{
    static const MItem it[] = {
        { "Inspect", "details, files, environment, signature" },
        { "Send signal\xe2\x80\xa6", "terminate, kill, pause, resume" },
        { "Priority\xe2\x80\xa6", "nice / background mode" },
        { "Tag / untag", NULL },
        { "Reveal in Finder", NULL },
        { "Copy path", NULL },
        { "Filter to this name", NULL },
    };
    Proc *p = sel_proc();
    Menu m;
    char t[128];
    int r;
    if (!p) return;
    snprintf(t, sizeof t, "%s (%d)", p->name, p->pid);
    memset(&m, 0, sizeof m);
    m.title = t;
    m.it = it;
    m.n = (int)(sizeof it / sizeof it[0]);
    m.col = P_ACCENT;
    r = menu_run(&m);
    switch (r) {
    case 0: inspect_open(p); break;
    case 1: kill_menu(NULL); break;
    case 2: priority_menu(NULL); break;
    case 3: p->tagged = !p->tagged; break;
    case 4: reveal(p); break;
    case 5: copy_path(p); break;
    case 6:
        snprintf(U.filter, sizeof U.filter, "%s", p->name);
        rebuild();
        break;
    }
}

/* ------------------------------------------------------------------ */
/* input handling                                                      */
/* ------------------------------------------------------------------ */

static void search_next(size_t from)
{
    size_t i;
    if (!U.input[0] || !U.nrows) return;
    for (i = 0; i < U.nrows; i++) {
        size_t j = (from + i) % U.nrows;
        if (cmd_matches(U.rows[j].p, U.input)) { set_sel(j); return; }
    }
}

static void input_key(int k)
{
    size_t n = strlen(U.input);
    if (k == K_ESC) {
        if (U.input_mode == 1) { U.filter[0] = 0; rebuild(); }
        U.input_mode = 0;
        return;
    }
    if (k == '\r' || k == '\n') { U.input_mode = 0; return; }
    if (k == 127 || k == 8) {
        while (n > 0 && ((unsigned char)U.input[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0) n--;
        U.input[n] = 0;
    } else if (k == 21) {
        U.input[0] = 0;
    } else if (U.input_mode == 2 && (k == K_F(3) || k == K_DOWN)) {
        search_next(U.sel + 1);
        return;
    } else if (k == K_UP || k == K_DOWN || k == K_PGUP || k == K_PGDN) {
        size_t page = (size_t)U.list_h;
        if (k == K_UP && U.sel > 0) set_sel(U.sel - 1);
        if (k == K_DOWN) set_sel(U.sel + 1);
        if (k == K_PGUP) set_sel(U.sel > page ? U.sel - page : 0);
        if (k == K_PGDN) set_sel(U.sel + page);
        return;
    } else if (k >= 32 && k < 256 && k != 127 && n + 1 < sizeof U.input) {
        U.input[n] = (char)k;
        U.input[n + 1] = 0;
    } else {
        return;
    }
    if (U.input_mode == 1) {
        snprintf(U.filter, sizeof U.filter, "%s", U.input);
        U.sel = 0;
        U.anim_t0 = now_s();
        rebuild();
    } else {
        search_next(U.sel);
    }
}

static int inspect_key(int k)
{
    Proc *p = in_proc();
    int page = IN.view_h > 1 ? IN.view_h - 1 : 1;
    switch (k) {
    case K_ESC: case 'q': case K_F(2): case 'i':
        inspect_close();
        break;
    case 3:
        return 0;
    case K_LEFT: case K_BTAB: IN.tab = (IN.tab + IT_N - 1) % IT_N; IN.scroll = 0; break;
    case K_RIGHT: case '\t': IN.tab = (IN.tab + 1) % IT_N; IN.scroll = 0; break;
    case K_F(1): case '?': U.help = 1; break;
    case K_F(10): return 0;
    case K_UP: IN.scroll--; break;
    case K_DOWN: IN.scroll++; break;
    case K_PGUP: IN.scroll -= page; break;
    case K_PGDN: case ' ': IN.scroll += page; break;
    case K_HOME: IN.scroll = 0; break;
    case K_END: IN.scroll = 1 << 30; break;
    case 'k': case K_F(9): case K_DEL: if (p && !p->died) kill_menu(p); break;
    case 'n': if (p && !p->died) priority_menu(p); break;
    case K_F(7): case ']': nice_step(-1); break;
    case K_F(8): case '[': nice_step(1); break;
    case 'o': reveal(p); break;
    case 'y': copy_path(p); break;
    case 'S': relaunch_sudo(); break;
    case 'r':
        IN.built_tab = -1;
        if (p && IN.tab == IT_SIGN) cs_start(p);
        break;
    case K_MOUSE:
        if (MS.b == 64) IN.scroll -= 3;
        else if (MS.b == 65) IN.scroll += 3;
        else if (MS.press && MS.b == 0 && MS.y == IN.tab_y) {
            int i;
            for (i = 0; i < IT_N; i++)
                if (MS.x >= IN.tab_x[i] && MS.x < IN.tab_x[i + 1]) { IN.tab = i; IN.scroll = 0; }
        }
        break;
    }
    if (IN.scroll < 0) IN.scroll = 0;
    return 1;
}

static int handle_key(int k);

static void mouse_main(void)
{
    int i;
    double now = now_s();
    if (MS.b == 64 || MS.b == 65) {
        size_t step = 3;
        if (MS.b == 64) set_sel(U.sel > step ? U.sel - step : 0);
        else set_sel(U.sel + step);
        return;
    }
    if (!MS.press) return;
    if (MS.y == SC.h - 1 && MS.b == 0) {
        for (i = 0; i < NFOOT; i++)
            if (MS.x >= g_foot_x[i] && MS.x < g_foot_x[i + 1]) { handle_key(FOOT[i].key); return; }
        return;
    }
    if (MS.y == U.tab_y && MS.b == 0) {
        for (i = 0; i < NTABS; i++)
            if (MS.x >= U.tab_x[i] && MS.x < U.tab_x[i + 1]) { set_sort(i); return; }
        return;
    }
    if (MS.y == U.colhdr_y && MS.b == 0) {
        for (i = NCOLS - 1; i >= 0; i--)
            if (U.col_on[i] && MS.x >= U.col_x[i] && COLDEF[i].sk >= 0) { set_sort(COLDEF[i].sk); return; }
        return;
    }
    if (MS.y >= U.list_y && MS.y < U.list_y + U.list_h) {
        size_t idx = U.top + (size_t)(MS.y - U.list_y);
        if (idx >= U.nrows) return;
        if (MS.b == 0 && U.tree && U.rows[idx].kids && MS.x >= U.col_x[COL_CMD] &&
            MS.x < U.col_x[COL_CMD] + 2 * U.rows[idx].depth + 1 && MS.x >= U.col_x[COL_CMD] + 2 * U.rows[idx].depth - 1) {
            U.rows[idx].p->collapsed = !U.rows[idx].p->collapsed;
            rebuild();
            return;
        }
        set_sel(idx);
        if (MS.b == 2) { context_menu(); return; }
        if (MS.b == 0 && idx == U.click_row && now - U.click_t < 0.4) {
            inspect_open(sel_proc());
            U.click_t = 0;
            return;
        }
        U.click_row = idx;
        U.click_t = now;
    }
}

/* Returns 0 to quit. */
static int handle_key(int k)
{
    Proc *p = sel_proc();
    size_t page = (size_t)(U.list_h > 1 ? U.list_h - 1 : 1);
    if (U.help) {
        if (k != K_MOUSE || MS.press) U.help = 0;
        return 1;
    }
    if (U.input_mode) {
        if (k == 3) return 0;
        if (k == K_MOUSE) { mouse_main(); return 1; }
        input_key(k);
        return 1;
    }
    if (k >= '0' && k <= '9') k = K_F(k == '0' ? 10 : k - '0'); /* htop F-keys without Fn */
    if (IN.on) return inspect_key(k);
    switch (k) {
    case 'q': case 'Q': case 3: case K_F(10):
        return 0;
    case K_MOUSE: mouse_main(); break;
    case K_UP: if (U.sel > 0) set_sel(U.sel - 1); break;
    case K_DOWN: set_sel(U.sel + 1); break;
    case K_PGUP: set_sel(U.sel > page ? U.sel - page : 0); break;
    case K_PGDN: set_sel(U.sel + page); break;
    case K_HOME: set_sel(0); break;
    case K_END: if (U.nrows) set_sel(U.nrows - 1); break;
    case '\r': case '\n': case K_F(2): case 'i': inspect_open(p); break;
    case ' ':
        if (p) {
            p->tagged = !p->tagged;
            set_sel(U.sel + 1);
        }
        break;
    case 'U': {
        size_t i;
        for (i = 0; i < PT.n; i++) PT.v[i]->tagged = 0;
        break;
    }
    case 'c':
        if (p) {
            PidList l = { 0 };
            int i;
            collect_targets(&l, p, 1);
            for (i = 0; i < l.n; i++)
                if (bypid[l.v[i]]) bypid[l.v[i]]->tagged = 1;
            set_msg(0, "Tagged %d process%s", l.n, l.n == 1 ? "" : "es");
            free(l.v);
        }
        break;
    case K_F(3):
        U.input_mode = 2;
        U.input[0] = 0;
        break;
    case '/': case '\\': case K_F(4):
        U.input_mode = 1;
        snprintf(U.input, sizeof U.input, "%s", U.filter);
        break;
    case K_ESC:
        if (U.filter[0]) { U.filter[0] = 0; rebuild(); }
        break;
    case K_F(5): case 't':
        U.tree = !U.tree;
        U.anim_t0 = now_s();
        rebuild();
        break;
    case K_F(6): sort_menu(); break;
    case '\t': set_sort(U.sort < NTABS - 1 ? U.sort + 1 : 0); break;
    case K_BTAB: set_sort(U.sort > 0 && U.sort < NTABS ? U.sort - 1 : NTABS - 1); break;
    case 'P': set_sort(SK_CPU); break;
    case 'G': set_sort(SK_GPU); break;
    case 'M': set_sort(SK_MEM); break;
    case 'B': case 'E': set_sort(SK_POWER); break;
    case 'D': set_sort(SK_DISK); break;
    case 'N': set_sort(SK_PID); break;
    case 'T': set_sort(SK_TIME); break;
    case 'I':
        U.desc = !U.desc;
        U.anim_t0 = now_s();
        rebuild();
        break;
    case K_F(7): case ']': nice_step(-1); break;
    case K_F(8): case '[': nice_step(1); break;
    case 'n': priority_menu(NULL); break;
    case K_F(9): case 'k': case K_DEL: kill_menu(NULL); break;
    case K_LEFT: case '-':
        if (U.tree && p) {
            if (U.rows[U.sel].kids && !p->collapsed) {
                p->collapsed = 1;
                rebuild();
            } else if (parent_of(p)) {
                size_t i;
                for (i = U.sel; i-- > 0;)
                    if (U.rows[i].p == parent_of(p)) { set_sel(i); break; }
            }
        }
        break;
    case K_RIGHT: case '+': case '=':
        if (U.tree && p && p->collapsed) {
            p->collapsed = 0;
            rebuild();
        }
        break;
    case '*': {
        size_t i;
        int any = 0;
        for (i = 0; i < PT.n; i++) any |= PT.v[i]->collapsed;
        set_all_collapsed(!any);
        break;
    }
    case 'u':
        U.only_user = !U.only_user;
        U.anim_t0 = now_s();
        rebuild();
        break;
    case 'p': U.fullpath = !U.fullpath; break;
    case 'o': reveal(p); break;
    case 'y': copy_path(p); break;
    case 'z':
        U.paused = !U.paused;
        if (!U.paused) U.next_sample = now_s();
        set_msg(0, U.paused ? "Updates paused (z to resume)" : "Updates resumed");
        break;
    case ',': case '.': {
        static const double IV[] = { 0.25, 0.5, 1.0, 1.5, 2.0, 3.0, 5.0, 10.0 };
        int i, n = (int)(sizeof IV / sizeof IV[0]), cur = 0;
        for (i = 0; i < n; i++)
            if (fabs(IV[i] - U.interval) < fabs(IV[cur] - U.interval)) cur = i;
        cur += k == ',' ? 1 : -1;
        if (cur < 0) cur = 0;
        if (cur >= n) cur = n - 1;
        U.interval = IV[cur];
        U.next_sample = now_s() + U.interval;
        set_msg(0, "Refresh every %.2gs", U.interval);
        break;
    }
    case 'm': U.compact = !U.compact; break;
    case 'S': relaunch_sudo(); break;
    case '?': case 'h': case K_F(1): U.help = 1; break;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* startup splash                                                      */
/* ------------------------------------------------------------------ */

static double g_splash_t0;

static void splash(double frac, const char *what)
{
    static const char *logo[] = {
        "\xe2\x96\x88\xe2\x96\x80\xe2\x96\x84\xe2\x96\x80\xe2\x96\x88 \xe2\x96\x88 \xe2\x96\x88 \xe2\x96\x80\xe2\x96\x88\xe2\x96\x80 \xe2\x96\x88\xe2\x96\x80\xe2\x96\x88 \xe2\x96\x88\xe2\x96\x80\xe2\x96\x88",
        "\xe2\x96\x88 \xe2\x96\x80 \xe2\x96\x88 \xe2\x96\x88\xe2\x96\x80\xe2\x96\x88  \xe2\x96\x88  \xe2\x96\x88\xe2\x96\x84\xe2\x96\x88 \xe2\x96\x88\xe2\x96\x80\xe2\x96\x80",
    };
    double now = now_s(), t = now - g_splash_t0;
    int y, x, w = 40, i, j;
    if (g_winch) { g_winch = 0; update_size(); }
    scr_clear();
    y = SC.h / 2 - 4;
    if (y < 0) y = 0;
    for (j = 0; j < 2; j++) {
        int lw = str_width(logo[j]);
        size_t len = strlen(logo[j]), b = 0;
        x = (SC.w - lw) / 2;
        for (i = 0; b < len; i++) {
            Glyph g = glyph(logo[j] + b, len - b);
            double ph = sin(i * 0.35 - t * 6.0) * 0.5 + 0.5;
            set_cell(y + j, x + i, ST(GRAD[(int)(ph * (NGRAD - 1))], CDEF, A_BOLD), logo[j] + b, g.n, 1);
            b += (size_t)g.n;
        }
    }
    if (w > SC.w - 4) w = SC.w - 4;
    x = (SC.w - w) / 2;
    scr_put(y + 3, (SC.w - 30) / 2, 30, ST(245, CDEF, 0), "process manager for macOS " VERSION);
    heat_bar(y + 5, x, w, frac, now, BAR_BG);
    scr_printf(y + 7, x, w, ST(252, CDEF, 0), "%s %s", SPIN[(int)(now * 12) % 10], what);
    scr_flush();
}

static void splash_progress(size_t done, size_t total)
{
    char t[96];
    snprintf(t, sizeof t, "Reading processes %zu / %zu", done, total);
    splash(total ? 0.7 * (double)done / (double)total : 0, t);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(FILE *f)
{
    fprintf(f,
            "usage: " APP " [-d secs] [-s key] [-t] [-u user] [-f filter] [-c] [-M]\n"
            "  Interactive process manager for macOS (htop / Activity Monitor style).\n"
            "  -d secs   refresh interval (default 1.5)\n"
            "  -s key    sort: cpu gpu mem power disk time threads pri nice pid user name\n"
            "  -t        start in tree view (parents with their children)\n"
            "  -u user   show only this user's processes (toggle with u)\n"
            "  -f text   initial filter\n"
            "  -c        compact header\n"
            "  -M        disable mouse support (keeps terminal text selection)\n"
            "  -h        show this help;  -V  print version\n"
            "  Run with sudo to see CPU, memory and power of every user's processes.\n");
}

static void setup_locale(void)
{
    setlocale(LC_CTYPE, "");
    if (MB_CUR_MAX == 1 && !setlocale(LC_CTYPE, "en_US.UTF-8")) setlocale(LC_CTYPE, "UTF-8");
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    int i, k;
    double t1;

    U.argc = argc;
    U.argv = argv;
    U.interval = 1.5;
    U.sort = SK_CPU;
    U.desc = 1;
    U.sel_pid = -1;
    U.me = getuid();
    U.user_uid = U.me;
    U.root = geteuid() == 0;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        else if (!strcmp(a, "-V") || !strcmp(a, "--version")) { puts(APP " " VERSION); return 0; }
        else if (!strcmp(a, "-t")) U.tree = 1;
        else if (!strcmp(a, "-c")) U.compact = 1;
        else if (!strcmp(a, "-M")) g_mouse = 0;
        else if (!strcmp(a, "-d") && i + 1 < argc) {
            U.interval = atof(argv[++i]);
            if (U.interval < 0.1 || U.interval > 3600) { fprintf(stderr, APP ": bad interval\n"); return 2; }
        } else if (!strcmp(a, "-s") && i + 1 < argc) {
            int s;
            for (s = 0; s < SK_N && strcmp(SK_ARG[s], argv[i + 1]); s++)
                ;
            if (s == SK_N) { fprintf(stderr, APP ": unknown sort key '%s'\n", argv[i + 1]); return 2; }
            U.sort = s;
            U.desc = sk_desc_default(s);
            i++;
        } else if (!strcmp(a, "-u") && i + 1 < argc) {
            struct passwd *pw = getpwnam(argv[++i]);
            if (!pw) { fprintf(stderr, APP ": unknown user '%s'\n", argv[i]); return 2; }
            U.user_uid = pw->pw_uid;
            U.only_user = 1;
        } else if (!strcmp(a, "-f") && i + 1 < argc) {
            snprintf(U.filter, sizeof U.filter, "%s", argv[++i]);
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, APP ": needs an interactive terminal\n");
        return 1;
    }

    setup_locale();
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fatal_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    sys_init();
    update_size();
    term_raw();

    /* two samples are needed for rates; animate while gathering them */
    g_splash_t0 = now_s();
    splash(0, "Reading system information");
    sample_all(now_s(), splash_progress);
    t1 = now_s();
    for (;;) {
        double f = (now_s() - t1) / 0.45;
        if (f >= 1) break;
        splash(0.7 + 0.3 * f, "Measuring CPU, GPU and energy");
        k = read_key(16);
        if (k == 'q' || k == 3 || k == K_ESC) return 0;
    }
    sample_all(now_s(), NULL);
    U.next_sample = now_s() + U.interval;
    U.pulse_t = U.anim_t0 = now_s();
    rebuild();
    SC.full = 1;

    for (;;) {
        frame(NULL, NULL);
        k = next_key();
        if (k == K_NONE) continue;
        if (!handle_key(k)) break;
    }
    term_restore();
    return 0;
}
