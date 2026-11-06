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

/* One accelerator user client with GPU work: accumulated GPU time, last
 * submission (ns of mach_absolute_time), command queues, APIs used. */
typedef struct { pid_t pid; uint64_t ns, last; int queues, qmax; unsigned api; } GpuUse;
enum { API_METAL = 1, API_GL = 2, API_CL = 4, API_OTHER = 8 };

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
    double gpu_util, gpu_rend, gpu_tiler; /* rend/tiler in %, -1 unknown */
    uint64_t gpu_mem, gpu_alloc, gpu_drv, gpu_pb, gpu_tiled;
    int64_t gpu_split, gpu_recov;
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

static void gu_add(const GpuUse *u)
{
    if (S.ngu == S.gucap) {
        S.gucap = S.gucap ? S.gucap * 2 : 128;
        S.gu = xrealloc(S.gu, S.gucap * sizeof *S.gu);
    }
    S.gu[S.ngu++] = *u;
}

/* GPU: device utilisation and memory from IOAccelerator, per-process
 * accumulated GPU time from each accelerator user client's "AppUsage". */
static void sample_gpu(void)
{
    io_iterator_t it, kids;
    io_object_t acc, cl;
    double util = -1, rend = -1, tiler = -1;
    S.ngu = 0;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS) return;
    while ((acc = IOIteratorNext(it))) {
        CFTypeRef ps = reg_prop(acc, "PerformanceStatistics");
        if (ps) {
            int64_t u = dict_i64(ps, "Device Utilization %", -1), r = dict_i64(ps, "Renderer Utilization %", -1);
            int64_t t = dict_i64(ps, "Tiler Utilization %", -1);
            if (u < 0) u = dict_i64(ps, "GPU Activity(%)", -1);
            if (u >= 0 && (double)u > util) util = (double)u;
            if (r >= 0 && (double)r > rend) rend = (double)r;
            if (t >= 0 && (double)t > tiler) tiler = (double)t;
            S.gpu_mem = (uint64_t)dict_i64(ps, "In use system memory", 0);
            S.gpu_alloc = (uint64_t)dict_i64(ps, "Alloc system memory", 0);
            S.gpu_drv = (uint64_t)dict_i64(ps, "In use system memory (driver)", 0);
            S.gpu_pb = (uint64_t)dict_i64(ps, "Allocated PB Size", 0);
            S.gpu_tiled = (uint64_t)dict_i64(ps, "TiledSceneBytes", 0);
            S.gpu_split = dict_i64(ps, "SplitSceneCount", 0);
            S.gpu_recov = dict_i64(ps, "recoveryCount", 0);
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
                        GpuUse u = { pid, 0, 0, 0, 0, 0 };
                        for (i = 0; i < n; i++) {
                            CFDictionaryRef d = CFArrayGetValueAtIndex(au, i);
                            uint64_t l = (uint64_t)dict_i64(d, "lastSubmittedTime", 0);
                            char api[32];
                            u.ns += (uint64_t)dict_i64(d, "accumulatedGPUTime", 0);
                            if (l > u.last) u.last = l;
                            if (CFGetTypeID(d) != CFDictionaryGetTypeID()) continue;
                            cfstr_get(CFDictionaryGetValue(d, CFSTR("API")), api, sizeof api);
                            u.api |= !strcmp(api, "Metal") ? API_METAL : !strcmp(api, "OpenGL") ? API_GL
                                   : !strcmp(api, "OpenCL") ? API_CL : api[0] ? API_OTHER : 0;
                        }
                        if (u.ns) {
                            u.queues = (int)reg_i64(cl, "CommandQueueCount", 0);
                            u.qmax = (int)reg_i64(cl, "CommandQueueCountMax", 0);
                            gu_add(&u);
                        }
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
    S.gpu_rend = rend;
    S.gpu_tiler = tiler;
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

/* ------------------------------------------------------------------ */
/* GPU telemetry (GPU view and --gpu)                                  */
/* ------------------------------------------------------------------ */

/* libIOReport: a macOS system library without a public header. */
typedef struct IOReportSubscription *IOReportSubscriptionRef;
CFDictionaryRef IOReportCopyChannelsInGroup(CFStringRef group, CFStringRef subgroup, uint64_t a, uint64_t b, uint64_t c);
void IOReportMergeChannels(CFDictionaryRef dst, CFDictionaryRef src, CFTypeRef unused);
IOReportSubscriptionRef IOReportCreateSubscription(void *a, CFMutableDictionaryRef desired, CFMutableDictionaryRef *subbed,
                                                   uint64_t id, CFTypeRef b);
CFDictionaryRef IOReportCreateSamples(IOReportSubscriptionRef sub, CFMutableDictionaryRef subbed, CFTypeRef unused);
CFDictionaryRef IOReportCreateSamplesDelta(CFDictionaryRef a, CFDictionaryRef b, CFTypeRef unused);
CFStringRef IOReportChannelGetGroup(CFDictionaryRef ch);
CFStringRef IOReportChannelGetSubGroup(CFDictionaryRef ch);
CFStringRef IOReportChannelGetChannelName(CFDictionaryRef ch);
CFStringRef IOReportChannelGetUnitLabel(CFDictionaryRef ch);
int IOReportChannelGetFormat(CFDictionaryRef ch);
int64_t IOReportSimpleGetIntegerValue(CFDictionaryRef ch, int32_t unused);
int32_t IOReportStateGetCount(CFDictionaryRef ch);
CFStringRef IOReportStateGetNameForIndex(CFDictionaryRef ch, int32_t i);
int64_t IOReportStateGetResidency(CFDictionaryRef ch, int32_t i);

#define GHIST 512  /* system GPU history samples (stats window) */
#define GPH 240    /* per-process GPU history samples */
#define GNS 40     /* states kept per IOReport state channel */
#define GNEV 128   /* event log entries */
#define GMAXCL 512 /* tracked GPU client processes */

enum { GM_UTIL, GM_REND, GM_TILER, GM_ACTIVE, GM_FREQ, GM_POWER, GM_TEMP, GM_TMAX, GM_MEM, GM_BW, GM_DRAMW, GM_N };
static const struct { const char *key, *name; } GMDEF[GM_N] = {
    { "device_util_pct", "Device" },        { "renderer_util_pct", "Renderer" }, { "tiler_util_pct", "Tiler" },
    { "active_residency_pct", "Active" },   { "clock_mhz", "Clock" },            { "gpu_power_w", "Power" },
    { "temp_avg_c", "Temp" },               { "temp_max_c", "Temp max" },        { "gpu_mem_in_use_bytes", "GPU memory" },
    { "dram_bandwidth_gbs", "DRAM bandwidth" }, { "dram_power_w", "DRAM power" },
};

/* One metric: history ring (shared position G.hpos) and stats since reset. */
typedef struct { float h[GHIST]; double cur, sum, mn, mx, p95; long n; int ok; } GMet;
/* An IOReport state channel: residency per state this sample and since reset. */
typedef struct { int n; char name[GNS][16]; double res[GNS], cum[GNS], tot, cumtot; } GStates;

typedef struct {
    pid_t pid;
    long start_s, start_us;
    char name[96];
    int alive, fresh, nclients, queues, qmax;
    unsigned api, gen;
    double first_t, t_prev, gone_t, cur, peak, est_w;
    uint64_t ns, ns0, ns_prev, last_sub, phys;
    int64_t gfx, gfx_peak;
    float hist[GPH];
    int hn;
} GClient;

typedef struct { time_t t; int sev; char s[160]; } GEvent; /* sev: 0 info, 1 warning, 2 fault */

enum { GT_OVER, GT_PROC, GT_CLOCK, GT_MEM, GT_EVENTS, GT_N };
static const char *GT_LABEL[GT_N] = { "Overview", "Processes", "Clocks & Power", "Memory & Bandwidth", "Events" };
enum { GS_NOW, GS_AVG, GS_TIME, GS_GFX, GS_PID, GS_N };
static const char *GS_LABEL[GS_N] = { "GPU% now", "average GPU%", "GPU time", "graphics memory", "PID" };

static struct {
    int active, primed, on, tab, scroll, total, view_h, sort, sel, top, list_y, list_h, tab_y, tab_x[GT_N + 1];
    double t0, t_reset, t_last, t_ior, click_t;
    int click_row;
    pid_t sel_pid;
    const char *query; /* -p: preferred focus when none is pinned */
    /* device */
    char name[64], family[32], driver[32], metal[48];
    int cores, mgpus;
    double freq[GNS], fmax;
    int nfreq;
    long wired_mb;
    /* IOReport */
    IOReportSubscriptionRef sub;
    CFMutableDictionaryRef subbed;
    CFDictionaryRef prev;
    int ior;
    /* SMC GPU temperature keys */
    io_connect_t smc;
    uint32_t tkey[128];
    int ntk;
    /* this sample */
    double active_res, mhz, gpu_w, dram_w, ane_w, cpu_w, temp, tmax, bw_rd, bw_wr, lat, ppm;
    int has_gpu_w, has_dram_w, has_ane_w, has_cpu_w, has_freq, has_temp, has_bw, has_lat, has_ppm, ntemp;
    long throttle, restarts, uvwarn;
    GStates ps, pc, cltm, pmu, pz;
    /* since reset */
    double energy_j, dram_j, tmax_peak;
    long throttle_tot, restarts_tot, uvwarn_tot, samples;
    GMet m[GM_N];
    int hpos, hn;
    /* GPU client processes */
    GClient *cl;
    int ncl, clcap, *ord, nord;
    unsigned gen;
    pid_t focus;
    long focus_s, focus_us;
    int clients_seen;
    /* events */
    GEvent ev[GNEV];
    unsigned long nev;
    int was_thermal, was_limit, was_pressure, was_mem;
    int64_t recov;
} G = { .focus = -1, .sel_pid = -1 };

static void gpu_event(int sev, const char *fmt, ...)
{
    GEvent *e = &G.ev[G.nev % GNEV];
    va_list ap;
    e->t = time(NULL);
    e->sev = sev;
    va_start(ap, fmt);
    vsnprintf(e->s, sizeof e->s, fmt, ap);
    va_end(ap);
    G.nev++;
}

static void gpu_static_init(void)
{
    io_iterator_t it;
    io_object_t acc;
    snprintf(G.name, sizeof G.name, "%s", S.gpu_name[0] ? S.gpu_name : "GPU");
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS) return;
    if ((acc = IOIteratorNext(it))) {
        CFTypeRef cfg = reg_prop(acc, "GPUConfigurationVariable"), v;
        G.cores = (int)reg_i64(acc, "gpu-core-count", 0);
        if (cfg && CFGetTypeID(cfg) == CFDictionaryGetTypeID()) {
            char var[8];
            int64_t gen = dict_i64(cfg, "gpu_gen", -1);
            if (!G.cores) G.cores = (int)dict_i64(cfg, "num_cores", 0);
            G.mgpus = (int)dict_i64(cfg, "num_mgpus", 0);
            cfstr_get(CFDictionaryGetValue(cfg, CFSTR("gpu_var")), var, sizeof var);
            if (gen >= 0) snprintf(G.family, sizeof G.family, "G%lld%s", (long long)gen, var);
        }
        if (cfg) CFRelease(cfg);
        if ((v = reg_prop(acc, "IOSourceVersion"))) { cfstr_get(v, G.driver, sizeof G.driver); CFRelease(v); }
        if ((v = reg_prop(acc, "MetalPluginName"))) { cfstr_get(v, G.metal, sizeof G.metal); CFRelease(v); }
        IOObjectRelease(acc);
    }
    IOObjectRelease(it);
}

/* GPU DVFS table: pmgr "voltage-states9", (frequency, voltage) u32 pairs;
 * entry k is performance state P(k+1) once the 0 Hz entry is dropped. */
static void gpu_freq_init(void)
{
    io_iterator_t it;
    io_object_t e;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceNameMatching("pmgr"), &it) != KERN_SUCCESS) return;
    while ((e = IOIteratorNext(it))) {
        CFTypeRef v = reg_prop(e, "voltage-states9");
        if (v && CFGetTypeID(v) == CFDataGetTypeID() && !G.nfreq) {
            const uint8_t *b = CFDataGetBytePtr(v);
            CFIndex len = CFDataGetLength(v), i;
            double mx = 0, div;
            uint32_t raw[GNS];
            int n = 0, k;
            for (i = 0; i + 8 <= len && n < GNS; i += 8) {
                uint32_t f;
                memcpy(&f, b + i, sizeof f);
                if (!f) continue;
                raw[n++] = f;
                if (f > mx) mx = f;
            }
            div = mx > 1e7 ? 1e6 : mx > 1e4 ? 1e3 : 1; /* Hz or kHz depending on the chip */
            for (k = 0; k < n; k++) G.freq[k] = raw[k] / div;
            G.nfreq = n;
            G.fmax = mx / div;
        }
        if (v) CFRelease(v);
        IOObjectRelease(e);
    }
    IOObjectRelease(it);
}

enum { GC_E_GPU, GC_E_GPUN, GC_E_DRAM, GC_E_ANE, GC_E_CPU, GC_PSTATE, GC_PWRCTRL, GC_CLTM, GC_PMU, GC_PPM, GC_PZONE,
       GC_THROT, GC_RESTART, GC_UVWARN, GC_BW_RD, GC_BW_WR, GC_LAT };

/* Which of the subscribed IOReport channels this is; -1 for the rest. */
static int ior_class(CFDictionaryRef ch)
{
    char g[64], sg[96], nm[96];
    cfstr_get(IOReportChannelGetGroup(ch), g, sizeof g);
    cfstr_get(IOReportChannelGetSubGroup(ch), sg, sizeof sg);
    cfstr_get(IOReportChannelGetChannelName(ch), nm, sizeof nm);
    if (!strcmp(g, "Energy Model")) {
        if (!strcmp(nm, "GPU Energy")) return GC_E_GPU;
        if (!strncmp(nm, "GPU", 3) && nm[3] >= '0' && nm[3] <= '9') return GC_E_GPUN;
        if (!strncmp(nm, "DRAM", 4)) return GC_E_DRAM;
        if (!strncmp(nm, "ANE", 3)) return GC_E_ANE;
        if (!strcmp(nm, "CPU Energy")) return GC_E_CPU;
    } else if (!strcmp(g, "GPU Stats")) {
        if (!strcmp(sg, "GPU Performance States")) return GC_PSTATE;
        if (!strcmp(sg, "GPU Power Controller States")) return GC_PWRCTRL;
        if (!strcmp(sg, "CLTM-induced GPU Performance States")) return GC_CLTM;
        if (!strcmp(sg, "PMU Loop Lost Performance Reason Code States")) return GC_PMU;
        if (!strcmp(sg, "PPM Target as % of Max GPU Power")) return GC_PPM;
        if (!strcmp(sg, "GPU Discrete Power Zone Residency")) return GC_PZONE;
        if (!strcmp(sg, "GPU Throttler Counters") && !strncmp(nm, "Throttle Counter Total", 22)) return GC_THROT;
        if (!strcmp(sg, "Issue Counts") && !strcmp(nm, "Total Restarts")) return GC_RESTART;
        if (!strcmp(sg, "UV Warn State") && !strcmp(nm, "UVWarn Instances")) return GC_UVWARN;
    } else if (!strncmp(g, "PMP", 3)) {
        if (!strcmp(sg, "DCS BW") && !strcmp(nm, "AMCC RD")) return GC_BW_RD;
        if (!strcmp(sg, "DCS BW") && !strcmp(nm, "AMCC WR")) return GC_BW_WR;
        if (!strcmp(sg, "AGX") && !strcmp(nm, "Mem Latency")) return GC_LAT;
    }
    return -1;
}

static CFIndex ior_nchan(CFDictionaryRef d)
{
    CFArrayRef a = d ? CFDictionaryGetValue(d, CFSTR("IOReportChannels")) : NULL;
    return a && CFGetTypeID(a) == CFArrayGetTypeID() ? CFArrayGetCount(a) : 0;
}

/* Subscribes to the GPU energy, performance-state, limiter and memory
 * controller channels only; the full channel set is thousands strong. */
static void ior_init(void)
{
    static const char *grp[] = { "Energy Model", "GPU Stats", "PMP", "PMP0" };
    CFMutableDictionaryRef all = NULL;
    CFMutableArrayRef keep;
    CFArrayRef a;
    CFIndex i, n;
    size_t k;
    int pmp = 0;
    for (k = 0; k < sizeof grp / sizeof grp[0]; k++) {
        CFStringRef s;
        CFDictionaryRef d;
        if (k == 3 && pmp) break; /* "PMP0" only on chips without a "PMP" group */
        s = CFStringCreateWithCString(NULL, grp[k], kCFStringEncodingUTF8);
        d = IOReportCopyChannelsInGroup(s, NULL, 0, 0, 0);
        CFRelease(s);
        if (!ior_nchan(d)) {
            if (d) CFRelease(d);
            continue;
        }
        if (k == 2) pmp = 1;
        if (!all) all = CFDictionaryCreateMutableCopy(NULL, 0, d);
        else IOReportMergeChannels(all, d, NULL);
        CFRelease(d);
    }
    if (!all) return;
    a = CFDictionaryGetValue(all, CFSTR("IOReportChannels"));
    n = a && CFGetTypeID(a) == CFArrayGetTypeID() ? CFArrayGetCount(a) : 0;
    keep = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (i = 0; i < n; i++) {
        CFDictionaryRef ch = CFArrayGetValueAtIndex(a, i);
        if (CFGetTypeID(ch) == CFDictionaryGetTypeID() && ior_class(ch) >= 0) CFArrayAppendValue(keep, ch);
    }
    n = CFArrayGetCount(keep);
    CFDictionarySetValue(all, CFSTR("IOReportChannels"), keep);
    CFRelease(keep);
    if (!n) return;
    /* `all` stays alive for the lifetime of the subscription */
    G.sub = IOReportCreateSubscription(NULL, all, &G.subbed, 0, NULL);
    if (!G.sub || !G.subbed) return;
    G.t_ior = now_s();
    G.prev = IOReportCreateSamples(G.sub, G.subbed, NULL);
    G.ior = G.prev != NULL;
}

static void gst_begin(GStates *st)
{
    int i;
    for (i = 0; i < st->n; i++) st->res[i] = 0;
    st->tot = 0;
}

/* Adds one state channel's residencies (channels of a class are summed). */
static void gst_read(GStates *st, CFDictionaryRef ch)
{
    int32_t i, n = IOReportStateGetCount(ch);
    if (n > GNS) n = GNS;
    if (n < 0) n = 0;
    if (n != st->n) {
        memset(st, 0, sizeof *st);
        st->n = n;
    }
    for (i = 0; i < n; i++) {
        double r = (double)IOReportStateGetResidency(ch, i);
        if (r < 0) r = 0;
        cfstr_get(IOReportStateGetNameForIndex(ch, i), st->name[i], sizeof st->name[i]);
        st->res[i] += r;
        st->tot += r;
        st->cum[i] += r;
        st->cumtot += r;
    }
}

/* Fraction of time spent in the state called `name`; -1 if unknown. */
static double gst_frac(const GStates *st, const char *name, int cum)
{
    int i;
    double t = cum ? st->cumtot : st->tot;
    if (t <= 0) return -1;
    for (i = 0; i < st->n; i++)
        if (!strcmp(st->name[i], name)) return (cum ? st->cum[i] : st->res[i]) / t;
    return -1;
}

/* Fraction of time outside the state called `ok` (0 when unknown). */
static double gst_out(const GStates *st, const char *ok, int cum)
{
    double f = gst_frac(st, ok, cum);
    return f < 0 ? 0 : 1 - f;
}

/* Performance state number of "P7", 0 for "OFF" and the like. */
static int gst_pnum(const char *name)
{
    return name[0] == 'P' && name[1] >= '0' && name[1] <= '9' ? atoi(name + 1) : 0;
}

/* Power controller states that hold the clock below what the load asks
 * for (power, thermal and platform limits). */
static int pc_limiting(const char *n)
{
    static const char *L[] = { "POWER", "PWR_ZONE", "FAST_DIE", "PRFLIMIT", "PPMLIMIT", "CLTMLIMI", "PPMTARGE", "PMULOOP" };
    size_t i;
    for (i = 0; i < sizeof L / sizeof L[0]; i++)
        if (!strcmp(n, L[i])) return 1;
    return 0;
}

/* Share of active (non-idle) GPU time under limiting controllers; their
 * names go to `o`. */
static double gpu_limit(char *o, size_t sz, int cum)
{
    const GStates *st = &G.pc;
    double act = 0, lim = 0;
    int i;
    size_t used = 0;
    if (o && sz) o[0] = 0;
    for (i = 0; i < st->n; i++) {
        double r = cum ? st->cum[i] : st->res[i];
        if (!strcmp(st->name[i], "IDLE_OFF")) continue;
        act += r;
        if (!pc_limiting(st->name[i]) || r <= 0) continue;
        lim += r;
        if (o && used + 12 < sz) used += (size_t)snprintf(o + used, sz - used, "%s%s", used ? ", " : "", st->name[i]);
    }
    return act > 0 ? lim / act : 0;
}

/* Mean of a bucketed state channel ("16GB/s", "64Cyc", ...): each state
 * counts the windows that fell in its bucket, labelled by the upper edge,
 * so the bucket midpoint stands for them. -1 when empty. */
static double ior_hist_mean(CFDictionaryRef ch)
{
    int32_t i, n = IOReportStateGetCount(ch);
    double edge[128], cnt = 0, sum = 0, step;
    char nm[32];
    if (n > 128) n = 128;
    if (n < 2) return -1;
    for (i = 0; i < n; i++) {
        cfstr_get(IOReportStateGetNameForIndex(ch, i), nm, sizeof nm);
        edge[i] = strtod(nm, NULL);
    }
    step = edge[1] - edge[0];
    for (i = 0; i < n; i++) {
        double c = (double)IOReportStateGetResidency(ch, i);
        if (c <= 0) continue;
        sum += (edge[i] - step / 2) * c;
        cnt += c;
    }
    return cnt > 0 ? sum / cnt : -1;
}

/* Time-weighted power target from "0-9%" .. "100%" buckets. */
static double ior_ppm(CFDictionaryRef ch)
{
    int32_t i, n = IOReportStateGetCount(ch);
    double sum = 0, tot = 0;
    char nm[32];
    for (i = 0; i < n; i++) {
        double r = (double)IOReportStateGetResidency(ch, i), lo;
        if (r <= 0) continue;
        cfstr_get(IOReportStateGetNameForIndex(ch, i), nm, sizeof nm);
        lo = strtod(nm, NULL);
        sum += (lo >= 100 ? 100 : lo + 5) * r;
        tot += r;
    }
    return tot > 0 ? sum / tot : -1;
}

static double ior_joules(CFDictionaryRef ch)
{
    char u[16];
    double v = (double)IOReportSimpleGetIntegerValue(ch, 0);
    cfstr_get(IOReportChannelGetUnitLabel(ch), u, sizeof u);
    if (!strcmp(u, "nJ")) return v * 1e-9;
    if (!strcmp(u, "uJ") || !strcmp(u, "\xc2\xb5J")) return v * 1e-6;
    if (!strcmp(u, "J")) return v;
    return v * 1e-3; /* mJ */
}

static void ior_sample(double now)
{
    CFDictionaryRef cur, d;
    CFArrayRef a;
    CFIndex i, n;
    double dt, eg = 0, egn = 0, ed = 0, ea = 0, ec = 0, bwr = 0, bww = 0, lat = 0, ppm = -1, v;
    int seen_eg = 0, seen_egn = 0, nbw = 0, nlat = 0, j;
    if (!G.ior) return;
    cur = IOReportCreateSamples(G.sub, G.subbed, NULL);
    if (!cur) return;
    d = IOReportCreateSamplesDelta(G.prev, cur, NULL);
    CFRelease(G.prev);
    G.prev = cur;
    dt = now - G.t_ior;
    G.t_ior = now;
    if (!d) return;
    gst_begin(&G.ps);
    gst_begin(&G.pc);
    gst_begin(&G.cltm);
    gst_begin(&G.pmu);
    gst_begin(&G.pz);
    G.throttle = G.restarts = G.uvwarn = 0;
    a = CFDictionaryGetValue(d, CFSTR("IOReportChannels"));
    n = a && CFGetTypeID(a) == CFArrayGetTypeID() ? CFArrayGetCount(a) : 0;
    for (i = 0; i < n; i++) {
        CFDictionaryRef ch = CFArrayGetValueAtIndex(a, i);
        int state = IOReportChannelGetFormat(ch) == 2;
        switch (ior_class(ch)) {
        case GC_E_GPU: eg += ior_joules(ch); seen_eg = 1; break;
        case GC_E_GPUN: egn += ior_joules(ch); seen_egn = 1; break;
        case GC_E_DRAM: ed += ior_joules(ch); break;
        case GC_E_ANE: ea += ior_joules(ch); break;
        case GC_E_CPU: ec += ior_joules(ch); break;
        case GC_PSTATE: if (state) gst_read(&G.ps, ch); break;
        case GC_PWRCTRL: if (state) gst_read(&G.pc, ch); break;
        case GC_CLTM: if (state) gst_read(&G.cltm, ch); break;
        case GC_PMU: if (state) gst_read(&G.pmu, ch); break;
        case GC_PZONE: if (state) gst_read(&G.pz, ch); break;
        case GC_PPM: if (state && (v = ior_ppm(ch)) >= 0) ppm = v; break;
        case GC_THROT: G.throttle += (long)IOReportSimpleGetIntegerValue(ch, 0); break;
        case GC_RESTART: G.restarts += (long)IOReportSimpleGetIntegerValue(ch, 0); break;
        case GC_UVWARN: G.uvwarn += (long)IOReportSimpleGetIntegerValue(ch, 0); break;
        case GC_BW_RD: if (state && (v = ior_hist_mean(ch)) >= 0) { bwr += v; nbw++; } break;
        case GC_BW_WR: if (state && (v = ior_hist_mean(ch)) >= 0) { bww += v; nbw++; } break;
        case GC_LAT: if (state && (v = ior_hist_mean(ch)) >= 0) { lat += v; nlat++; } break;
        }
    }
    CFRelease(d);
    if (dt <= 0.01) return;
    if (!seen_eg) eg = egn;
    G.has_gpu_w = seen_eg || seen_egn;
    G.gpu_w = eg / dt;
    /* these channels exist on chips that leave them at zero: show only once seen */
    G.has_dram_w |= ed > 0;
    G.has_ane_w |= ea > 0;
    G.has_cpu_w |= ec > 0;
    G.dram_w = ed / dt;
    G.ane_w = ea / dt;
    G.cpu_w = ec / dt;
    if (G.primed) {
        G.energy_j += eg;
        G.dram_j += ed;
        G.throttle_tot += G.throttle;
        G.restarts_tot += G.restarts;
        G.uvwarn_tot += G.uvwarn;
    }
    G.has_bw = nbw > 0;
    G.bw_rd = bwr;
    G.bw_wr = bww;
    G.has_lat = nlat > 0;
    G.lat = nlat ? lat / nlat : 0;
    G.has_ppm = ppm >= 0;
    G.ppm = ppm;
    {
        double act = 0, fsum = 0, fres = 0;
        for (j = 0; j < G.ps.n; j++) {
            int k = gst_pnum(G.ps.name[j]);
            if (k <= 0) continue;
            act += G.ps.res[j];
            if (k <= G.nfreq) {
                fsum += G.freq[k - 1] * G.ps.res[j];
                fres += G.ps.res[j];
            }
        }
        G.has_freq = G.ps.tot > 0;
        G.active_res = G.ps.tot > 0 ? act / G.ps.tot * 100 : 0;
        G.mhz = fres > 0 ? fsum / fres : 0;
    }
}

/* AppleSMC user client: GPU die temperatures are the "Tg.." float keys. */
typedef struct { uint8_t major, minor, build, reserved; uint16_t release; } SmcVers;
typedef struct { uint16_t version, length; uint32_t cpu, gpu, mem; } SmcPLimit;
typedef struct { uint32_t size, type; uint8_t attr; } SmcKeyInfo;
typedef struct {
    uint32_t key;
    SmcVers vers;
    SmcPLimit plimit;
    SmcKeyInfo info;
    uint8_t result, status, data8;
    uint32_t data32;
    uint8_t bytes[32];
} SmcParam;
enum { SMC_READ = 5, SMC_INDEX = 8, SMC_INFO = 9 };
#define KEY4(s) ((uint32_t)(s)[0] << 24 | (uint32_t)(s)[1] << 16 | (uint32_t)(s)[2] << 8 | (uint32_t)(s)[3])

static int smc_call(SmcParam *in, SmcParam *out)
{
    size_t osz = sizeof *out;
    memset(out, 0, sizeof *out);
    return IOConnectCallStructMethod(G.smc, 2, in, sizeof *in, out, &osz) == KERN_SUCCESS && !out->result;
}

/* Reads `key`; ki->size == 0 looks the key's size and type up first. */
static int smc_read(uint32_t key, SmcKeyInfo *ki, SmcParam *out)
{
    SmcParam in;
    memset(&in, 0, sizeof in);
    in.key = key;
    if (!ki->size) {
        in.data8 = SMC_INFO;
        if (!smc_call(&in, out)) return 0;
        *ki = out->info;
    }
    in.info.size = ki->size;
    in.data8 = SMC_READ;
    return smc_call(&in, out);
}

static void smc_init(void)
{
    io_service_t s = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("AppleSMC"));
    SmcParam in, out;
    SmcKeyInfo ki = { 0, 0, 0 };
    uint32_t i, n;
    if (!s) return;
    if (IOServiceOpen(s, mach_task_self(), 0, &G.smc) != KERN_SUCCESS) G.smc = 0;
    IOObjectRelease(s);
    if (!G.smc || !smc_read(KEY4("#KEY"), &ki, &out)) return;
    n = (uint32_t)out.bytes[0] << 24 | (uint32_t)out.bytes[1] << 16 | (uint32_t)out.bytes[2] << 8 | out.bytes[3];
    for (i = 0; i < n && G.ntk < (int)(sizeof G.tkey / sizeof G.tkey[0]); i++) {
        SmcKeyInfo k2 = { 0, 0, 0 };
        uint32_t key;
        memset(&in, 0, sizeof in);
        in.data8 = SMC_INDEX;
        in.data32 = i;
        if (!smc_call(&in, &out)) continue;
        key = out.key;
        if ((key >> 24) != 'T' || ((key >> 16) & 0xff) != 'g') continue;
        if (smc_read(key, &k2, &out) && k2.type == KEY4("flt ") && k2.size == 4) G.tkey[G.ntk++] = key;
    }
}

static void smc_sample(void)
{
    SmcKeyInfo ki = { 4, KEY4("flt "), 0 };
    SmcParam out;
    double sum = 0, mx = 0;
    int i, n = 0;
    for (i = 0; i < G.ntk; i++) {
        float f;
        if (!smc_read(G.tkey[i], &ki, &out)) continue;
        memcpy(&f, out.bytes, sizeof f);
        if (!(f > 1 && f < 150)) continue; /* unpopulated sensors read 0 or garbage */
        sum += f;
        if (f > mx) mx = f;
        n++;
    }
    G.has_temp = n > 0;
    G.ntemp = n;
    G.temp = n ? sum / n : 0;
    G.tmax = mx;
    if (G.primed && mx > G.tmax_peak) G.tmax_peak = mx;
}

/* GPU-tagged memory of a process (Metal heaps, textures, buffers): its
 * graphics ledger, resident plus compressed. Same-user processes or root. */
static int64_t task_gfx(pid_t pid)
{
    mach_port_t t;
    task_vm_info_data_t vi;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    kern_return_t kr;
    if (task_name_for_pid(mach_task_self(), pid, &t) != KERN_SUCCESS) return -1;
    kr = task_info(t, TASK_VM_INFO, (task_info_t)&vi, &n);
    mach_port_deallocate(mach_task_self(), t);
    if (kr != KERN_SUCCESS || n < TASK_VM_INFO_REV3_COUNT) return -1;
    return vi.ledger_tag_graphics_footprint + vi.ledger_tag_graphics_footprint_compressed;
}

static GClient *gcl_get(pid_t pid, long ss, long su)
{
    GClient *c;
    int i, old = -1;
    for (i = 0; i < G.ncl; i++) {
        c = &G.cl[i];
        if (c->pid == pid && c->start_s == ss && c->start_us == su) {
            if (!c->alive) { c->alive = 1; c->fresh = 1; }
            return c;
        }
    }
    if (G.ncl == G.clcap) {
        if (G.clcap >= GMAXCL) {
            for (i = 0; i < G.ncl; i++)
                if (!G.cl[i].alive && (old < 0 || G.cl[i].gone_t < G.cl[old].gone_t)) old = i;
            if (old < 0) return NULL;
            G.cl[old] = G.cl[--G.ncl];
        } else {
            G.clcap = G.clcap ? G.clcap * 2 : 64;
            G.cl = xrealloc(G.cl, (size_t)G.clcap * sizeof *G.cl);
            G.ord = xrealloc(G.ord, (size_t)G.clcap * sizeof *G.ord);
        }
    }
    c = &G.cl[G.ncl++];
    memset(c, 0, sizeof *c);
    c->pid = pid;
    c->start_s = ss;
    c->start_us = su;
    c->alive = c->fresh = 1;
    c->gfx = c->gfx_peak = -1;
    return c;
}

static double gcl_avg(const GClient *c)
{
    double t = (c->alive ? G.t_last : c->gone_t) - c->first_t;
    return t > 0.5 ? (double)(c->ns - c->ns0) / (t * 1e9) * 100.0 : c->cur;
}

static int gcl_cmp(const void *a, const void *b)
{
    const GClient *x = &G.cl[*(const int *)a], *y = &G.cl[*(const int *)b];
    double vx, vy;
    if (x->alive != y->alive) return y->alive - x->alive;
    switch (G.sort) {
    case GS_AVG: vx = gcl_avg(x); vy = gcl_avg(y); break;
    case GS_TIME: vx = (double)(x->ns - x->ns0); vy = (double)(y->ns - y->ns0); break;
    case GS_GFX: vx = (double)x->gfx; vy = (double)y->gfx; break;
    case GS_PID: return x->pid < y->pid ? -1 : x->pid > y->pid;
    default: vx = x->cur; vy = y->cur;
    }
    if (vx != vy) return vx < vy ? 1 : -1;
    if (x->ns != y->ns) return x->ns < y->ns ? 1 : -1;
    return x->pid < y->pid ? -1 : x->pid > y->pid;
}

/* Rebuilds the display order; the selection follows its process. */
static void gpu_order(void)
{
    int i;
    G.nord = G.ncl;
    for (i = 0; i < G.ncl; i++) G.ord[i] = i;
    if (G.nord > 1) qsort(G.ord, (size_t)G.nord, sizeof *G.ord, gcl_cmp);
    for (i = 0; i < G.nord; i++)
        if (G.cl[G.ord[i]].pid == G.sel_pid) { G.sel = i; break; }
    if (G.sel >= G.nord) G.sel = G.nord ? G.nord - 1 : 0;
}

/* Folds this sample's accelerator clients into per-process records. */
static void gpu_clients(double now)
{
    double tot = 0;
    size_t k;
    int i;
    G.gen++;
    for (k = 0; k < S.ngu; k++) {
        const GpuUse *u = &S.gu[k];
        Proc *p = u->pid >= 0 && u->pid < PID_CAP ? bypid[u->pid] : NULL;
        GClient *c = gcl_get(u->pid, p ? p->start_s : 0, p ? p->start_us : 0);
        if (!c) continue;
        if (c->gen != G.gen) {
            c->gen = G.gen;
            c->ns = c->last_sub = 0;
            c->queues = c->qmax = c->nclients = 0;
            c->api = 0;
        }
        if (p) snprintf(c->name, sizeof c->name, "%s", p->name);
        else if (!c->name[0]) snprintf(c->name, sizeof c->name, "pid %d", u->pid);
        c->ns += u->ns;
        if (u->last > c->last_sub) c->last_sub = u->last;
        c->queues += u->queues;
        c->qmax += u->qmax;
        c->nclients++;
        c->api |= u->api;
    }
    for (i = 0; i < G.ncl; i++) {
        GClient *c = &G.cl[i];
        Proc *p = c->pid >= 0 && c->pid < PID_CAP ? bypid[c->pid] : NULL;
        int same = p && p->start_s == c->start_s && p->start_us == c->start_us;
        if (c->gen != G.gen) {
            if (c->alive) {
                c->alive = 0;
                c->gone_t = now;
                c->cur = c->est_w = 0;
                if (G.clients_seen)
                    gpu_event(same ? 0 : 1, "%s (%d) %s", c->name, c->pid, same ? "closed its GPU connections" : "exited");
            }
            continue;
        }
        if (c->fresh) {
            c->fresh = 0;
            c->ns0 = c->ns_prev = c->ns;
            c->t_prev = c->first_t = now;
            c->cur = 0;
            if (G.clients_seen) gpu_event(0, "%s (%d) started GPU work", c->name, c->pid);
        } else if (c->ns < c->ns_prev) { /* a GPU connection closed and took its time with it */
            uint64_t drop = c->ns_prev - c->ns;
            c->ns0 = c->ns0 > drop ? c->ns0 - drop : 0;
            c->ns_prev = c->ns;
            c->t_prev = now;
            c->cur = 0;
        } else if (now - c->t_prev > 0.05) {
            c->cur = (double)(c->ns - c->ns_prev) / ((now - c->t_prev) * 1e9) * 100.0;
            c->ns_prev = c->ns;
            c->t_prev = now;
        }
        if (c->cur > c->peak) c->peak = c->cur;
        c->hist[c->hn % GPH] = (float)c->cur;
        c->hn++;
        c->phys = p && (p->has_task || p->has_ru) ? p->mem : 0;
        c->gfx = task_gfx(c->pid);
        if (c->gfx > c->gfx_peak) c->gfx_peak = c->gfx;
        tot += c->cur;
    }
    for (i = 0; i < G.ncl; i++) {
        GClient *c = &G.cl[i];
        c->est_w = c->alive && G.has_gpu_w && tot > 0 ? G.gpu_w * c->cur / tot : 0;
    }
    G.clients_seen = 1;
    gpu_order();
}

static int fcmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void gm_push(int id, double v, int ok)
{
    GMet *m = &G.m[id];
    float tmp[GHIST];
    int w, i;
    m->h[G.hpos] = ok ? (float)v : 0;
    if (!ok) return;
    m->ok = 1;
    m->cur = v;
    m->sum += v;
    if (!m->n || v < m->mn) m->mn = v;
    if (!m->n || v > m->mx) m->mx = v;
    m->n++;
    w = m->n < GHIST ? (int)m->n : GHIST;
    for (i = 0; i < w; i++) tmp[i] = m->h[(G.hpos - i + GHIST) % GHIST];
    qsort(tmp, (size_t)w, sizeof *tmp, fcmp);
    m->p95 = tmp[(int)ceil(0.95 * w) - 1]; /* nearest rank */
}

static double gm_avg(int id) { return G.m[id].n ? G.m[id].sum / (double)G.m[id].n : 0; }

static uint64_t gpu_mem_limit(void)
{
    return G.wired_mb > 0 ? (uint64_t)G.wired_mb << 20 : 0;
}

static const char *pressure_name(int p) { return p >= 4 ? "critical" : p >= 2 ? "warning" : "normal"; }

/* State transitions worth a line in the event log. */
static void gpu_events(int emit)
{
    char lim[96];
    double th = gst_out(&G.cltm, "NO_CLTM", 0), lf = gpu_limit(lim, sizeof lim, 0);
    uint64_t ml = gpu_mem_limit();
    int thermal = th > 0.001, limit = lf > 0.001, mem = ml && S.gpu_mem >= ml / 10 * 9;
    if (emit) {
        if (thermal != G.was_thermal)
            gpu_event(thermal, thermal ? "Thermal limiting (CLTM) engaged for %.0f%% of the sample" : "Thermal limiting cleared",
                      th * 100);
        if (limit != G.was_limit)
            gpu_event(limit, limit ? "GPU clock held by %s for %.0f%% of active time" : "GPU clock limits cleared", lim, lf * 100);
        if (G.throttle > 0) gpu_event(1, "GPU throttle counter +%ld", G.throttle);
        if (G.restarts > 0) gpu_event(2, "GPU restarted %ld time%s", G.restarts, G.restarts == 1 ? "" : "s");
        if (S.gpu_recov > G.recov) gpu_event(2, "GPU recovery: driver recoveryCount now %lld", (long long)S.gpu_recov);
        if (G.uvwarn > 0) gpu_event(1, "GPU undervoltage warning \xc3\x97%ld", G.uvwarn);
        if (S.pressure != G.was_pressure)
            gpu_event(S.pressure >= 4 ? 2 : S.pressure >= 2, "Memory pressure %s", pressure_name(S.pressure));
        if (mem != G.was_mem) {
            char a[16], b[16];
            fmt_mem(S.gpu_mem, a, sizeof a);
            fmt_mem(ml, b, sizeof b);
            gpu_event(mem, mem ? "GPU memory in use %s reached 90%% of the %s wired limit" : "GPU memory in use back to %s of %s",
                      a, b);
        }
    }
    G.was_thermal = thermal;
    G.was_limit = limit;
    G.was_pressure = S.pressure;
    if (mem || !ml || S.gpu_mem < ml / 100 * 85) G.was_mem = mem; /* hysteresis */
    G.recov = S.gpu_recov;
}

/* Runs after every sample_all once GPU telemetry is active. The first call
 * is a baseline: rates and events start with the second. */
static void gpu_sample(double now)
{
    int primed = G.primed;
    G.wired_mb = sysctl_long("iogpu.wired_limit_mb");
    ior_sample(now);
    smc_sample();
    gpu_clients(S.sample_t);
    gpu_events(primed);
    G.primed = 1;
    G.t_last = now;
    if (!primed) return;
    G.hpos = (G.hpos + 1) % GHIST;
    if (G.hn < GHIST) G.hn++;
    gm_push(GM_UTIL, S.gpu_util * 100, S.has_gpu);
    gm_push(GM_REND, S.gpu_rend, S.gpu_rend >= 0);
    gm_push(GM_TILER, S.gpu_tiler, S.gpu_tiler >= 0);
    gm_push(GM_ACTIVE, G.active_res, G.has_freq);
    gm_push(GM_FREQ, G.mhz, G.has_freq && G.nfreq);
    gm_push(GM_POWER, G.gpu_w, G.has_gpu_w);
    gm_push(GM_TEMP, G.temp, G.has_temp);
    gm_push(GM_TMAX, G.tmax, G.has_temp);
    gm_push(GM_MEM, (double)S.gpu_mem, S.has_gpu);
    gm_push(GM_BW, G.bw_rd + G.bw_wr, G.has_bw);
    gm_push(GM_DRAMW, G.dram_w, G.has_dram_w);
    G.samples++;
}

/* Statistics restart from now; history graphs keep their past. */
static void gpu_reset(void)
{
    GStates *st[] = { &G.ps, &G.pc, &G.cltm, &G.pmu, &G.pz };
    size_t k;
    int i, j;
    for (i = 0; i < GM_N; i++) {
        GMet *m = &G.m[i];
        m->sum = m->mn = m->mx = m->p95 = 0;
        m->n = 0;
    }
    for (k = 0; k < sizeof st / sizeof st[0]; k++) {
        for (j = 0; j < st[k]->n; j++) st[k]->cum[j] = 0;
        st[k]->cumtot = 0;
    }
    G.energy_j = G.dram_j = G.tmax_peak = 0;
    G.throttle_tot = G.restarts_tot = G.uvwarn_tot = G.samples = 0;
    for (i = 0; i < G.ncl;) {
        GClient *c = &G.cl[i];
        if (!c->alive) {
            G.cl[i] = G.cl[--G.ncl];
            continue;
        }
        c->ns0 = c->ns;
        c->first_t = G.t_last;
        c->peak = c->cur;
        c->gfx_peak = c->gfx;
        i++;
    }
    G.t_reset = G.t_last;
    gpu_event(0, "Statistics reset");
    gpu_order();
}

/* Starts GPU telemetry; it then keeps sampling for the rest of the run so
 * session statistics survive closing the view. */
static void gpu_activate(void)
{
    if (G.active) return;
    G.active = 1;
    gpu_static_init();
    gpu_freq_init();
    ior_init();
    smc_init();
    G.recov = S.gpu_recov;
    G.t_reset = G.t_last = now_s();
    gpu_sample(G.t_last);
}

static int gcl_match(const GClient *c, const char *q)
{
    char *end;
    long v;
    if (!q || !q[0]) return 1;
    v = strtol(q, &end, 10);
    if (!*end) return c->pid == (pid_t)v;
    return strcasestr(c->name, q) != NULL;
}

/* The pinned focus process, else the busiest one matching -p, else the
 * busiest one. */
static GClient *gpu_focus(void)
{
    GClient *best = NULL, *bq = NULL;
    int i;
    for (i = 0; i < G.ncl; i++) {
        GClient *c = &G.cl[i];
        if (G.focus >= 0 && c->pid == G.focus && c->start_s == G.focus_s && c->start_us == G.focus_us) return c;
        if (!c->alive) continue;
        if (!best || c->cur > best->cur || (c->cur == best->cur && c->ns > best->ns)) best = c;
        if (G.query && gcl_match(c, G.query) && (!bq || c->cur > bq->cur || (c->cur == bq->cur && c->ns > bq->ns))) bq = c;
    }
    return bq ? bq : best;
}

/* "12 ms", "3.4 s", "5m 02s" since a lastSubmittedTime */
static void fmt_ago(uint64_t last, char *o, size_t sz)
{
    uint64_t now = tk2ns(mach_absolute_time());
    double s = now > last ? (double)(now - last) / 1e9 : 0;
    if (!last) snprintf(o, sz, "-");
    else if (s < 1) snprintf(o, sz, "%.0f ms", s * 1000);
    else if (s < 60) snprintf(o, sz, "%.1f s", s);
    else fmt_dur(s, o, sz);
}

static void api_str(unsigned a, char *o, size_t sz)
{
    snprintf(o, sz, "%s%s%s%s", a & API_METAL ? "Metal+" : "", a & API_GL ? "GL+" : "", a & API_CL ? "CL+" : "",
             a & API_OTHER ? "other+" : "");
    if (o[0]) o[strlen(o) - 1] = 0;
    else snprintf(o, sz, "-");
}

static void gm_fmt(int id, double v, char *o, size_t sz)
{
    switch (id) {
    case GM_FREQ: snprintf(o, sz, "%.0f MHz", v); break;
    case GM_POWER: case GM_DRAMW: fmt_power(v, o, sz); break;
    case GM_TEMP: case GM_TMAX: snprintf(o, sz, "%.1f\xc2\xb0" "C", v); break;
    case GM_MEM: fmt_mem((uint64_t)(v < 0 ? 0 : v), o, sz); break;
    case GM_BW: snprintf(o, sz, "%.1f GB/s", v); break;
    default: snprintf(o, sz, "%.0f%%", v);
    }
}

/* "avg 45% · p95 80% · max 98%" since reset */
static void gm_stats(int id, char *o, size_t sz)
{
    char a[24], b[24], c[24];
    if (!G.m[id].n) { snprintf(o, sz, "no samples yet"); return; }
    gm_fmt(id, gm_avg(id), a, sizeof a);
    gm_fmt(id, G.m[id].p95, b, sizeof b);
    gm_fmt(id, G.m[id].mx, c, sizeof c);
    snprintf(o, sz, "avg %s \xc2\xb7 p95 %s \xc2\xb7 max %s", a, b, c);
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
    if (G.active) gpu_sample(now);
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

/* Row r of an h-row sparkline over a ring of `cap` samples whose newest is
 * at `pos` and which holds `n` samples; newest on the right. */
static void spark_row(int y, int x, int w, int h, int r, const float *ring, int pos, int n, int cap, double vmax, int bg)
{
    int i;
    for (i = 0; i < w; i++) {
        int age = w - 1 - i;
        double v, lv;
        if (age >= n) {
            set_cell(y, x + i, ST(P_GUIDE, bg, 0), r == h - 1 ? "\xc2\xb7" : " ", r == h - 1 ? 2 : 1, 1);
            continue;
        }
        v = ring[(pos - age + cap) % cap];
        v = vmax > 0 ? clampd(v / vmax, 0, 1) : 0;
        lv = clampd(v * h * 8 - (double)(h - 1 - r) * 8, 0, 8);
        set_cell(y, x + i, ST(P_BAR, bg, 0), LEVEL[(int)(lv + 0.5)], (int)(lv + 0.5) ? 3 : 1, 1);
    }
}

/* Sparkline of a system history ring (S.hpos / S.hn); h rows tall. */
static void spark(int y, int x, int w, int h, const float *ring, double vmax)
{
    int r;
    for (r = 0; r < h; r++) spark_row(y + r, x, w, h, r, ring, S.hpos, S.hn, HIST, vmax, CDEF);
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
    { "0", "Quit", K_F(10) },  { "g", "GPU", 'g' }
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
static void draw_gpu(double now);

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
    draw_gpu(now);
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
/* GPU view                                                            */
/* ------------------------------------------------------------------ */

#define GKW 18 /* key column width */

/* Rows laid out top to bottom in a scrolled area. */
typedef struct { int x, y, w, h, row, scroll; } GCtx;

/* Screen line of logical row `row + off`, -1 when scrolled out. */
static int gc_y(const GCtx *c, int off)
{
    int r = c->row + off - c->scroll;
    return r >= 0 && r < c->h ? c->y + r : -1;
}

static void gc_head(GCtx *c, const char *t)
{
    int y, u, i;
    if (c->row) c->row++;
    if ((y = gc_y(c, 0)) >= 0) {
        u = scr_put(y, c->x, c->w, ST(P_ACCENT, P_BOX, A_BOLD), t);
        for (i = u + 1; i < c->w; i++) set_cell(y, c->x + i, ST(P_GUIDE, P_BOX, 0), "\xe2\x94\x80", 3, 1);
    }
    c->row++;
}

static void gc_kv(GCtx *c, int col, const char *k, const char *fmt, ...)
{
    int y = gc_y(c, 0);
    if (y >= 0) {
        char v[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(v, sizeof v, fmt, ap);
        va_end(ap);
        scr_put(y, c->x, GKW - 1, ST(P_DIM, P_BOX, 0), k);
        scr_put(y, c->x + GKW, c->w - GKW, ST(col ? col : P_TEXT, P_BOX, 0), v);
    }
    c->row++;
}

/* label, bar (f < 0: empty), right-aligned value, faint stats */
static void gc_meter(GCtx *c, const char *k, double f, int bcol, const char *val, int vcol, const char *stats)
{
    int y = gc_y(c, 0), vw = 22, sw = stats && c->w >= 104 ? 42 : 0, bw = c->w - GKW - 1 - vw - (sw ? sw + 1 : 0);
    if (y < 0) { c->row++; return; }
    if (bw < 4) bw = 0;
    scr_put(y, c->x, GKW - 1, ST(P_SOFT, P_BOX, A_BOLD), k);
    if (bw) {
        Seg sg;
        sg.v = clampd(f, 0, 1);
        sg.col = bcol;
        seg_bar(y, c->x + GKW, bw, &sg, f >= 0, P_BOX);
    }
    scr_rput(y, c->x + GKW + bw + 1, vw, ST(vcol, P_BOX, A_BOLD), val);
    if (sw) scr_put(y, c->x + GKW + bw + 1 + vw + 2, sw - 1, ST(P_FAINT, P_BOX, 0), stats);
    c->row++;
}

/* A metric's meter with its session stats; util-like metrics colour by load. */
static void gc_metric(GCtx *c, int id, double f, const char *val, int load)
{
    char st[128];
    if (!G.m[id].ok) {
        gc_meter(c, GMDEF[id].name, -1, P_BAR, "n/a", P_FAINT, NULL);
        return;
    }
    gm_stats(id, st, sizeof st);
    gc_meter(c, GMDEF[id].name, f, load ? grad_col(f) : P_BAR, val, load ? cpu_val_col(f * 100) : P_TEXT, st);
}

static double gm_ringmax(int id, double floor)
{
    double mx = floor;
    int i;
    for (i = 0; i < G.hn; i++) {
        float v = G.m[id].h[(G.hpos - i + GHIST) % GHIST];
        if (v > mx) mx = v;
    }
    return mx;
}

/* Three rows: name, value and stats, then a 2-row history graph. */
static void gc_spark_at(GCtx *c, int xo, int w, int id, double vmax)
{
    const GMet *m = &G.m[id];
    char cur[32], st[128];
    int y, r, u;
    if ((y = gc_y(c, 0)) >= 0) {
        if (m->ok) gm_fmt(id, m->cur, cur, sizeof cur);
        else snprintf(cur, sizeof cur, "n/a");
        u = scr_put(y, c->x + xo, w, ST(P_SOFT, P_BOX, A_BOLD), GMDEF[id].name) + 1;
        u += scr_put(y, c->x + xo + u, w - u, ST(m->ok ? P_TEXT : P_FAINT, P_BOX, A_BOLD), cur) + 1;
        if (m->n && w - u > 12) {
            gm_stats(id, st, sizeof st);
            scr_rput(y, c->x + xo + u, w - u, ST(P_FAINT, P_BOX, 0), st);
        }
    }
    for (r = 0; r < 2; r++)
        if ((y = gc_y(c, 1 + r)) >= 0) spark_row(y, c->x + xo, w, 2, r, m->h, G.hpos, G.hn, GHIST, vmax, P_BOX);
}

/* Two graphs side by side when there is room; b < 0 for one. */
static void gc_sparks(GCtx *c, int a, double va, int b, double vb)
{
    if (c->w >= 110 && b >= 0) {
        int w1 = (c->w - 3) / 2;
        gc_spark_at(c, 0, w1, a, va);
        gc_spark_at(c, w1 + 3, c->w - w1 - 3, b, vb);
        c->row += 4;
        return;
    }
    gc_spark_at(c, 0, c->w, a, va);
    c->row += 4;
    if (b >= 0) {
        gc_spark_at(c, 0, c->w, b, vb);
        c->row += 4;
    }
}

/* process table columns; NAME takes what is left after PID */
enum { PC_PID, PC_GPU, PC_AVG, PC_PEAK, PC_TIME, PC_W, PC_Q, PC_GFX, PC_MEM, PC_LAST, PC_API, PC_N };
static const struct { const char *name; int w, pri; } PCOL[PC_N] = {
    { "PID", 7, 0 },       { "GPU%", 6, 0 },     { "AVG%", 6, 1 },   { "PEAK%", 6, 4 },
    { "GPU TIME", 10, 2 }, { "EST W", 8, 5 },    { "QUEUES", 7, 6 }, { "GFX MEM", 8, 3 },
    { "FOOTPRINT", 9, 5 }, { "LAST SUBMIT", 11, 3 }, { "API", 9, 7 },
};
typedef struct { int on[PC_N], x[PC_N], namex, namew; } PLayout;

static void pcol_layout(int w, PLayout *L)
{
    int pri, i, need, x;
    memset(L, 0, sizeof *L);
    for (pri = 0; pri <= 7; pri++) {
        need = 2 + 18;
        for (i = 0; i < PC_N; i++)
            if (L->on[i] || PCOL[i].pri == pri) need += PCOL[i].w + 1;
        if (need > w) break;
        for (i = 0; i < PC_N; i++)
            if (PCOL[i].pri == pri) L->on[i] = 1;
    }
    L->x[PC_PID] = 2;
    L->namex = 2 + PCOL[PC_PID].w + 2;
    need = 0;
    for (i = 1; i < PC_N; i++)
        if (L->on[i]) need += PCOL[i].w + 1;
    L->namew = w - L->namex - need;
    if (L->namew < 4) L->namew = 4;
    x = L->namex + L->namew;
    for (i = 1; i < PC_N; i++)
        if (L->on[i]) {
            L->x[i] = x + 1;
            x += PCOL[i].w + 1;
        }
}

static int gs_col(int s) { return s == GS_AVG ? PC_AVG : s == GS_TIME ? PC_TIME : s == GS_GFX ? PC_GFX : s == GS_PID ? PC_PID : PC_GPU; }

static void gpu_phdr(int y, int x, int w, const PLayout *L)
{
    int i, sc = gs_col(G.sort);
    St s = ST(P_DIM, P_SEL, A_BOLD), on = ST(P_BRIGHT, P_ACC_BG, A_BOLD);
    scr_fill(y, x, w, s);
    for (i = 0; i < PC_N; i++)
        if (L->on[i]) scr_rput(y, x + L->x[i], PCOL[i].w, i == sc ? on : s, PCOL[i].name);
    scr_put(y, x + L->namex, L->namew, s, "NAME");
}

static void gpu_prow(int y, int x, int w, const PLayout *L, const GClient *c, int sel, int focus)
{
    int bg = sel ? P_SEL : P_BOX, base = c->alive ? P_SOFT : P_FAINT, i;
    char t[48];
    scr_fill(y, x, w, ST(base, bg, 0));
    if (focus) scr_put(y, x, 2, ST(P_ACCENT, bg, A_BOLD), "\xe2\x97\x86");
    else if (sel) scr_put(y, x, 2, ST(P_ACCENT, bg, A_BOLD), "\xe2\x96\xb6");
    for (i = 0; i < PC_N; i++) {
        int col = base;
        if (!L->on[i]) continue;
        switch (i) {
        case PC_PID: snprintf(t, sizeof t, "%d", c->pid); col = P_DIM; break;
        case PC_GPU:
            snprintf(t, sizeof t, c->cur < 100 ? "%.1f" : "%.0f", c->cur);
            col = c->alive ? cpu_val_col(c->cur) : P_FAINT;
            break;
        case PC_AVG: snprintf(t, sizeof t, "%.1f", gcl_avg(c)); col = c->alive ? cpu_val_col(gcl_avg(c)) : P_FAINT; break;
        case PC_PEAK: snprintf(t, sizeof t, "%.1f", c->peak); break;
        case PC_TIME: fmt_cputime(c->ns - c->ns0, t, sizeof t); col = P_DIM; break;
        case PC_W:
            if (c->alive && G.has_gpu_w) fmt_power(c->est_w, t, sizeof t);
            else snprintf(t, sizeof t, "-");
            break;
        case PC_Q: snprintf(t, sizeof t, "%d/%d", c->queues, c->qmax); col = P_DIM; break;
        case PC_GFX:
            if (c->gfx >= 0) fmt_mem((uint64_t)c->gfx, t, sizeof t);
            else snprintf(t, sizeof t, "-");
            break;
        case PC_MEM:
            if (c->phys) fmt_mem(c->phys, t, sizeof t);
            else snprintf(t, sizeof t, "-");
            col = P_DIM;
            break;
        case PC_LAST: fmt_ago(c->alive ? c->last_sub : 0, t, sizeof t); col = P_DIM; break;
        case PC_API: api_str(c->api, t, sizeof t); col = P_DIM; break;
        }
        if (!c->alive) col = P_FAINT;
        scr_rput(y, x + L->x[i], PCOL[i].w, ST(col, bg, 0), t);
    }
    i = scr_put(y, x + L->namex, L->namew, ST(c->alive ? P_BRIGHT : P_DIM, bg, c->alive ? A_BOLD : A_STRIKE), c->name);
    if (!c->alive && i + 9 < L->namew) scr_put(y, x + L->namex + i + 1, L->namew - i - 1, ST(P_HOT, bg, 0), "gone");
}

/* Name, stats and history of one process. */
static void gpu_detail(GCtx *c, const GClient *fc)
{
    char a[32], b[32], d[32], q[32], t[96];
    int y, r;
    double mx = 100;
    for (r = 0; r < GPH && r < fc->hn; r++)
        if (fc->hist[r] > mx) mx = fc->hist[r];
    gc_kv(c, P_TEXT, "GPU", "%.1f%% now \xc2\xb7 %.1f%% avg \xc2\xb7 %.1f%% peak", fc->cur, gcl_avg(fc), fc->peak);
    fmt_cputime(fc->ns, a, sizeof a);
    fmt_cputime(fc->ns - fc->ns0, b, sizeof b);
    t[0] = 0;
    if (G.has_gpu_w && fc->alive) {
        fmt_power(fc->est_w, d, sizeof d);
        snprintf(t, sizeof t, " \xc2\xb7 est. power %s (its share of GPU time)", d);
    }
    gc_kv(c, P_TEXT, "GPU time", "%s total \xc2\xb7 %s this session%s", a, b, t);
    fmt_mem(fc->phys, d, sizeof d);
    if (fc->gfx >= 0) {
        fmt_mem((uint64_t)fc->gfx, a, sizeof a);
        fmt_mem(fc->gfx_peak > 0 ? (uint64_t)fc->gfx_peak : 0, b, sizeof b);
        snprintf(t, sizeof t, "%s (peak %s)", a, b);
    } else {
        snprintf(t, sizeof t, "n/a (another user's process: run with sudo)");
    }
    gc_kv(c, P_TEXT, "Memory", "graphics %s \xc2\xb7 footprint %s", t, fc->phys ? d : "-");
    api_str(fc->api, q, sizeof q);
    if (fc->alive) {
        fmt_ago(fc->last_sub, a, sizeof a);
        gc_kv(c, P_TEXT, "Submission", "last %s ago \xc2\xb7 %d command queue%s (max %d) \xc2\xb7 %d GPU connection%s \xc2\xb7 %s", a,
              fc->queues, fc->queues == 1 ? "" : "s", fc->qmax, fc->nclients, fc->nclients == 1 ? "" : "s", q);
    } else {
        fmt_dur(G.t_last - fc->gone_t, a, sizeof a);
        gc_kv(c, P_HOT, "Submission", "gone %s ago \xc2\xb7 %s", a, q);
    }
    if ((y = gc_y(c, 0)) >= 0) scr_put(y, c->x, GKW - 1, ST(P_DIM, P_BOX, 0), "GPU% history");
    for (r = 0; r < 2; r++)
        if ((y = gc_y(c, r)) >= 0)
            spark_row(y, c->x + GKW, c->w - GKW, 2, r, fc->hist, fc->hn ? (fc->hn - 1) % GPH : 0,
                      fc->hn < GPH ? fc->hn : GPH, GPH, mx, P_BOX);
    c->row += 2;
}

static void gpu_tab_overview(GCtx *c)
{
    char v[64], a[16], b[16];
    uint64_t ml = gpu_mem_limit(), lim = ml ? ml : S.mem_total;
    double f;
    GClient *fc;
    PLayout L;
    int i, n, y;

    gc_head(c, "Now");
    gm_fmt(GM_UTIL, S.gpu_util * 100, v, sizeof v);
    gc_metric(c, GM_UTIL, S.gpu_util, v, 1);
    gm_fmt(GM_REND, S.gpu_rend, v, sizeof v);
    gc_metric(c, GM_REND, S.gpu_rend / 100, v, 1);
    gm_fmt(GM_TILER, S.gpu_tiler, v, sizeof v);
    gc_metric(c, GM_TILER, S.gpu_tiler / 100, v, 1);
    gm_fmt(GM_ACTIVE, G.active_res, v, sizeof v);
    gc_metric(c, GM_ACTIVE, G.active_res / 100, v, 1);
    if (G.mhz > 0) snprintf(v, sizeof v, "%.0f / %.0f MHz", G.mhz, G.fmax);
    else snprintf(v, sizeof v, "idle");
    gc_metric(c, GM_FREQ, G.fmax > 0 ? G.mhz / G.fmax : 0, v, 0);
    fmt_power(G.gpu_w, v, sizeof v);
    gc_metric(c, GM_POWER, G.gpu_w / gm_ringmax(GM_POWER, 1), v, 0);
    snprintf(v, sizeof v, "%.1f\xc2\xb0" "C max %.1f", G.temp, G.tmax);
    gc_metric(c, GM_TEMP, G.tmax / 110, v, 0);
    fmt_mem(S.gpu_mem, a, sizeof a);
    fmt_mem(lim, b, sizeof b);
    snprintf(v, sizeof v, "%s / %s%s", a, b, ml ? "" : " RAM");
    f = lim ? (double)S.gpu_mem / (double)lim : 0;
    gc_metric(c, GM_MEM, f, v, 1);
    gm_fmt(GM_BW, G.bw_rd + G.bw_wr, v, sizeof v);
    gc_metric(c, GM_BW, (G.bw_rd + G.bw_wr) / gm_ringmax(GM_BW, 1), v, 0);
    if (G.has_dram_w) {
        fmt_power(G.dram_w, v, sizeof v);
        gc_metric(c, GM_DRAMW, G.dram_w / gm_ringmax(GM_DRAMW, 1), v, 0);
    }

    gc_head(c, "History");
    gc_sparks(c, GM_UTIL, 100, GM_POWER, gm_ringmax(GM_POWER, 1));
    gc_sparks(c, GM_FREQ, G.fmax > 0 ? G.fmax : gm_ringmax(GM_FREQ, 1), GM_TMAX, 110);
    gc_sparks(c, GM_MEM, (double)lim, GM_BW, gm_ringmax(GM_BW, 1));

    gc_head(c, "GPU processes");
    pcol_layout(c->w, &L);
    if ((y = gc_y(c, 0)) >= 0) gpu_phdr(y, c->x, c->w, &L);
    c->row++;
    fc = gpu_focus();
    for (i = 0, n = 0; i < G.nord && n < 8; i++) {
        const GClient *cl = &G.cl[G.ord[i]];
        if (!cl->alive) continue;
        if ((y = gc_y(c, 0)) >= 0) gpu_prow(y, c->x, c->w, &L, cl, 0, cl == fc);
        c->row++;
        n++;
    }
    if (!n) gc_kv(c, P_FAINT, "", "no process is using the GPU");

    if (fc) {
        char t[160];
        snprintf(t, sizeof t, "Focus \xc2\xb7 %s (%d) \xc2\xb7 %s", fc->name, fc->pid,
                 G.focus >= 0 ? "pinned" : G.query ? "matching -p" : "busiest");
        gc_head(c, t);
        gpu_detail(c, fc);
    }
}

static void gpu_tab_procs(int x, int y, int w, int h)
{
    PLayout L;
    GClient *fc = gpu_focus();
    int detail = h >= 20 ? 8 : 0, lh = h - 1 - detail, i;
    pcol_layout(w, &L);
    gpu_phdr(y, x, w, &L);
    G.list_y = y + 1;
    G.list_h = lh;
    if (G.sel >= G.nord) G.sel = G.nord ? G.nord - 1 : 0;
    if (G.sel < 0) G.sel = 0;
    if (G.top > G.sel) G.top = G.sel;
    if (G.sel >= G.top + lh) G.top = G.sel - lh + 1;
    if (G.top > G.nord - lh) G.top = G.nord - lh;
    if (G.top < 0) G.top = 0;
    for (i = 0; i < lh && G.top + i < G.nord; i++) {
        const GClient *c = &G.cl[G.ord[G.top + i]];
        gpu_prow(y + 1 + i, x, w, &L, c, G.top + i == G.sel, c == fc);
    }
    if (!G.nord) scr_put(y + 1, x + 2, w - 2, ST(P_FAINT, P_BOX, A_ITAL), "(no process has used the GPU yet)");
    if (G.nord) G.sel_pid = G.cl[G.ord[G.sel]].pid;
    if (detail && G.nord) {
        const GClient *c = &G.cl[G.ord[G.sel]];
        GCtx d = { x, y + 1 + lh, w, detail, 0, 0 };
        char t[160];
        snprintf(t, sizeof t, "%s (%d)%s", c->name, c->pid, c == fc && G.focus >= 0 ? " \xc2\xb7 pinned focus" : "");
        gc_head(&d, t);
        gpu_detail(&d, c);
    }
}

static void gst_rows(GCtx *c, const GStates *st, int skip_idle)
{
    double act = 0, cact = 0;
    int i;
    for (i = 0; i < st->n; i++)
        if (!skip_idle || strcmp(st->name[i], "IDLE_OFF")) { act += st->res[i]; cact += st->cum[i]; }
    for (i = 0; i < st->n; i++) {
        char v[32], s[64];
        double f = act > 0 ? st->res[i] / act : 0, cf = cact > 0 ? st->cum[i] / cact : 0;
        int lim = pc_limiting(st->name[i]);
        if ((skip_idle && !strcmp(st->name[i], "IDLE_OFF")) || st->cum[i] <= 0) continue;
        snprintf(v, sizeof v, "%.1f%%", f * 100);
        snprintf(s, sizeof s, "session %.1f%%%s", cf * 100, lim ? " \xc2\xb7 limiting" : "");
        gc_meter(c, st->name[i], f, lim ? P_WARN : P_BAR, v, lim && f > 0 ? P_WARN : P_TEXT, s);
    }
}

static void gpu_tab_clock(GCtx *c)
{
    char a[32], b[32], d[32], lim[96];
    double lf, slf, csum = 0, cres = 0;
    int i;
    gc_head(c, "Clock (GPU performance states)");
    if (!G.ior) {
        gc_kv(c, P_WARN, "", "IOReport is unavailable: no clocks, power or limits on this Mac");
    } else {
        for (i = 0; i < G.ps.n; i++) {
            int p = gst_pnum(G.ps.name[i]);
            if (p > 0 && p <= G.nfreq) { csum += G.freq[p - 1] * G.ps.cum[i]; cres += G.ps.cum[i]; }
        }
        if (G.mhz > 0) gc_kv(c, P_TEXT, "Now", "%.0f MHz average while active \xc2\xb7 %.1f%% active residency \xc2\xb7 max %.0f MHz",
                             G.mhz, G.active_res, G.fmax);
        else gc_kv(c, P_DIM, "Now", "idle \xc2\xb7 max %.0f MHz", G.fmax);
        if (cres > 0) gc_kv(c, P_TEXT, "Session", "%.0f MHz average while active \xc2\xb7 %.1f%% active residency", csum / cres,
                            G.ps.cumtot > 0 ? 100 - gst_frac(&G.ps, "OFF", 1) * 100 : 0);
        for (i = 0; i < G.ps.n; i++) {
            char k[32], v[32], s[64];
            int p = gst_pnum(G.ps.name[i]);
            double f = G.ps.tot > 0 ? G.ps.res[i] / G.ps.tot : 0, cf = G.ps.cumtot > 0 ? G.ps.cum[i] / G.ps.cumtot : 0;
            if (p > G.nfreq && G.ps.cum[i] <= 0) continue;
            if (p > 0 && p <= G.nfreq) snprintf(k, sizeof k, "%-4s %5.0f MHz", G.ps.name[i], G.freq[p - 1]);
            else snprintf(k, sizeof k, "%s", p ? G.ps.name[i] : "idle (off)");
            snprintf(v, sizeof v, "%.1f%%", f * 100);
            snprintf(s, sizeof s, "session %.1f%%", cf * 100);
            gc_meter(c, k, f, p ? P_BAR : P_GUIDE, v, f > 0 ? P_TEXT : P_FAINT, s);
        }
    }

    gc_head(c, "Power");
    fmt_power(G.gpu_w, a, sizeof a);
    fmt_power(gm_avg(GM_POWER), b, sizeof b);
    fmt_power(G.m[GM_POWER].mx, d, sizeof d);
    if (G.has_gpu_w) gc_kv(c, P_TEXT, "GPU", "%s now \xc2\xb7 %s avg \xc2\xb7 %s peak", a, b, d);
    else gc_kv(c, P_FAINT, "GPU", "n/a");
    fmt_dur(G.t_last - G.t_reset, a, sizeof a);
    if (G.has_gpu_w) gc_kv(c, P_TEXT, "GPU energy", "%.1f J (%.4f Wh) in %s", G.energy_j, G.energy_j / 3600, a);
    if (G.has_dram_w) {
        fmt_power(G.dram_w, a, sizeof a);
        gc_kv(c, P_TEXT, "DRAM", "%s now \xc2\xb7 %.1f J this session", a, G.dram_j);
    }
    if (G.has_ane_w) { fmt_power(G.ane_w, a, sizeof a); gc_kv(c, P_TEXT, "ANE", "%s", a); }
    if (G.has_cpu_w) { fmt_power(G.cpu_w, a, sizeof a); gc_kv(c, P_TEXT, "CPU", "%s", a); }
    if (S.sys_w >= 0) { fmt_power(S.sys_w, a, sizeof a); gc_kv(c, P_TEXT, "System", "%s (battery/adapter)", a); }
    if (G.has_ppm) gc_kv(c, G.ppm < 99.5 ? P_WARN : P_TEXT, "Power target", "%.0f%% of maximum GPU power", G.ppm);

    gc_head(c, "Limits & throttling");
    lf = gpu_limit(lim, sizeof lim, 0);
    slf = gpu_limit(NULL, 0, 1);
    gc_kv(c, lf > 0 ? P_WARN : P_TEXT, "Clock limited", "%.1f%% of active time%s%s%s \xc2\xb7 session %.1f%%", lf * 100,
          lim[0] ? " by " : "", lim, "", slf * 100);
    gc_kv(c, gst_out(&G.cltm, "NO_CLTM", 0) > 0 ? P_WARN : P_TEXT, "Thermal (CLTM)", "%.1f%% now \xc2\xb7 session %.1f%%",
          gst_out(&G.cltm, "NO_CLTM", 0) * 100, gst_out(&G.cltm, "NO_CLTM", 1) * 100);
    gc_kv(c, gst_out(&G.pmu, "NONE", 0) > 0 ? P_WARN : P_TEXT, "Perf lost (PMU)", "%.1f%% now \xc2\xb7 session %.1f%%",
          gst_out(&G.pmu, "NONE", 0) * 100, gst_out(&G.pmu, "NONE", 1) * 100);
    gc_kv(c, gst_out(&G.pz, "NO_ZONE", 0) > 0 ? P_WARN : P_TEXT, "Power zones", "%.1f%% now \xc2\xb7 session %.1f%%",
          gst_out(&G.pz, "NO_ZONE", 0) * 100, gst_out(&G.pz, "NO_ZONE", 1) * 100);
    gc_kv(c, G.throttle_tot ? P_WARN : P_TEXT, "Throttle events", "%ld this sample \xc2\xb7 %ld this session", G.throttle,
          G.throttle_tot);
    gc_kv(c, P_DIM, "", "Power controller residency over active time:");
    gst_rows(c, &G.pc, 1);

    gc_head(c, "Temperature (SMC GPU sensors)");
    if (G.has_temp)
        gc_kv(c, G.tmax >= 95 ? P_HOT : G.tmax >= 85 ? P_WARN : P_TEXT, "GPU", "avg %.1f\xc2\xb0" "C \xc2\xb7 max %.1f\xc2\xb0" "C \xc2\xb7 session max %.1f\xc2\xb0" "C \xc2\xb7 %d sensors",
              G.temp, G.tmax, G.tmax_peak, G.ntemp);
    else gc_kv(c, P_FAINT, "GPU", "no GPU temperature sensors found");
}

static void gpu_tab_mem(GCtx *c)
{
    char a[32], b[32], d[32], v[64], st[128];
    uint64_t ml = gpu_mem_limit(), lim = ml ? ml : S.mem_total;
    int i, n;
    gc_head(c, "GPU memory (driver)");
    fmt_mem(S.gpu_mem, a, sizeof a);
    fmt_mem(lim, b, sizeof b);
    snprintf(v, sizeof v, "%s / %s", a, b);
    gm_stats(GM_MEM, st, sizeof st);
    gc_meter(c, "In use", lim ? (double)S.gpu_mem / (double)lim : 0, grad_col(lim ? (double)S.gpu_mem / (double)lim : 0), v,
             P_TEXT, st);
    fmt_mem(S.gpu_alloc, a, sizeof a);
    gc_kv(c, P_TEXT, "Allocated", "%s", a);
    fmt_mem(S.gpu_drv, a, sizeof a);
    gc_kv(c, P_TEXT, "Driver", "%s", a);
    fmt_mem(S.gpu_pb, a, sizeof a);
    fmt_mem(S.gpu_tiled, b, sizeof b);
    gc_kv(c, P_TEXT, "Parameter buffer", "%s \xc2\xb7 tiled scenes %s \xc2\xb7 %lld split scenes", a, b, (long long)S.gpu_split);
    if (ml) {
        fmt_mem(ml > S.gpu_mem ? ml - S.gpu_mem : 0, a, sizeof a);
        gc_kv(c, P_TEXT, "Wired limit", "%ld MB (sysctl iogpu.wired_limit_mb) \xc2\xb7 %s headroom", G.wired_mb, a);
    } else {
        gc_kv(c, P_TEXT, "Wired limit", "macOS default (sysctl iogpu.wired_limit_mb = 0)");
    }
    gc_kv(c, S.gpu_recov ? P_HOT : P_TEXT, "Recoveries", "%lld (driver recoveryCount) \xc2\xb7 %ld GPU restarts this session",
          (long long)S.gpu_recov, G.restarts_tot);

    gc_head(c, "Unified memory (system)");
    fmt_mem(S.mem_used, a, sizeof a);
    fmt_mem(S.mem_total, b, sizeof b);
    snprintf(v, sizeof v, "%s / %s", a, b);
    gc_meter(c, "Used", S.mem_total ? (double)S.mem_used / (double)S.mem_total : 0,
             S.pressure >= 4 ? P_HOT : S.pressure >= 2 ? P_WARN : P_BAR, v, P_TEXT, NULL);
    fmt_mem(S.mem_app, a, sizeof a);
    fmt_mem(S.mem_wired, b, sizeof b);
    fmt_mem(S.mem_comp, d, sizeof d);
    gc_kv(c, P_TEXT, "Breakdown", "app %s \xc2\xb7 wired %s \xc2\xb7 compressed %s", a, b, d);
    fmt_mem(S.swap_used, a, sizeof a);
    fmt_mem(S.swap_total, b, sizeof b);
    gc_kv(c, S.swap_used ? P_WARN : P_TEXT, "Swap", "%s / %s", a, b);
    gc_kv(c, S.pressure >= 4 ? P_HOT : S.pressure >= 2 ? P_WARN : P_TEXT, "Pressure", "%s", pressure_name(S.pressure));

    gc_head(c, "Bandwidth (DRAM controllers, whole SoC)");
    if (G.has_bw) {
        gm_fmt(GM_BW, G.bw_rd + G.bw_wr, v, sizeof v);
        gc_metric(c, GM_BW, (G.bw_rd + G.bw_wr) / gm_ringmax(GM_BW, 1), v, 0);
        gc_kv(c, P_TEXT, "Read / write", "%.1f GB/s read \xc2\xb7 %.1f GB/s write", G.bw_rd, G.bw_wr);
    } else {
        gc_kv(c, P_FAINT, "DRAM", "no memory controller counters on this Mac");
    }
    if (G.has_lat) gc_kv(c, P_TEXT, "GPU mem latency", "%.0f cycles average", G.lat);

    gc_head(c, "GPU processes by graphics memory");
    {
        int idx[GMAXCL], k;
        for (i = 0, n = 0; i < G.ncl; i++)
            if (G.cl[i].alive && G.cl[i].gfx > 0) idx[n++] = i;
        for (i = 1; i < n; i++) /* insertion sort, largest first */
            for (k = i; k > 0 && G.cl[idx[k]].gfx > G.cl[idx[k - 1]].gfx; k--) {
                int t = idx[k];
                idx[k] = idx[k - 1];
                idx[k - 1] = t;
            }
        for (i = 0; i < n && i < 12; i++) {
            const GClient *cl = &G.cl[idx[i]];
            char k2[64];
            fmt_mem((uint64_t)cl->gfx, a, sizeof a);
            fmt_mem((uint64_t)cl->gfx_peak, b, sizeof b);
            fmt_mem(cl->phys, d, sizeof d);
            snprintf(k2, sizeof k2, "%s", cl->name);
            gc_kv(c, P_TEXT, k2, "%s graphics (peak %s) \xc2\xb7 footprint %s \xc2\xb7 pid %d", a, b, cl->phys ? d : "-", cl->pid);
        }
        if (!n) gc_kv(c, P_FAINT, "", "none readable (other users' processes need sudo)");
    }
}

static void gpu_tab_events(GCtx *c)
{
    unsigned long e;
    int any = 0;
    gc_head(c, "Events (newest first)");
    for (e = G.nev; e > 0 && G.nev - e < GNEV; e--) {
        const GEvent *ev = &G.ev[(e - 1) % GNEV];
        struct tm tm;
        char ts[16];
        localtime_r(&ev->t, &tm);
        strftime(ts, sizeof ts, "%H:%M:%S", &tm);
        gc_kv(c, ev->sev >= 2 ? P_HOT : ev->sev ? P_WARN : P_SOFT, ts, "%s", ev->s);
        any = 1;
    }
    if (!any) gc_kv(c, P_FAINT, "", "nothing yet: processes starting or stopping GPU work, clock limits, thermal");
    if (!any) gc_kv(c, P_FAINT, "", "limiting, throttling, GPU restarts and memory pressure show up here");
}

static void draw_gpu(double now)
{
    int bw = SC.w, bh = SC.h - 2, w, h, x, y, i, cx, cy, cw, ch, chx;
    double e;
    char title[192], dur[32], chip[96];
    if (!G.on) return;
    e = ease_out((now - G.t0) / 0.22);
    w = (int)(24 + (bw - 24) * e);
    h = (int)(5 + (bh - 5) * e);
    x = (bw - w) / 2;
    y = 1 + (bh - h) / 2;
    i = snprintf(title, sizeof title, "GPU \xc2\xb7 %s", G.name);
    if (G.cores) i += snprintf(title + i, sizeof title - (size_t)i, " \xc2\xb7 %d-core %s", G.cores, G.family);
    if (G.driver[0]) snprintf(title + i, sizeof title - (size_t)i, " \xc2\xb7 driver %s", G.driver);
    draw_box(y, x, w, h, P_ACCENT, title);
    if (e < 1 || h < 10 || w < 50) return;
    /* chips on the top border, right to left */
    fmt_dur(G.t_last - G.t_reset, dur, sizeof dur);
    snprintf(chip, sizeof chip, " session %s \xc2\xb7 %.2gs ", dur, U.interval);
    chx = x + w - 3 - str_width(chip);
    scr_put(y, chx, str_width(chip), ST(P_SOFT, P_BOX, 0), chip);
    if (U.paused) { chx -= 9; scr_put(y, chx, 8, ST(16, P_WARN, A_BOLD), " PAUSED "); }
    if (G.was_thermal) { chx -= 10; scr_put(y, chx, 9, ST(16, P_HOT, A_BOLD), " THERMAL "); }
    if (G.was_limit) { chx -= 10; scr_put(y, chx, 9, ST(16, P_WARN, A_BOLD), " LIMITED "); }
    G.tab_y = y + 1;
    {
        int tx = x + 2;
        for (i = 0; i < GT_N; i++) {
            char t[48];
            snprintf(t, sizeof t, " %s ", GT_LABEL[i]);
            G.tab_x[i] = tx;
            tx += scr_put(y + 1, tx, x + w - 2 - tx, i == G.tab ? ST(P_BRIGHT, P_ACC_BG, A_BOLD) : ST(P_DIM, P_SEL, 0), t) + 1;
        }
        G.tab_x[GT_N] = tx;
    }
    cx = x + 2;
    cy = y + 3;
    cw = w - 4;
    ch = h - 5;
    G.view_h = ch;
    if (!G.primed || !G.samples) {
        scr_printf(cy + 1, cx + 1, cw - 2, ST(P_ACCENT, P_BOX, A_BOLD), "%s Collecting the first GPU sample\xe2\x80\xa6",
                   SPIN[(int)(now * 12) % 10]);
    } else if (G.tab == GT_PROC) {
        gpu_tab_procs(cx, cy, cw, ch);
    } else {
        GCtx c;
        if (G.scroll > G.total - ch) G.scroll = G.total - ch;
        if (G.scroll < 0) G.scroll = 0;
        c.x = cx;
        c.y = cy;
        c.w = cw - 1;
        c.h = ch;
        c.row = 0;
        c.scroll = G.scroll;
        if (G.tab == GT_OVER) gpu_tab_overview(&c);
        else if (G.tab == GT_CLOCK) gpu_tab_clock(&c);
        else if (G.tab == GT_MEM) gpu_tab_mem(&c);
        else gpu_tab_events(&c);
        G.total = c.row;
        if (G.total > ch) {
            int th = ch * ch / G.total, ty;
            if (th < 1) th = 1;
            ty = (int)((double)G.scroll / (double)(G.total - ch) * (ch - th) + 0.5);
            for (i = 0; i < ch; i++)
                set_cell(cy + i, cx + cw - 1, ST(i >= ty && i < ty + th ? 75 : 237, P_BOX, 0),
                         i >= ty && i < ty + th ? "\xe2\x94\x83" : "\xe2\x94\x82", 3, 1);
        }
    }
    scr_put(y + h - 2, x + 2, w - 4, ST(P_FAINT, P_BOX, 0),
            "\xe2\x86\x90\xe2\x86\x92 Tab tabs \xc2\xb7 \xe2\x86\x91\xe2\x86\x93 scroll/select \xc2\xb7 Enter pin focus \xc2\xb7 s sort ("
            "Processes) \xc2\xb7 r reset stats \xc2\xb7 k signal \xc2\xb7 , . interval \xc2\xb7 z pause \xc2\xb7 Esc/g close");
    if (G.tab == GT_PROC && x + w - 4 - 30 > x + 2) {
        char t[48];
        snprintf(t, sizeof t, "sort: %s", GS_LABEL[G.sort]);
        scr_rput(y + h - 1, x + w - 3 - str_width(t) - 2, str_width(t) + 2, ST(P_SOFT, P_BOX, 0), t);
    }
}

static void gpu_open(Proc *p)
{
    int i;
    gpu_activate();
    G.on = 1;
    G.t0 = now_s();
    G.scroll = 0;
    for (i = 0; p && i < G.ncl; i++) {
        GClient *c = &G.cl[i];
        if (c->alive && c->pid == p->pid && c->start_s == p->start_s && c->start_us == p->start_us) {
            G.focus = c->pid;
            G.focus_s = c->start_s;
            G.focus_us = c->start_us;
        }
    }
}

static void gpu_close(void) { G.on = 0; }

/* Pins the focus on the selected process; again on it returns to automatic. */
static void gpu_pin(void)
{
    GClient *c;
    if (G.sel < 0 || G.sel >= G.nord) return;
    c = &G.cl[G.ord[G.sel]];
    if (G.focus == c->pid && G.focus_s == c->start_s && G.focus_us == c->start_us) {
        G.focus = -1;
        gpu_event(0, "Focus back to the busiest process");
    } else {
        G.focus = c->pid;
        G.focus_s = c->start_s;
        G.focus_us = c->start_us;
    }
}

static Proc *gpu_sel_proc(void)
{
    const GClient *c;
    Proc *p;
    if (G.sel < 0 || G.sel >= G.nord) return NULL;
    c = &G.cl[G.ord[G.sel]];
    p = c->alive && c->pid < PID_CAP ? bypid[c->pid] : NULL;
    return p && p->start_s == c->start_s && p->start_us == c->start_us ? p : NULL;
}

static void toggle_pause(void);
static void step_interval(int k);
static void kill_menu(Proc *single);
static void relaunch_sudo(void);

static int gpu_key(int k)
{
    int page = G.view_h > 2 ? G.view_h - 2 : 1, proc = G.tab == GT_PROC, i;
    switch (k) {
    case K_ESC: case 'q': case 'g': gpu_close(); break;
    case 3: case K_F(10): return 0;
    case K_F(1): case '?': case 'h': U.help = 1; break;
    case K_LEFT: case K_BTAB: G.tab = (G.tab + GT_N - 1) % GT_N; G.scroll = 0; break;
    case K_RIGHT: case '\t': G.tab = (G.tab + 1) % GT_N; G.scroll = 0; break;
    case K_UP: if (proc) G.sel--; else G.scroll--; break;
    case K_DOWN: if (proc) G.sel++; else G.scroll++; break;
    case K_PGUP: if (proc) G.sel -= G.list_h > 1 ? G.list_h - 1 : 1; else G.scroll -= page; break;
    case K_PGDN: case ' ': if (proc) G.sel += G.list_h > 1 ? G.list_h - 1 : 1; else G.scroll += page; break;
    case K_HOME: G.sel = 0; G.scroll = 0; break;
    case K_END: G.sel = G.nord - 1; G.scroll = 1 << 30; break;
    case '\r': case '\n': case 'f': if (proc) gpu_pin(); else G.tab = GT_PROC; break;
    case 's':
        G.sort = (G.sort + 1) % GS_N;
        gpu_order();
        break;
    case 'r': gpu_reset(); break;
    case 'k': case K_F(9): case K_DEL: if (gpu_sel_proc()) kill_menu(gpu_sel_proc()); break;
    case 'z': toggle_pause(); break;
    case ',': case '.': step_interval(k); break;
    case 'S': relaunch_sudo(); break;
    case K_MOUSE:
        if (MS.b == 64 || MS.b == 65) {
            int d = MS.b == 64 ? -3 : 3;
            if (proc) G.sel += d;
            else G.scroll += d;
        } else if (MS.press && MS.b == 0 && MS.y == G.tab_y) {
            for (i = 0; i < GT_N; i++)
                if (MS.x >= G.tab_x[i] && MS.x < G.tab_x[i + 1]) { G.tab = i; G.scroll = 0; }
        } else if (MS.press && MS.b == 0 && MS.y == SC.h - 1) {
            for (i = 0; i < NFOOT; i++)
                if (MS.x >= g_foot_x[i] && MS.x < g_foot_x[i + 1]) return gpu_key(FOOT[i].key);
        } else if (MS.press && proc && MS.y >= G.list_y && MS.y < G.list_y + G.list_h) {
            int idx = G.top + MS.y - G.list_y;
            double t = now_s();
            if (idx >= G.nord) break;
            G.sel = idx;
            if (MS.b == 2) { if (gpu_sel_proc()) kill_menu(gpu_sel_proc()); break; }
            if (MS.b == 0 && idx == G.click_row && t - G.click_t < 0.4) { gpu_pin(); G.click_t = 0; break; }
            G.click_row = idx;
            G.click_t = t;
        }
        break;
    }
    if (G.sel >= G.nord) G.sel = G.nord - 1;
    if (G.sel < 0) G.sel = 0;
    if (G.scroll < 0) G.scroll = 0;
    if (proc && G.nord) G.sel_pid = G.cl[G.ord[G.sel]].pid;
    return 1;
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
        now - U.pulse_t < 0.8 || (IN.on && now - IN.t0 < 0.3) || (IN.on && IN.cs_state == 1) || U.input_mode ||
        (G.on && (now - G.t0 < 0.3 || !G.samples)))
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
        "g                  GPU view: utilization, clocks, power, temperature, memory, bandwidth,",
        "                   limits and per-process GPU use with session stats (r resets them)",
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
    case 'g':
        inspect_close();
        gpu_open(p);
        break;
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

static void toggle_pause(void)
{
    U.paused = !U.paused;
    if (!U.paused) U.next_sample = now_s();
    set_msg(0, U.paused ? "Updates paused (z to resume)" : "Updates resumed");
}

static void step_interval(int k)
{
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
    if (G.on) return gpu_key(k);
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
    case 'z': toggle_pause(); break;
    case ',': case '.': step_interval(k); break;
    case 'g': gpu_open(p); break;
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
/* --gpu: GPU telemetry on stdout                                      */
/* ------------------------------------------------------------------ */

static void jstr(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void jnum(FILE *f, double v, int ok)
{
    if (!ok || !isfinite(v)) fputs("null", f);
    else if (v == floor(v) && fabs(v) < 1e15) fprintf(f, "%.0f", v);
    else fprintf(f, "%.3f", v);
}

static void jkey(FILE *f, int *first, const char *k)
{
    fprintf(f, "%s\"%s\":", *first ? "" : ",", k);
    *first = 0;
}

static void jkv(FILE *f, int *first, const char *k, double v, int ok)
{
    jkey(f, first, k);
    jnum(f, v, ok);
}

static void jstates(FILE *f, const GStates *st, int skip_idle)
{
    int i, first = 1;
    double act = 0, cact = 0;
    for (i = 0; i < st->n; i++)
        if (!skip_idle || strcmp(st->name[i], "IDLE_OFF")) { act += st->res[i]; cact += st->cum[i]; }
    fputc('{', f);
    for (i = 0; i < st->n; i++) {
        if ((skip_idle && !strcmp(st->name[i], "IDLE_OFF")) || st->cum[i] <= 0) continue;
        fprintf(f, "%s", first ? "" : ",");
        first = 0;
        jstr(f, st->name[i]);
        fputs(":{\"pct\":", f);
        jnum(f, act > 0 ? st->res[i] / act * 100 : 0, 1);
        fputs(",\"session_pct\":", f);
        jnum(f, cact > 0 ? st->cum[i] / cact * 100 : 0, 1);
        fputc('}', f);
    }
    fputc('}', f);
}

static void gpu_json(FILE *f, unsigned long *ev_out)
{
    char ts[40], a[32];
    time_t t = time(NULL);
    struct tm tm;
    uint64_t ml = gpu_mem_limit();
    unsigned long e;
    int i, k, first;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S%z", &tm);
    fprintf(f, "{\"time\":\"%s\",\"unix\":%lld,\"interval_s\":", ts, (long long)t);
    jnum(f, U.interval, 1);
    fputs(",\"session_s\":", f);
    jnum(f, G.t_last - G.t_reset, 1);
    fprintf(f, ",\"samples\":%ld", G.samples);

    fputs(",\"device\":{\"name\":", f);
    jstr(f, G.name);
    fprintf(f, ",\"cores\":%d,\"family\":", G.cores);
    jstr(f, G.family);
    fputs(",\"driver\":", f);
    jstr(f, G.driver);
    fputs(",\"metal_plugin\":", f);
    jstr(f, G.metal);
    fputs(",\"max_clock_mhz\":", f);
    jnum(f, G.fmax, G.nfreq > 0);
    fputc('}', f);

    fputs(",\"utilization\":{", f);
    first = 1;
    jkv(f, &first, "device_pct", S.gpu_util * 100, S.has_gpu);
    jkv(f, &first, "renderer_pct", S.gpu_rend, S.gpu_rend >= 0);
    jkv(f, &first, "tiler_pct", S.gpu_tiler, S.gpu_tiler >= 0);
    jkv(f, &first, "active_residency_pct", G.active_res, G.has_freq);
    fputc('}', f);

    fputs(",\"clock\":{", f);
    first = 1;
    jkv(f, &first, "mhz", G.mhz, G.has_freq && G.nfreq);
    jkv(f, &first, "max_mhz", G.fmax, G.nfreq > 0);
    jkey(f, &first, "pstates");
    fputc('[', f);
    for (i = 0, k = 0; i < G.ps.n; i++) {
        int p = gst_pnum(G.ps.name[i]), known = p > 0 && p <= G.nfreq;
        if (p > 0 && !known && G.ps.cum[i] <= 0) continue; /* states this GPU never uses */
        fprintf(f, "%s{\"state\":", k++ ? "," : "");
        jstr(f, G.ps.name[i]);
        fputs(",\"mhz\":", f);
        jnum(f, known ? G.freq[p - 1] : 0, known);
        fputs(",\"pct\":", f);
        jnum(f, G.ps.tot > 0 ? G.ps.res[i] / G.ps.tot * 100 : 0, G.ps.tot > 0);
        fputs(",\"session_pct\":", f);
        jnum(f, G.ps.cumtot > 0 ? G.ps.cum[i] / G.ps.cumtot * 100 : 0, G.ps.cumtot > 0);
        fputc('}', f);
    }
    fputs("]}", f);

    fputs(",\"power\":{", f);
    first = 1;
    jkv(f, &first, "gpu_w", G.gpu_w, G.has_gpu_w);
    jkv(f, &first, "dram_w", G.dram_w, G.has_dram_w);
    jkv(f, &first, "ane_w", G.ane_w, G.has_ane_w);
    jkv(f, &first, "cpu_w", G.cpu_w, G.has_cpu_w);
    jkv(f, &first, "system_w", S.sys_w, S.sys_w >= 0);
    jkv(f, &first, "gpu_energy_j", G.energy_j, G.has_gpu_w);
    jkv(f, &first, "dram_energy_j", G.dram_j, G.has_dram_w);
    jkv(f, &first, "power_target_pct", G.ppm, G.has_ppm);
    fputc('}', f);

    fputs(",\"temperature\":{", f);
    first = 1;
    jkv(f, &first, "avg_c", G.temp, G.has_temp);
    jkv(f, &first, "max_c", G.tmax, G.has_temp);
    jkv(f, &first, "session_max_c", G.tmax_peak, G.has_temp);
    jkv(f, &first, "sensors", G.ntemp, 1);
    fputc('}', f);

    fputs(",\"memory\":{", f);
    first = 1;
    jkv(f, &first, "gpu_in_use_bytes", (double)S.gpu_mem, S.has_gpu);
    jkv(f, &first, "gpu_allocated_bytes", (double)S.gpu_alloc, S.has_gpu);
    jkv(f, &first, "gpu_driver_bytes", (double)S.gpu_drv, S.has_gpu);
    jkv(f, &first, "parameter_buffer_bytes", (double)S.gpu_pb, S.has_gpu);
    jkv(f, &first, "tiled_scene_bytes", (double)S.gpu_tiled, S.has_gpu);
    jkv(f, &first, "split_scenes", (double)S.gpu_split, S.has_gpu);
    jkv(f, &first, "wired_limit_bytes", (double)ml, ml > 0);
    jkv(f, &first, "in_use_pct_of_wired_limit", ml ? (double)S.gpu_mem / (double)ml * 100 : 0, ml > 0);
    jkv(f, &first, "system_total_bytes", (double)S.mem_total, 1);
    jkv(f, &first, "system_used_bytes", (double)S.mem_used, 1);
    jkv(f, &first, "system_wired_bytes", (double)S.mem_wired, 1);
    jkv(f, &first, "system_compressed_bytes", (double)S.mem_comp, 1);
    jkv(f, &first, "swap_used_bytes", (double)S.swap_used, 1);
    jkey(f, &first, "pressure");
    jstr(f, pressure_name(S.pressure));
    fputc('}', f);

    fputs(",\"bandwidth\":{", f);
    first = 1;
    jkv(f, &first, "dram_read_gbs", G.bw_rd, G.has_bw);
    jkv(f, &first, "dram_write_gbs", G.bw_wr, G.has_bw);
    jkv(f, &first, "dram_total_gbs", G.bw_rd + G.bw_wr, G.has_bw);
    jkv(f, &first, "gpu_mem_latency_cycles", G.lat, G.has_lat);
    fputc('}', f);

    fputs(",\"limits\":{", f);
    first = 1;
    jkv(f, &first, "limited_pct", gpu_limit(NULL, 0, 0) * 100, G.pc.n > 0);
    jkv(f, &first, "session_limited_pct", gpu_limit(NULL, 0, 1) * 100, G.pc.n > 0);
    jkv(f, &first, "thermal_limited_pct", gst_out(&G.cltm, "NO_CLTM", 0) * 100, G.cltm.n > 0);
    jkv(f, &first, "session_thermal_limited_pct", gst_out(&G.cltm, "NO_CLTM", 1) * 100, G.cltm.n > 0);
    jkv(f, &first, "pmu_perf_lost_pct", gst_out(&G.pmu, "NONE", 0) * 100, G.pmu.n > 0);
    jkv(f, &first, "power_zone_pct", gst_out(&G.pz, "NO_ZONE", 0) * 100, G.pz.n > 0);
    jkv(f, &first, "throttle_events", (double)G.throttle, G.ior);
    jkv(f, &first, "session_throttle_events", (double)G.throttle_tot, G.ior);
    jkey(f, &first, "controller");
    jstates(f, &G.pc, 1);
    fputc('}', f);

    fputs(",\"health\":{", f);
    first = 1;
    jkv(f, &first, "restarts", (double)G.restarts, G.ior);
    jkv(f, &first, "session_restarts", (double)G.restarts_tot, G.ior);
    jkv(f, &first, "recovery_count", (double)S.gpu_recov, S.has_gpu);
    jkv(f, &first, "session_uv_warnings", (double)G.uvwarn_tot, G.ior);
    fputc('}', f);

    fputs(",\"session\":{", f);
    for (i = 0; i < GM_N; i++) {
        const GMet *m = &G.m[i];
        fprintf(f, "%s\"%s\":", i ? "," : "", GMDEF[i].key);
        if (!m->n) { fputs("null", f); continue; }
        fputs("{\"avg\":", f);
        jnum(f, gm_avg(i), 1);
        fputs(",\"min\":", f);
        jnum(f, m->mn, 1);
        fputs(",\"max\":", f);
        jnum(f, m->mx, 1);
        fputs(",\"p95\":", f);
        jnum(f, m->p95, 1);
        fputc('}', f);
    }
    fputc('}', f);

    fputs(",\"processes\":[", f);
    for (i = 0, k = 0; i < G.nord; i++) {
        const GClient *c = &G.cl[G.ord[i]];
        if (!gcl_match(c, G.query)) continue;
        fprintf(f, "%s{\"pid\":%d,\"name\":", k++ ? "," : "", c->pid);
        jstr(f, c->name);
        fprintf(f, ",\"alive\":%s,\"gpu_pct\":", c->alive ? "true" : "false");
        jnum(f, c->cur, 1);
        fputs(",\"avg_pct\":", f);
        jnum(f, gcl_avg(c), 1);
        fputs(",\"peak_pct\":", f);
        jnum(f, c->peak, 1);
        fputs(",\"gpu_time_s\":", f);
        jnum(f, (double)c->ns / 1e9, 1);
        fputs(",\"session_gpu_time_s\":", f);
        jnum(f, (double)(c->ns - c->ns0) / 1e9, 1);
        fputs(",\"est_power_w\":", f);
        jnum(f, c->est_w, G.has_gpu_w && c->alive);
        fprintf(f, ",\"clients\":%d,\"queues\":%d,\"queues_max\":%d,\"api\":", c->nclients, c->queues, c->qmax);
        api_str(c->api, a, sizeof a);
        jstr(f, a);
        fputs(",\"graphics_footprint_bytes\":", f);
        jnum(f, (double)c->gfx, c->gfx >= 0);
        fputs(",\"graphics_footprint_peak_bytes\":", f);
        jnum(f, (double)c->gfx_peak, c->gfx_peak >= 0);
        fputs(",\"phys_footprint_bytes\":", f);
        jnum(f, (double)c->phys, c->phys > 0);
        fputs(",\"last_submit_ms_ago\":", f);
        {
            uint64_t now = tk2ns(mach_absolute_time());
            jnum(f, now > c->last_sub ? (double)(now - c->last_sub) / 1e6 : 0, c->last_sub > 0 && c->alive);
        }
        fputc('}', f);
    }
    fputs("],\"events\":[", f);
    e = G.nev > GNEV && *ev_out < G.nev - GNEV ? G.nev - GNEV : *ev_out;
    for (k = 0; e < G.nev; e++) {
        const GEvent *ev = &G.ev[e % GNEV];
        localtime_r(&ev->t, &tm);
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S%z", &tm);
        fprintf(f, "%s{\"time\":\"%s\",\"severity\":\"%s\",\"text\":", k++ ? "," : "", ts,
                ev->sev >= 2 ? "fault" : ev->sev ? "warning" : "info");
        jstr(f, ev->s);
        fputc('}', f);
    }
    *ev_out = G.nev;
    fputs("]}\n", f);
}

static void gpu_text(FILE *f, unsigned long *ev_out)
{
    char ts[32], a[32], b[32], c2[32], d[32], st[160];
    time_t t = time(NULL);
    struct tm tm;
    uint64_t ml = gpu_mem_limit();
    unsigned long e;
    int i, n;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "GPU        %s \xc2\xb7 %d cores \xc2\xb7 %s \xc2\xb7 driver %s \xc2\xb7 %s\n", G.name, G.cores,
            G.family[0] ? G.family : "-", G.driver[0] ? G.driver : "-", G.metal[0] ? G.metal : "-");
    fmt_dur(G.t_last - G.t_reset, a, sizeof a);
    fprintf(f, "sample     %s \xc2\xb7 interval %.2gs \xc2\xb7 sample %ld \xc2\xb7 session %s\n", ts, U.interval, G.samples, a);
    fprintf(f, "util       device %.0f%%", S.gpu_util * 100);
    if (S.gpu_rend >= 0) fprintf(f, " \xc2\xb7 renderer %.0f%%", S.gpu_rend);
    if (S.gpu_tiler >= 0) fprintf(f, " \xc2\xb7 tiler %.0f%%", S.gpu_tiler);
    if (G.has_freq) fprintf(f, " \xc2\xb7 active residency %.1f%%", G.active_res);
    fputc('\n', f);
    if (G.has_freq) {
        fprintf(f, "clock      ");
        if (G.nfreq && G.mhz > 0) fprintf(f, "%.0f MHz while active (max %.0f)", G.mhz, G.fmax);
        else fprintf(f, G.nfreq ? "idle (max %.0f MHz)" : "no frequency table", G.fmax);
        for (i = 0; i < G.ps.n; i++) {
            int p = gst_pnum(G.ps.name[i]);
            double pct = G.ps.tot > 0 ? G.ps.res[i] / G.ps.tot * 100 : 0;
            if (p <= 0 || pct < 0.5) continue;
            if (p <= G.nfreq) fprintf(f, " \xc2\xb7 %.0f:%.0f%%", G.freq[p - 1], pct);
            else fprintf(f, " \xc2\xb7 %s:%.0f%%", G.ps.name[i], pct);
        }
        fputc('\n', f);
    }
    fmt_power(G.gpu_w, a, sizeof a);
    fprintf(f, "power      GPU %s", G.has_gpu_w ? a : "n/a");
    if (G.has_dram_w) { fmt_power(G.dram_w, a, sizeof a); fprintf(f, " \xc2\xb7 DRAM %s", a); }
    if (G.has_ane_w) { fmt_power(G.ane_w, a, sizeof a); fprintf(f, " \xc2\xb7 ANE %s", a); }
    if (S.sys_w >= 0) { fmt_power(S.sys_w, a, sizeof a); fprintf(f, " \xc2\xb7 system %s", a); }
    if (G.has_gpu_w) fprintf(f, " \xc2\xb7 GPU energy %.1f J this session", G.energy_j);
    if (G.has_ppm) fprintf(f, " \xc2\xb7 power target %.0f%%", G.ppm);
    fputc('\n', f);
    if (G.has_temp)
        fprintf(f, "temp       avg %.1f\xc2\xb0" "C \xc2\xb7 max %.1f\xc2\xb0" "C \xc2\xb7 session max %.1f\xc2\xb0" "C (%d sensors)\n",
                G.temp, G.tmax, G.tmax_peak, G.ntemp);
    fmt_mem(S.gpu_mem, a, sizeof a);
    fmt_mem(S.gpu_alloc, b, sizeof b);
    fmt_mem(ml, c2, sizeof c2);
    fprintf(f, "memory     GPU in use %s \xc2\xb7 allocated %s \xc2\xb7 wired limit %s", a, b, ml ? c2 : "macOS default");
    if (ml) fprintf(f, " (%.0f%% used)", (double)S.gpu_mem / (double)ml * 100);
    fmt_mem(S.mem_used, a, sizeof a);
    fmt_mem(S.mem_total, b, sizeof b);
    fmt_mem(S.swap_used, c2, sizeof c2);
    fprintf(f, " \xc2\xb7 system %s / %s \xc2\xb7 swap %s \xc2\xb7 pressure %s\n", a, b, c2, pressure_name(S.pressure));
    if (G.has_bw || G.has_lat) {
        fprintf(f, "bandwidth  ");
        if (G.has_bw) fprintf(f, "DRAM read %.1f GB/s \xc2\xb7 write %.1f GB/s \xc2\xb7 total %.1f GB/s", G.bw_rd, G.bw_wr, G.bw_rd + G.bw_wr);
        if (G.has_lat) fprintf(f, "%sGPU memory latency %.0f cycles", G.has_bw ? " \xc2\xb7 " : "", G.lat);
        fputc('\n', f);
    }
    if (G.ior) {
        double lf = gpu_limit(st, sizeof st, 0);
        fprintf(f, "limits     clock limited %.0f%%%s%s%s \xc2\xb7 thermal %.0f%% \xc2\xb7 throttle events %ld \xc2\xb7 restarts %ld (session %ld)",
                lf * 100, st[0] ? " (" : "", st, st[0] ? ")" : "", gst_out(&G.cltm, "NO_CLTM", 0) * 100, G.throttle,
                G.restarts, G.restarts_tot);
        fputs(" \xc2\xb7 controller", f);
        {
            double act = 0;
            for (i = 0; i < G.pc.n; i++)
                if (strcmp(G.pc.name[i], "IDLE_OFF")) act += G.pc.res[i];
            for (i = 0, n = 0; i < G.pc.n; i++) {
                if (!strcmp(G.pc.name[i], "IDLE_OFF") || act <= 0 || G.pc.res[i] / act < 0.005) continue;
                fprintf(f, " %s %.0f%%", G.pc.name[i], G.pc.res[i] / act * 100);
                n++;
            }
            if (!n) fputs(" idle", f);
        }
        fputc('\n', f);
    }
    if (G.samples) {
        fmt_dur(G.t_last - G.t_reset, d, sizeof d);
        fprintf(f, "session    %s, %ld samples", d, G.samples);
        for (i = 0; i < GM_N; i++) {
            if (i == GM_REND || i == GM_TILER || i == GM_TMAX || !G.m[i].n) continue;
            gm_stats(i, st, sizeof st);
            fprintf(f, "\n           %-15s %s", GMDEF[i].name, st);
        }
        if (G.has_gpu_w) fprintf(f, "\n           %-15s %.1f J (%.4f Wh)", "GPU energy", G.energy_j, G.energy_j / 3600);
        fputc('\n', f);
    }
    fprintf(f, "processes  %7s  %-22s %6s %6s %6s %10s %7s %7s %9s %9s %11s\n", "PID", "NAME", "GPU%", "AVG%", "PEAK%",
            "GPU TIME", "EST W", "QUEUES", "GFX MEM", "FOOTPRINT", "LAST SUBMIT");
    for (i = 0, n = 0; i < G.nord; i++) {
        const GClient *c = &G.cl[G.ord[i]];
        char q[16];
        if (!gcl_match(c, G.query) || (!G.query && (n >= 15 || !c->alive))) continue;
        fmt_cputime(c->ns - c->ns0, a, sizeof a);
        fmt_power(c->est_w, b, sizeof b);
        snprintf(q, sizeof q, "%d/%d", c->queues, c->qmax);
        fmt_mem(c->gfx > 0 ? (uint64_t)c->gfx : 0, c2, sizeof c2);
        fmt_mem(c->phys, d, sizeof d);
        fmt_ago(c->alive ? c->last_sub : 0, st, sizeof st);
        fprintf(f, "           %7d  %-22.22s %6.1f %6.1f %6.1f %10s %7s %7s %9s %9s %11s%s\n", c->pid, c->name, c->cur, gcl_avg(c),
                c->peak, a, G.has_gpu_w && c->alive ? b : "-", q, c->gfx >= 0 ? c2 : "-", c->phys ? d : "-", st,
                c->alive ? "" : "  (gone)");
        n++;
    }
    if (!n) fprintf(f, "           (no GPU clients%s)\n", G.query ? " match -p" : "");
    e = G.nev > GNEV && *ev_out < G.nev - GNEV ? G.nev - GNEV : *ev_out;
    for (; e < G.nev; e++) {
        const GEvent *ev = &G.ev[e % GNEV];
        localtime_r(&ev->t, &tm);
        strftime(ts, sizeof ts, "%H:%M:%S", &tm);
        fprintf(f, "event      %s %s%s\n", ts, ev->sev >= 2 ? "FAULT " : ev->sev ? "WARN " : "", ev->s);
    }
    *ev_out = G.nev;
}

/* mhtop --gpu: samples every `interval` seconds and prints `count` reports
 * (0 = until interrupted). Needs no terminal. */
static int gpu_cli(int json, long count, double interval, const char *query)
{
    unsigned long ev_out = 0;
    long i;
    U.interval = interval;
    G.query = query;
    sys_init();
    sample_all(now_s(), NULL);
    gpu_activate();
    for (i = 0; count <= 0 || i < count; i++) {
        double until = now_s() + interval, left;
        while ((left = until - now_s()) > 0) {
            struct timespec ts = { (time_t)left, (long)((left - floor(left)) * 1e9) };
            nanosleep(&ts, NULL);
        }
        sample_all(now_s(), NULL);
        procs_expire(now_s());
        if (json) gpu_json(stdout, &ev_out);
        else {
            if (i) fputc('\n', stdout);
            gpu_text(stdout, &ev_out);
        }
        if (fflush(stdout) != 0 || ferror(stdout)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(FILE *f)
{
    fprintf(f,
            "usage: " APP " [-d secs] [-s key] [-t] [-u user] [-f filter] [-c] [-M] [-g] [-p pid|name]\n"
            "       " APP " --gpu [--json] [-d secs] [-n count] [-p pid|name]\n"
            "  Interactive process manager for macOS (htop / Activity Monitor style).\n"
            "  -d secs   refresh interval (default 1.5; 1 with --gpu)\n"
            "  -s key    sort: cpu gpu mem power disk time threads pri nice pid user name\n"
            "  -t        start in tree view (parents with their children)\n"
            "  -u user   show only this user's processes (toggle with u)\n"
            "  -f text   initial filter\n"
            "  -c        compact header\n"
            "  -M        disable mouse support (keeps terminal text selection)\n"
            "  -g        start in the GPU view (g toggles it)\n"
            "  -p proc   GPU focus: a pid or part of a process name\n"
            "  --gpu     print GPU telemetry instead of the UI (no terminal needed): utilization,\n"
            "            clocks, power, temperature, memory, bandwidth, limits, per-process GPU use\n"
            "            and session stats (avg/min/max/p95); -p keeps only matching processes\n"
            "  --json    with --gpu: one JSON object per sample (JSON Lines)\n"
            "  -n count  with --gpu: number of samples (default 1, 0 = until interrupted)\n"
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
    double t1;
    int i, k, gpu_cli_mode = 0, json = 0, start_gpu = 0, have_d = 0;
    long count = 1;
    const char *query = NULL;

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
        else if (!strcmp(a, "-g")) start_gpu = 1;
        else if (!strcmp(a, "--gpu")) gpu_cli_mode = 1;
        else if (!strcmp(a, "--json")) json = 1;
        else if (!strcmp(a, "-p") && i + 1 < argc) query = argv[++i];
        else if (!strcmp(a, "-n") && i + 1 < argc) {
            char *end;
            count = strtol(argv[++i], &end, 10);
            if (*end || count < 0) { fprintf(stderr, APP ": bad count\n"); return 2; }
        } else if (!strcmp(a, "-d") && i + 1 < argc) {
            U.interval = atof(argv[++i]);
            have_d = 1;
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
    if (json && !gpu_cli_mode) { fprintf(stderr, APP ": --json needs --gpu\n"); return 2; }
    if (gpu_cli_mode) return gpu_cli(json, count, have_d ? U.interval : 1.0, query);
    G.query = query;
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
    if (start_gpu || query) gpu_open(NULL);
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
