/*
 * macdirstat - WinDirStat-style disk usage explorer for the macOS terminal.
 *
 * ISO/ANSI C99 + POSIX only (libc + pthreads). No curses: raw termios and
 * ANSI/VT escape sequences. Build: make   Usage: ./macdirstat [-a] [path]
 */
#define _DARWIN_C_SOURCE 1

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define APP "macdirstat"
#define VERSION "1.0"

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

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void fmt_size(int64_t b, char *o, size_t sz)
{
    static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
    double v = (double)b;
    int i = 0;
    if (b < 1024) {
        snprintf(o, sz, "%lld B", (long long)b);
        return;
    }
    while (v >= 1024.0 && i < 6) { v /= 1024.0; i++; }
    snprintf(o, sz, v < 10 ? "%.2f %s" : v < 100 ? "%.1f %s" : "%.0f %s", v, u[i]);
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
/* output buffer                                                       */
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

static void bmove(Buf *b, int y, int x) { bprintf(b, "\x1b[%d;%dH", y, x); }

static void bspaces(Buf *b, int n)
{
    static const char sp[] = "                                                                ";
    while (n > 0) {
        int k = n > 64 ? 64 : n;
        bput(b, sp, (size_t)k);
        n -= k;
    }
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
/* UTF-8 aware text measurement / truncation                           */
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

static void emit_glyph(Buf *b, const char *s, Glyph g)
{
    if (g.bad) bput(b, "?", 1);
    else bput(b, s, (size_t)g.n);
}

static int str_width(const char *s)
{
    size_t len = strlen(s), i = 0;
    int w = 0;
    while (i < len) {
        Glyph g = glyph(s + i, len - i);
        w += g.w;
        i += (size_t)g.n;
    }
    return w;
}

/* Print at most maxw columns, cutting the end with an ellipsis. */
static int put_text(Buf *b, const char *s, int maxw, int pad)
{
    size_t len = strlen(s), i = 0;
    int total, limit, used = 0;
    if (maxw <= 0) return 0;
    total = str_width(s);
    limit = total <= maxw ? maxw : maxw - 1;
    while (i < len) {
        Glyph g = glyph(s + i, len - i);
        if (used + g.w > limit) break;
        emit_glyph(b, s + i, g);
        used += g.w;
        i += (size_t)g.n;
    }
    if (total > maxw) { bstr(b, "\xe2\x80\xa6"); used++; }
    if (pad && used < maxw) { bspaces(b, maxw - used); used = maxw; }
    return used;
}

/* Print a path, cutting from the left; bytes before `split` use c1,
 * the rest c2 (e.g. dim directory part, bright basename). */
static int put_path(Buf *b, const char *s, size_t split, int maxw,
                    const char *c1, const char *c2, int pad)
{
    size_t len = strlen(s), i = 0;
    int total, used = 0, in2;
    if (maxw <= 0) return 0;
    total = str_width(s);
    if (total > maxw) {
        int rem = total;
        while (i < len && rem > maxw - 1) {
            Glyph g = glyph(s + i, len - i);
            rem -= g.w;
            i += (size_t)g.n;
        }
        bstr(b, c1);
        bstr(b, "\xe2\x80\xa6");
        used = 1;
    }
    in2 = i >= split;
    bstr(b, in2 ? c2 : c1);
    while (i < len) {
        Glyph g = glyph(s + i, len - i);
        if (!in2 && i >= split) { bstr(b, c2); in2 = 1; }
        if (used + g.w > maxw) break;
        emit_glyph(b, s + i, g);
        used += g.w;
        i += (size_t)g.n;
    }
    if (pad && used < maxw) { bspaces(b, maxw - used); used = maxw; }
    return used;
}

/* ------------------------------------------------------------------ */
/* tree                                                                */
/* ------------------------------------------------------------------ */

enum { F_DIR = 1, F_ERR = 2, F_LINK = 4, F_DUP = 8, F_MOUNT = 16 };

typedef struct Node {
    char *name;            /* root: absolute path; others: entry name */
    struct Node *parent;
    struct Node **kids;
    uint32_t nkids, cap;
    unsigned flags;
    int64_t size;          /* allocated bytes in subtree (st_blocks * 512) */
    int64_t files;         /* non-directory entries in subtree */
} Node;

static Node *node_new(const char *name, Node *parent)
{
    Node *n = calloc(1, sizeof *n);
    if (!n) die("out of memory");
    n->name = xstrdup(name);
    n->parent = parent;
    return n;
}

static void node_add(Node *p, Node *c)
{
    if (p->nkids == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 4;
        p->kids = xrealloc(p->kids, p->cap * sizeof *p->kids);
    }
    p->kids[p->nkids++] = c;
}

static void node_free(Node *n)
{
    uint32_t i;
    if (!n) return;
    for (i = 0; i < n->nkids; i++) node_free(n->kids[i]);
    free(n->kids);
    free(n->name);
    free(n);
}

static void path_rec(Buf *b, const Node *n, const Node *stop)
{
    if (n->parent && n->parent != stop) {
        path_rec(b, n->parent, stop);
        if (b->n == 0 || b->p[b->n - 1] != '/') bput(b, "/", 1);
    }
    bstr(b, n->name);
}

/* Full path of n (malloc'd). */
static char *node_path(const Node *n)
{
    Buf b = { 0 };
    path_rec(&b, n, NULL);
    return b.p;
}

/* Path of n relative to ancestor `base` (malloc'd). */
static char *rel_path(const Node *base, const Node *n)
{
    Buf b = { 0 };
    path_rec(&b, n, base);
    return b.p;
}

/* ------------------------------------------------------------------ */
/* (dev, inode) set: count hard links / firmlinked dirs only once      */
/* ------------------------------------------------------------------ */

typedef struct { uint64_t dev, ino; } Id;
typedef struct { Id *t; size_t cap, n; } IdSet;

static size_t id_hash(uint64_t dev, uint64_t ino)
{
    uint64_t h = ino * 0x9E3779B97F4A7C15ull ^ (dev + 0x632BE59BD9B4E019ull);
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return (size_t)h;
}

/* 1 if newly inserted, 0 if already present. Inode 0 marks empty slots. */
static int idset_add(IdSet *s, uint64_t dev, uint64_t ino)
{
    size_t m, i;
    if (ino == 0) return 1;
    if ((s->n + 1) * 2 > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 4096, k;
        Id *nt = calloc(nc, sizeof *nt);
        if (!nt) die("out of memory");
        for (k = 0; k < s->cap; k++) {
            if (!s->t[k].ino) continue;
            i = id_hash(s->t[k].dev, s->t[k].ino) & (nc - 1);
            while (nt[i].ino) i = (i + 1) & (nc - 1);
            nt[i] = s->t[k];
        }
        free(s->t);
        s->t = nt;
        s->cap = nc;
    }
    m = s->cap - 1;
    i = id_hash(dev, ino) & m;
    while (s->t[i].ino) {
        if (s->t[i].ino == ino && s->t[i].dev == dev) return 0;
        i = (i + 1) & m;
    }
    s->t[i].dev = dev;
    s->t[i].ino = ino;
    s->n++;
    return 1;
}

/* ------------------------------------------------------------------ */
/* background scanner                                                  */
/* ------------------------------------------------------------------ */

typedef struct { Node *n; int depth; } Deferred;
typedef struct { dev_t dev; int ok; } DevOk;

typedef struct {
    const char *path;      /* absolute directory to scan */
    const char *name;      /* name for the resulting root node */
    int allvol;            /* descend into every mounted volume */
    dev_t dev;             /* device of the scan root */
    char base[32];         /* "diskN" of the scan root's APFS container */
    int cancel, done;      /* accessed with __atomic builtins */
    int64_t nfiles, ndirs, bytes, nerr;
    Node *cur;             /* directory being read (for the progress UI) */
    Node *result;
    IdSet seen;
    Deferred *def;         /* mount points / firmlinks, scanned shallowest first */
    size_t ndef, capdef;
    DevOk devok[64];
    int ndevok;
} ScanJob;

#define ATOM_ADD(p, v) __atomic_fetch_add((p), (v), __ATOMIC_RELAXED)

static int is_dots(const char *s)
{
    return s[0] == '.' && (s[1] == 0 || (s[1] == '.' && s[2] == 0));
}

/* "/dev/disk3s5" -> "disk3" (the APFS container / physical disk). */
static void disk_base(const char *from, char *out, size_t n)
{
    size_t i = 4;
    out[0] = 0;
    if (strncmp(from, "/dev/", 5) == 0) from += 5;
    if (strncmp(from, "disk", 4) != 0) return;
    while (from[i] >= '0' && from[i] <= '9') i++;
    if (i == 4 || i >= n) return;
    memcpy(out, from, i);
    out[i] = 0;
}

/* May we descend into the filesystem of fd? Same device, same container,
 * or -a. Network shares, devfs, autofs, other disks and images are skipped. */
static int dev_allowed(ScanJob *j, int fd, dev_t dev)
{
    struct statfs s;
    char b[32];
    int i, ok;
    if (dev == j->dev || j->allvol) return 1;
    for (i = 0; i < j->ndevok; i++)
        if (j->devok[i].dev == dev) return j->devok[i].ok;
    ok = 0;
    if (j->base[0] && fstatfs(fd, &s) == 0) {
        disk_base(s.f_mntfromname, b, sizeof b);
        ok = strcmp(b, j->base) == 0;
    }
    if (j->ndevok < (int)(sizeof j->devok / sizeof j->devok[0])) {
        j->devok[j->ndevok].dev = dev;
        j->devok[j->ndevok].ok = ok;
        j->ndevok++;
    }
    return ok;
}

static void defer_push(ScanJob *j, Node *n, int depth)
{
    if (j->ndef == j->capdef) {
        j->capdef = j->capdef ? j->capdef * 2 : 32;
        j->def = xrealloc(j->def, j->capdef * sizeof *j->def);
    }
    j->def[j->ndef].n = n;
    j->def[j->ndef].depth = depth;
    j->ndef++;
}

/* Reads directory fd (takes ownership) of device `dev` into node n. */
static void scan_into(ScanJob *j, Node *n, int fd, dev_t dev, int depth)
{
    DIR *d = fdopendir(fd);
    struct dirent *e;
    if (!d) {
        close(fd);
        n->flags |= F_ERR;
        ATOM_ADD(&j->nerr, 1);
        return;
    }
    __atomic_store_n(&j->cur, n, __ATOMIC_RELEASE);
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        Node *c;
        if (__atomic_load_n(&j->cancel, __ATOMIC_RELAXED)) break;
        if (is_dots(e->d_name)) continue;
        c = node_new(e->d_name, n);
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            c->flags |= F_ERR;
            ATOM_ADD(&j->nerr, 1);
        } else if (S_ISDIR(st.st_mode)) {
            c->flags |= F_DIR;
            ATOM_ADD(&j->ndirs, 1);
            if (st.st_dev != dev) {
                /* Mount point or macOS firmlink: the same directory can be
                 * reachable twice (/Users and /System/Volumes/Data/Users).
                 * Scan later, shallowest path first, so it is counted at
                 * the shortest path and marked duplicate elsewhere. */
                defer_push(j, c, depth + 1);
            } else if (!idset_add(&j->seen, (uint64_t)st.st_dev, (uint64_t)st.st_ino)) {
                c->flags |= F_DUP;
            } else {
                int cfd = openat(dirfd(d), e->d_name,
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (cfd < 0) {
                    c->flags |= F_ERR;
                    ATOM_ADD(&j->nerr, 1);
                } else {
                    scan_into(j, c, cfd, dev, depth + 1);
                    __atomic_store_n(&j->cur, n, __ATOMIC_RELEASE);
                }
            }
        } else {
            if (S_ISLNK(st.st_mode)) c->flags |= F_LINK;
            c->files = 1;
            if (st.st_nlink > 1 &&
                !idset_add(&j->seen, (uint64_t)st.st_dev, (uint64_t)st.st_ino))
                c->flags |= F_DUP; /* another hard link already counted */
            else
                c->size = (int64_t)st.st_blocks * 512; /* allocated, not apparent */
            ATOM_ADD(&j->nfiles, 1);
            ATOM_ADD(&j->bytes, c->size);
        }
        node_add(n, c);
        n->size += c->size;
        n->files += c->files;
    }
    closedir(d);
}

static void scan_deferred(ScanJob *j, Node *root, Node *n, int depth)
{
    char *rel = rel_path(root, n);
    size_t pl = strlen(j->path);
    Buf p = { 0 };
    struct stat st;
    Node *a;
    int fd;
    bstr(&p, j->path);
    if (pl == 0 || j->path[pl - 1] != '/') bput(&p, "/", 1);
    bstr(&p, rel);
    free(rel);
    fd = open(p.p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    free(p.p);
    if (fd < 0 || fstat(fd, &st) != 0) {
        if (fd >= 0) close(fd);
        n->flags |= F_ERR;
        ATOM_ADD(&j->nerr, 1);
        return;
    }
    if (!dev_allowed(j, fd, st.st_dev)) {
        close(fd);
        n->flags |= F_MOUNT;
        return;
    }
    if (!idset_add(&j->seen, (uint64_t)st.st_dev, (uint64_t)st.st_ino)) {
        close(fd);
        n->flags |= F_DUP;
        return;
    }
    scan_into(j, n, fd, st.st_dev, depth);
    for (a = n->parent; a; a = a->parent) {
        a->size += n->size;
        a->files += n->files;
    }
}

static void *scan_thread(void *arg)
{
    ScanJob *j = arg;
    Node *r = node_new(j->name, NULL);
    struct stat st;
    struct statfs sf;
    int fd = open(j->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    r->flags |= F_DIR;
    if (fd < 0) {
        r->flags |= F_ERR;
    } else if (fstat(fd, &st) != 0) {
        close(fd);
        r->flags |= F_ERR;
    } else {
        j->dev = st.st_dev;
        if (fstatfs(fd, &sf) == 0) disk_base(sf.f_mntfromname, j->base, sizeof j->base);
        idset_add(&j->seen, (uint64_t)st.st_dev, (uint64_t)st.st_ino);
        scan_into(j, r, fd, st.st_dev, 0);
        while (j->ndef && !__atomic_load_n(&j->cancel, __ATOMIC_RELAXED)) {
            size_t i, best = 0;
            Deferred d;
            for (i = 1; i < j->ndef; i++)
                if (j->def[i].depth < j->def[best].depth) best = i;
            d = j->def[best];
            memmove(j->def + best, j->def + best + 1, (j->ndef - best - 1) * sizeof *j->def);
            j->ndef--;
            scan_deferred(j, r, d.n, d.depth);
        }
    }
    free(j->def);
    j->result = r;
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* terminal                                                            */
/* ------------------------------------------------------------------ */

static struct termios g_orig;
static volatile sig_atomic_t g_raw, g_winch;

#define RESTORE_SEQ "\x1b[?2026l\x1b[0m\x1b[?25h\x1b[?1049l"

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
    static const char enter[] = "\x1b[?1049h\x1b[?25l\x1b[2J";
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
    atexit(term_restore);
    write_all(enter, sizeof enter - 1);
}

enum {
    K_NONE = 0, K_ESC = 27, K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT,
    K_PGUP, K_PGDN, K_HOME, K_END, K_DEL
};

static int read_key(int timeout_ms)
{
    struct pollfd p;
    unsigned char c, s[8];
    int n = 0;
    p.fd = STDIN_FILENO;
    p.events = POLLIN;
    if (poll(&p, 1, timeout_ms) <= 0) return K_NONE;
    if (read(STDIN_FILENO, &c, 1) != 1) return K_NONE;
    if (c != 27) return c;
    while (n < 7 && poll(&p, 1, 30) > 0 && read(STDIN_FILENO, &s[n], 1) == 1) {
        n++;
        if (s[0] != '[' && s[0] != 'O') break;
        if (n >= 2 && (s[0] == 'O' || (s[n - 1] >= 0x40 && s[n - 1] <= 0x7e))) break;
    }
    if (n == 0) return K_ESC;
    if (n < 2 || (s[0] != '[' && s[0] != 'O')) return K_NONE;
    switch (s[n - 1]) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case '~':
        switch (s[1]) {
        case '1': case '7': return K_HOME;
        case '4': case '8': return K_END;
        case '3': return K_DEL;
        case '5': return K_PGUP;
        case '6': return K_PGDN;
        }
    }
    return K_NONE;
}

/* ------------------------------------------------------------------ */
/* UI state                                                            */
/* ------------------------------------------------------------------ */

enum { SORT_SIZE, SORT_NAME, SORT_FILES, SORT_N };
static const char *SORT_LABEL[SORT_N] = { "size \xe2\x86\x93", "name \xe2\x86\x91", "files \xe2\x86\x93" };
static const size_t TOPN[] = { 0, 10, 25, 50 };
#define NTOPN (sizeof TOPN / sizeof TOPN[0])
#define FILES_VIEW_MAX 1000

static struct {
    Node *root, *cur;
    Node **vis;
    size_t nvis, viscap, sel, top;
    int sort, topi, files_mode, help;
    int rows, cols, allvol;
    double anim_t0;
    int64_t disk_free, disk_total, nerr;
    char msg[512];
    double msg_t;
    Buf fb;
} U;

static void set_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(U.msg, sizeof U.msg, fmt, ap);
    va_end(ap);
    U.msg_t = now_s();
}

static void update_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        U.cols = ws.ws_col;
        U.rows = ws.ws_row;
    } else {
        U.cols = 80;
        U.rows = 24;
    }
}

static void check_winch(void)
{
    if (g_winch) {
        g_winch = 0;
        update_size();
        bstr(&U.fb, "\x1b[2J");
    }
}

static void update_free(void)
{
    struct statfs s;
    if (statfs(U.root->name, &s) == 0) {
        U.disk_free = (int64_t)s.f_bavail * (int64_t)s.f_bsize;
        U.disk_total = (int64_t)s.f_blocks * (int64_t)s.f_bsize;
    }
}

static void anim_restart(void) { U.anim_t0 = now_s(); }

static int list_h(void) { return U.rows - 5 > 1 ? U.rows - 5 : 1; }

#define ANIM_DUR 0.42
#define ANIM_STAGGER 0.018

static double anim_ease(double now, int row)
{
    double t = (now - U.anim_t0 - row * ANIM_STAGGER) / ANIM_DUR;
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    t = 1 - t;
    return 1 - t * t * t;
}

static int animating(double now)
{
    return now - U.anim_t0 < ANIM_DUR + ANIM_STAGGER * list_h() + 0.05;
}

/* ------------------------------------------------------------------ */
/* visible list                                                        */
/* ------------------------------------------------------------------ */

static int cmp_name_raw(const Node *a, const Node *b)
{
    int r = strcasecmp(a->name, b->name);
    return r ? r : strcmp(a->name, b->name);
}

static int cmp_size(const void *x, const void *y)
{
    const Node *a = *(Node * const *)x, *b = *(Node * const *)y;
    if (a->size != b->size) return a->size < b->size ? 1 : -1;
    return cmp_name_raw(a, b);
}

static int cmp_name(const void *x, const void *y)
{
    return cmp_name_raw(*(Node * const *)x, *(Node * const *)y);
}

static int cmp_files(const void *x, const void *y)
{
    const Node *a = *(Node * const *)x, *b = *(Node * const *)y;
    if (a->files != b->files) return a->files < b->files ? 1 : -1;
    return cmp_size(x, y);
}

static void vis_push(Node *n)
{
    if (U.nvis == U.viscap) {
        U.viscap = U.viscap ? U.viscap * 2 : 256;
        U.vis = xrealloc(U.vis, U.viscap * sizeof *U.vis);
    }
    U.vis[U.nvis++] = n;
}

/* Bounded min-heap on size: keeps the k largest files. */
static void heap_offer(Node *n, size_t k)
{
    size_t i;
    if (U.nvis < k) {
        vis_push(n);
        i = U.nvis - 1;
        while (i > 0 && U.vis[(i - 1) / 2]->size > U.vis[i]->size) {
            Node *t = U.vis[i];
            U.vis[i] = U.vis[(i - 1) / 2];
            U.vis[(i - 1) / 2] = t;
            i = (i - 1) / 2;
        }
        return;
    }
    if (n->size <= U.vis[0]->size) return;
    U.vis[0] = n;
    i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        Node *t;
        if (l < U.nvis && U.vis[l]->size < U.vis[m]->size) m = l;
        if (r < U.nvis && U.vis[r]->size < U.vis[m]->size) m = r;
        if (m == i) break;
        t = U.vis[i];
        U.vis[i] = U.vis[m];
        U.vis[m] = t;
        i = m;
    }
}

static void collect_files(Node *n, size_t k)
{
    uint32_t i;
    for (i = 0; i < n->nkids; i++) {
        Node *c = n->kids[i];
        if (c->flags & F_DIR) collect_files(c, k);
        else heap_offer(c, k);
    }
}

static void rebuild(const Node *keep)
{
    size_t topn = TOPN[U.topi], i;
    int (*cmp)(const void *, const void *) =
        U.sort == SORT_NAME ? cmp_name : U.sort == SORT_FILES ? cmp_files : cmp_size;
    U.nvis = 0;
    if (U.files_mode) {
        collect_files(U.cur, topn ? topn : FILES_VIEW_MAX);
    } else {
        for (i = 0; i < U.cur->nkids; i++) vis_push(U.cur->kids[i]);
        if (topn && U.nvis > topn) {
            qsort(U.vis, U.nvis, sizeof *U.vis, cmp_size);
            U.nvis = topn;
        }
    }
    qsort(U.vis, U.nvis, sizeof *U.vis, cmp);
    if (keep) {
        for (i = 0; i < U.nvis; i++)
            if (U.vis[i] == keep) { U.sel = i; break; }
        if (i == U.nvis) U.sel = 0;
    }
    if (U.sel >= U.nvis) U.sel = U.nvis ? U.nvis - 1 : 0;
}

static void clamp_scroll(void)
{
    size_t h = (size_t)list_h();
    if (U.nvis == 0) { U.sel = U.top = 0; return; }
    if (U.sel >= U.nvis) U.sel = U.nvis - 1;
    if (U.sel < U.top) U.top = U.sel;
    if (U.sel >= U.top + h) U.top = U.sel - h + 1;
    if (U.top + h > U.nvis) U.top = U.nvis > h ? U.nvis - h : 0;
}

static Node *sel_node(void) { return U.nvis ? U.vis[U.sel] : NULL; }

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static const char *PART[8] = {
    " ", "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d",
    "\xe2\x96\x8c", "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89"
};
#define FULLBLOCK "\xe2\x96\x88"
static const int GRAD[] = { 35, 41, 77, 113, 149, 185, 221, 215, 209, 203, 197 };
#define NGRAD ((int)(sizeof GRAD / sizeof GRAD[0]))

/* Eighth-block bar with a heat gradient; `gleam` >= 0 adds a moving shine. */
static void put_bar(Buf *b, double frac, int w, double gleam, const char *bg)
{
    long e8, cells;
    int i, last = -1, gpos = -100;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    e8 = (long)(frac * w * 8 + 0.5);
    cells = (e8 + 7) / 8;
    if (gleam >= 0 && cells > 0)
        gpos = (int)fmod(gleam * 30.0, (double)(cells + 14)) - 4;
    bstr(b, "\x1b[48;5;236m");
    for (i = 0; i < w; i++) {
        long c = e8 - (long)i * 8;
        int col, d;
        if (c <= 0) { bput(b, " ", 1); continue; }
        col = GRAD[i * NGRAD / w];
        d = abs(i - gpos);
        if (d == 0) col = 231;
        else if (d == 1) col = 255;
        else if (d == 2) col = 252;
        if (col != last) { bprintf(b, "\x1b[38;5;%dm", col); last = col; }
        bstr(b, c >= 8 ? FULLBLOCK : PART[c]);
    }
    bstr(b, "\x1b[0m");
    bstr(b, bg);
}

typedef struct { int bw, filescol, namew; } Layout;

static Layout layout(void)
{
    Layout L;
    int used;
    L.bw = U.cols / 4;
    if (L.bw > 40) L.bw = 40;
    if (L.bw < 10) L.bw = U.cols < 60 ? 6 : 10;
    L.filescol = !U.files_mode && U.cols >= 72;
    used = 2 + 10 + 7 + L.bw + 1 + (L.filescol ? 11 : 0);
    L.namew = U.cols - used;
    return L;
}

static void draw_row(Buf *b, int y, Node *n, int selected, int row, double now, const Layout *L)
{
    const char *bg = selected ? "\x1b[48;5;237m" : "";
    double share = U.cur->size > 0 ? (double)n->size / (double)U.cur->size : 0;
    char sz[32], fc[32] = "", c1[48], c2[48];
    int szcol;
    if (share > 1) share = 1;
    fmt_size(n->size, sz, sizeof sz);
    szcol = share >= 0.5 ? 203 : share >= 0.1 ? 215 : 252;

    bmove(b, y, 1);
    bstr(b, "\x1b[0m");
    bstr(b, bg);
    bstr(b, selected ? "\x1b[1;38;5;214m\xe2\x96\xb6 " : "  ");
    bprintf(b, "\x1b[0m%s%s\x1b[38;5;%dm%9s ", bg, selected ? "\x1b[1m" : "", szcol, sz);
    bprintf(b, "\x1b[0m%s\x1b[38;5;245m%5.1f%% ", bg, share * 100.0);
    put_bar(b, share * anim_ease(now, row), L->bw, selected ? now : -1, bg);
    bput(b, " ", 1);
    if (L->filescol) {
        if (n->flags & F_DIR) fmt_count(n->files, fc, sizeof fc);
        bprintf(b, "\x1b[0m%s\x1b[38;5;244m%10s ", bg, fc);
    }

    if (U.files_mode) {
        char *rel = rel_path(U.cur, n);
        const char *slash = strrchr(rel, '/');
        snprintf(c1, sizeof c1, "\x1b[0m%s\x1b[38;5;242m", bg);
        snprintf(c2, sizeof c2, "\x1b[0m%s%s\x1b[38;5;%dm", bg, selected ? "\x1b[1m" : "",
                 (n->flags & F_LINK) ? 81 : 255);
        put_path(b, rel, slash ? (size_t)(slash - rel) + 1 : 0, L->namew, c1, c2, 1);
        free(rel);
    } else {
        const char *tag = "";
        int tw, used, col;
        Buf nm = { 0 };
        if (n->flags & F_ERR) tag = (n->flags & F_DIR) ? " [no access]" : " [error]";
        else if (n->flags & F_MOUNT) tag = " [other volume]";
        else if (n->flags & F_DUP) tag = (n->flags & F_DIR) ? " [duplicate]" : " [hard link]";
        else if (n->flags & F_LINK) tag = " [symlink]";
        tw = (int)strlen(tag);
        if (tw > L->namew / 2) { tag = ""; tw = 0; }
        col = (n->flags & F_ERR) ? 203 : (n->flags & F_DIR) ? 75 : (n->flags & F_LINK) ? 81 : 252;
        bprintf(b, "\x1b[0m%s%s\x1b[38;5;%dm", bg,
                (selected || (n->flags & F_DIR)) ? "\x1b[1m" : "", col);
        bstr(&nm, n->name);
        if (n->flags & F_DIR) bput(&nm, "/", 1);
        used = put_text(b, nm.p, L->namew - tw, 0);
        free(nm.p);
        if (tw) {
            bprintf(b, "\x1b[0m%s\x1b[38;5;240m%s", bg, tag);
            used += tw;
        }
        bstr(b, "\x1b[0m");
        bstr(b, bg);
        if (used < L->namew) bspaces(b, L->namew - used);
    }
    bstr(b, "\x1b[0m");
}

static void draw_footer(Buf *b)
{
    static const char *keys[][2] = {
        { "\xe2\x86\x91\xe2\x86\x93", "move" }, { "\xe2\x8f\x8e/\xe2\x86\x92", "open" },
        { "\xe2\x86\x90", "up" }, { "f", "big files" }, { "s", "sort" }, { "t", "top" },
        { "d", "delete" }, { "r", "rescan" }, { "?", "help" }, { "q", "quit" }
    };
    int used = 0, i;
    bmove(b, U.rows, 1);
    bstr(b, "\x1b[0;48;5;235m");
    for (i = 0; i < (int)(sizeof keys / sizeof keys[0]); i++) {
        int w = 1 + str_width(keys[i][0]) + 1 + str_width(keys[i][1]) + 1;
        if (used + w > U.cols) break;
        bprintf(b, " \x1b[1;38;5;214m%s\x1b[0;48;5;235;38;5;250m %s ", keys[i][0], keys[i][1]);
        used += w;
    }
    bspaces(b, U.cols - used);
    bstr(b, "\x1b[0m");
}

static void draw_main(Buf *b, double now)
{
    Layout L = layout();
    char tot[32], fcount[32], fr[32], right[160], line[256], show[32];
    char *path;
    int rw, pw, i, h = list_h();
    Node *sn;

    if (U.cols < 40 || U.rows < 8) {
        bstr(b, "\x1b[H\x1b[0m\x1b[2JTerminal too small");
        return;
    }
    clamp_scroll();

    /* header */
    fmt_size(U.cur->size, tot, sizeof tot);
    fmt_count(U.cur->files, fcount, sizeof fcount);
    fmt_size(U.disk_free, fr, sizeof fr);
    snprintf(right, sizeof right, " %s \xc2\xb7 %s files \xc2\xb7 %s free ", tot, fcount, fr);
    rw = str_width(right);
    pw = U.cols - 12 - 1 - rw;
    if (pw < 12) { right[0] = 0; rw = 0; pw = U.cols - 13; }
    bmove(b, 1, 1);
    bstr(b, "\x1b[0;1;38;5;16;48;5;214m " APP " \x1b[0;48;5;235m ");
    path = node_path(U.cur);
    put_path(b, path, 0, pw, "\x1b[0;48;5;235;1;38;5;255m", "\x1b[0;48;5;235;1;38;5;255m", 1);
    free(path);
    bprintf(b, "\x1b[0;48;5;235;38;5;250m%s\x1b[0m", right);

    /* mode line */
    bmove(b, 2, 1);
    if (U.topi) snprintf(show, sizeof show, "top %zu biggest", TOPN[U.topi]);
    else snprintf(show, sizeof show, "all");
    if (U.files_mode)
        snprintf(line, sizeof line, " Largest files in this tree \xc2\xb7 %zu shown \xc2\xb7 sort: %s \xc2\xb7 show: %s",
                 U.nvis, SORT_LABEL[U.sort], show);
    else
        snprintf(line, sizeof line, " %zu of %u entries \xc2\xb7 sort: %s \xc2\xb7 show: %s",
                 U.nvis, U.cur->nkids, SORT_LABEL[U.sort], show);
    bstr(b, "\x1b[0;38;5;111m");
    i = put_text(b, line, U.cols, 0);
    if (U.nerr > 0 && i + 40 < U.cols) {
        char e[96];
        snprintf(e, sizeof e, "  \xe2\x9a\xa0 %lld unreadable (grant Full Disk Access)", (long long)U.nerr);
        bstr(b, "\x1b[38;5;203m");
        put_text(b, e, U.cols - i, 0);
    }
    bstr(b, "\x1b[0m\x1b[K");

    /* column header */
    bmove(b, 3, 1);
    bprintf(b, "\x1b[0;4;38;5;244m  %9s %6s %-*s ", "Size", "%", L.bw, "Share");
    if (L.filescol) bprintf(b, "%10s ", "Files");
    bstr(b, U.files_mode ? "Path" : "Name");
    bspaces(b, L.namew - 4);
    bstr(b, "\x1b[0m");

    /* list */
    for (i = 0; i < h; i++) {
        size_t idx = U.top + (size_t)i;
        if (idx < U.nvis) {
            draw_row(b, 4 + i, U.vis[idx], idx == U.sel, i, now, &L);
        } else {
            bmove(b, 4 + i, 1);
            bstr(b, "\x1b[0m\x1b[K");
            if (i == 0 && U.nvis == 0)
                bstr(b, U.files_mode ? "\x1b[38;5;242m  (no files)\x1b[0m"
                                     : "\x1b[38;5;242m  (empty directory)\x1b[0m");
        }
    }

    /* status line: message or selected path */
    bmove(b, U.rows - 1, 1);
    bstr(b, "\x1b[0m");
    sn = sel_node();
    if (U.msg[0] && now - U.msg_t < 6.0) {
        bstr(b, "\x1b[38;5;221m ");
        put_text(b, U.msg, U.cols - 1, 0);
    } else if (sn) {
        path = node_path(sn);
        bput(b, " ", 1);
        put_path(b, path, 0, U.cols - 1, "\x1b[38;5;242m", "\x1b[38;5;242m", 0);
        free(path);
    }
    bstr(b, "\x1b[0m\x1b[K");
    draw_footer(b);
}

/* Centered modal box. Line index `pathline` is left-truncated. */
static void draw_box(Buf *b, const char *title, const char **lines, int n,
                     int pathline, int color)
{
    int bw = str_width(title) + 6, i, x, y;
    for (i = 0; i < n; i++) {
        int w = str_width(lines[i]) + 4;
        if (w > bw) bw = w;
    }
    if (bw > U.cols - 2) bw = U.cols - 2;
    if (n > U.rows - 2) n = U.rows - 2;
    x = (U.cols - bw) / 2 + 1;
    y = (U.rows - (n + 2)) / 2 + 1;
    bmove(b, y, x);
    bprintf(b, "\x1b[0;48;5;234;38;5;%dm\xe2\x95\xad\xe2\x94\x80 \x1b[1m", color);
    i = put_text(b, title, bw - 6, 0);
    bprintf(b, "\x1b[22m ");
    for (i += 4; i < bw - 1; i++) bstr(b, "\xe2\x94\x80");
    bstr(b, "\xe2\x95\xae");
    for (i = 0; i < n; i++) {
        bmove(b, y + 1 + i, x);
        bprintf(b, "\x1b[38;5;%dm\xe2\x94\x82 \x1b[38;5;252m", color);
        if (i == pathline)
            put_path(b, lines[i], 0, bw - 4, "\x1b[1;38;5;255m", "\x1b[1;38;5;255m", 1);
        else
            put_text(b, lines[i], bw - 4, 1);
        bprintf(b, "\x1b[22;38;5;%dm \xe2\x94\x82", color);
    }
    bmove(b, y + n + 1, x);
    bprintf(b, "\x1b[38;5;%dm\xe2\x95\xb0", color);
    for (i = 1; i < bw - 1; i++) bstr(b, "\xe2\x94\x80");
    bstr(b, "\xe2\x95\xaf\x1b[0m");
}

static void draw_help(Buf *b)
{
    static const char *lines[] = {
        "\xe2\x86\x91 k / \xe2\x86\x93 j      move selection",
        "PgUp / PgDn      page up / down",
        "Home g / End G   first / last entry",
        "\xe2\x86\x92 l / Enter      open directory (file list: jump to its folder)",
        "\xe2\x86\x90 h / Backspace  parent directory (file list: back)",
        "f                largest files anywhere below this directory",
        "s                sort: size / name / file count",
        "t                show: all / top 10 / top 25 / top 50 biggest",
        "d / Delete       delete selected item (asks for confirmation)",
        "r                rescan current directory",
        "q                quit",
        "",
        "Sizes are allocated disk space; hard links and firmlinks count once.",
        "Press any key to close."
    };
    draw_box(b, "Help", lines, (int)(sizeof lines / sizeof lines[0]), -1, 111);
}

static void frame_begin(Buf *b)
{
    check_winch();
    bstr(b, "\x1b[?2026h\x1b[0m");
}

static void frame_end(Buf *b)
{
    bstr(b, "\x1b[?2026l");
    write_all(b->p, b->n);
    b->n = 0;
}

/* ------------------------------------------------------------------ */
/* scanning with progress screen                                       */
/* ------------------------------------------------------------------ */

static void draw_scan(Buf *b, ScanJob *j, double now, double t0)
{
    static const char *spin[] = {
        "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8", "\xe2\xa0\xbc",
        "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87", "\xe2\xa0\x8f"
    };
    static const char *lv[] = {
        " ", "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"
    };
    char f[32], d[32], by[32], line[256];
    int y = U.rows / 2 - 3, w = U.cols - 8, i, last = -1;
    Node *cur = __atomic_load_n(&j->cur, __ATOMIC_ACQUIRE);
    if (w > 72) w = 72;
    if (w < 10) w = 10;
    if (y < 1) y = 1;

    bstr(b, "\x1b[H\x1b[0m\x1b[J");
    bmove(b, y, 3);
    bprintf(b, "\x1b[1;38;5;214m%s \x1b[38;5;255mScanning \x1b[0m", spin[(int)(now * 12) % 10]);
    put_path(b, j->path, 0, U.cols - 16, "\x1b[38;5;111m", "\x1b[38;5;111m", 0);

    /* animated wave of block glyphs */
    bmove(b, y + 2, 3);
    for (i = 0; i < w; i++) {
        double v = sin(i * 0.32 - now * 7.0) * 0.5 + 0.5;
        double v2 = sin(i * 0.11 + now * 2.3) * 0.5 + 0.5;
        int l = (int)(v * v2 * 8.0 + 0.5), col = GRAD[i * NGRAD / w];
        if (col != last) { bprintf(b, "\x1b[38;5;%dm", col); last = col; }
        bstr(b, lv[l]);
    }
    bstr(b, "\x1b[0m");

    fmt_count(__atomic_load_n(&j->nfiles, __ATOMIC_RELAXED), f, sizeof f);
    fmt_count(__atomic_load_n(&j->ndirs, __ATOMIC_RELAXED), d, sizeof d);
    fmt_size(__atomic_load_n(&j->bytes, __ATOMIC_RELAXED), by, sizeof by);
    snprintf(line, sizeof line, "%s files \xc2\xb7 %s folders \xc2\xb7 %s \xc2\xb7 %.1fs",
             f, d, by, now - t0);
    bmove(b, y + 4, 3);
    bstr(b, "\x1b[38;5;252m");
    put_text(b, line, U.cols - 4, 0);
    if (cur) {
        Buf pb = { 0 };
        path_rec(&pb, cur, NULL);
        bmove(b, y + 5, 3);
        put_path(b, pb.p, 0, U.cols - 4, "\x1b[38;5;242m", "\x1b[38;5;242m", 0);
        free(pb.p);
    }
    bmove(b, y + 7, 3);
    bstr(b, "\x1b[38;5;240mq / Esc to cancel\x1b[0m");
}

/* Scans `path` in a thread while animating; NULL if the user cancelled. */
static Node *run_scan(const char *path, const char *name, int64_t *nerr)
{
    ScanJob j;
    pthread_t th;
    double t0 = now_s();
    memset(&j, 0, sizeof j);
    j.path = path;
    j.name = name;
    j.allvol = U.allvol;
    if (pthread_create(&th, NULL, scan_thread, &j) != 0) die("cannot start scanner thread");
    while (!__atomic_load_n(&j.done, __ATOMIC_ACQUIRE)) {
        int k;
        frame_begin(&U.fb);
        draw_scan(&U.fb, &j, now_s(), t0);
        frame_end(&U.fb);
        k = read_key(40);
        if (k == 'q' || k == K_ESC || k == 3)
            __atomic_store_n(&j.cancel, 1, __ATOMIC_RELAXED);
    }
    pthread_join(th, NULL);
    free(j.seen.t);
    bstr(&U.fb, "\x1b[2J");
    if (j.cancel) {
        node_free(j.result);
        return NULL;
    }
    if (nerr) *nerr = j.nerr;
    return j.result;
}

/* ------------------------------------------------------------------ */
/* tree edits                                                          */
/* ------------------------------------------------------------------ */

static void add_up(Node *from, int64_t dsize, int64_t dfiles)
{
    for (; from; from = from->parent) {
        from->size += dsize;
        from->files += dfiles;
    }
}

static void replace_node(Node *old, Node *nw)
{
    Node *p = old->parent;
    uint32_t i;
    nw->parent = p;
    if (p) {
        for (i = 0; i < p->nkids; i++)
            if (p->kids[i] == old) p->kids[i] = nw;
        add_up(p, nw->size - old->size, nw->files - old->files);
    } else {
        U.root = nw;
    }
    for (i = 0; i < nw->nkids; i++) nw->kids[i]->parent = nw;
    if (U.cur == old) U.cur = nw;
    node_free(old);
}

static void remove_node(Node *n)
{
    Node *p = n->parent;
    uint32_t i;
    add_up(p, -n->size, -n->files);
    for (i = 0; i < p->nkids; i++) {
        if (p->kids[i] == n) {
            memmove(p->kids + i, p->kids + i + 1, (p->nkids - i - 1) * sizeof *p->kids);
            p->nkids--;
            break;
        }
    }
    node_free(n);
}

static void rescan_node(Node *n)
{
    char *path = node_path(n);
    int64_t nerr = 0;
    Node *r = run_scan(path, n->parent ? n->name : path, &nerr);
    if (r) {
        if (!n->parent) U.nerr = nerr;
        replace_node(n, r);
        set_msg("Rescanned %s", path);
    } else {
        set_msg("Rescan cancelled");
    }
    free(path);
}

/* Recursive delete relative to dfd; never crosses into another device. */
static int rm_tree_at(int dfd, const char *name, dev_t dev, int *err)
{
    struct stat st;
    if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (!*err) *err = errno;
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        char **names = NULL;
        size_t n = 0, cap = 0, i;
        struct dirent *e;
        DIR *d;
        int fd;
        if (st.st_dev != dev) {
            if (!*err) *err = EXDEV;
            return -1;
        }
        fd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            if (!*err) *err = errno;
            return -1;
        }
        d = fdopendir(fd);
        if (!d) {
            if (!*err) *err = errno;
            close(fd);
            return -1;
        }
        while ((e = readdir(d)) != NULL) {
            if (is_dots(e->d_name)) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 32;
                names = xrealloc(names, cap * sizeof *names);
            }
            names[n++] = xstrdup(e->d_name);
        }
        for (i = 0; i < n; i++) {
            rm_tree_at(dirfd(d), names[i], dev, err);
            free(names[i]);
        }
        free(names);
        closedir(d);
        if (unlinkat(dfd, name, AT_REMOVEDIR) != 0) {
            if (!*err) *err = errno;
            return -1;
        }
        return 0;
    }
    if (unlinkat(dfd, name, 0) != 0) {
        if (!*err) *err = errno;
        return -1;
    }
    return 0;
}

static void do_delete(Node *n)
{
    char *ppath = node_path(n->parent), sz[32], *name = xstrdup(n->name);
    int err = 0, gone = 0, pfd;
    struct stat st;

    set_msg("Deleting %s ...", name);
    frame_begin(&U.fb);
    draw_main(&U.fb, now_s());
    frame_end(&U.fb);

    fmt_size(n->size, sz, sizeof sz);
    pfd = open(ppath, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (pfd < 0) {
        err = errno;
    } else {
        if (fstat(pfd, &st) == 0) rm_tree_at(pfd, n->name, st.st_dev, &err);
        else err = errno;
        gone = fstatat(pfd, n->name, &st, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
        close(pfd);
    }
    if (gone) {
        remove_node(n);
        set_msg("Deleted %s (%s freed)", name, sz);
    } else {
        int isdir = (n->flags & F_DIR) != 0;
        if (isdir) rescan_node(n);
        set_msg("Could not delete %s: %s", name, strerror(err ? err : EPERM));
    }
    free(name);
    free(ppath);
    update_free();
    rebuild(NULL);
    anim_restart();
}

static void confirm_delete(void)
{
    Node *n = sel_node();
    char *path, l2[160], l1[64];
    const char *lines[6];
    int k;
    if (!n) return;
    if (n->flags & (F_MOUNT | F_DUP)) {
        set_msg("Refusing to delete %s: it is %s", n->name,
                (n->flags & F_MOUNT) ? "on another volume" : "a duplicate of a counted entry");
        return;
    }
    path = node_path(n);
    fmt_size(n->size, l1, sizeof l1);
    if (n->flags & F_DIR) {
        char c[32];
        fmt_count(n->files, c, sizeof c);
        snprintf(l2, sizeof l2, "%s in %s files", l1, c);
    } else {
        snprintf(l2, sizeof l2, "%s", l1);
    }
    lines[0] = (n->flags & F_DIR) ? "Permanently delete this directory and everything in it?"
                                  : "Permanently delete this file?";
    lines[1] = path;
    lines[2] = l2;
    lines[3] = "";
    lines[4] = "This bypasses the Trash and cannot be undone.";
    lines[5] = "[y] delete      [any other key] cancel";
    for (;;) {
        frame_begin(&U.fb);
        draw_main(&U.fb, now_s());
        draw_box(&U.fb, "Confirm delete", lines, 6, 1, 203);
        frame_end(&U.fb);
        k = read_key(50);
        if (k != K_NONE) break;
    }
    if (k == 'y' || k == 'Y') do_delete(n);
    else set_msg("Delete cancelled");
    free(path);
}

/* ------------------------------------------------------------------ */
/* main loop                                                           */
/* ------------------------------------------------------------------ */

static void enter_dir(Node *d)
{
    U.cur = d;
    U.sel = U.top = 0;
    rebuild(NULL);
    anim_restart();
}

static int handle_key(int k)
{
    Node *n = sel_node();
    size_t page = (size_t)list_h();
    switch (k) {
    case 'q': case 'Q': case 3:
        return 0;
    case K_UP: case 'k':
        if (U.sel > 0) U.sel--;
        break;
    case K_DOWN: case 'j':
        if (U.sel + 1 < U.nvis) U.sel++;
        break;
    case K_PGUP:
        U.sel = U.sel > page ? U.sel - page : 0;
        break;
    case K_PGDN:
        U.sel = U.sel + page < U.nvis ? U.sel + page : (U.nvis ? U.nvis - 1 : 0);
        break;
    case K_HOME: case 'g':
        U.sel = 0;
        break;
    case K_END: case 'G':
        U.sel = U.nvis ? U.nvis - 1 : 0;
        break;
    case K_RIGHT: case 'l': case '\r': case '\n':
        if (!n) break;
        if (U.files_mode) {
            U.files_mode = 0;
            U.cur = n->parent;
            rebuild(n);
            U.top = 0;
            anim_restart();
        } else if ((n->flags & F_DIR) && n->nkids > 0) {
            enter_dir(n);
        }
        break;
    case K_LEFT: case 'h': case 127: case 8:
        if (U.files_mode) {
            U.files_mode = 0;
            rebuild(NULL);
            anim_restart();
        } else if (U.cur->parent) {
            Node *prev = U.cur;
            U.cur = U.cur->parent;
            U.top = 0;
            rebuild(prev);
            anim_restart();
        }
        break;
    case 's': case 'S':
        U.sort = (U.sort + 1) % SORT_N;
        rebuild(n);
        anim_restart();
        break;
    case 't': case 'T':
        U.topi = (U.topi + 1) % (int)NTOPN;
        rebuild(n);
        anim_restart();
        break;
    case 'f': case 'F':
        U.files_mode = !U.files_mode;
        U.sel = U.top = 0;
        rebuild(NULL);
        anim_restart();
        break;
    case 'd': case 'D': case K_DEL:
        confirm_delete();
        break;
    case 'r': case 'R':
        rescan_node(U.cur);
        update_free();
        rebuild(NULL);
        anim_restart();
        break;
    case '?':
        U.help = 1;
        break;
    }
    return 1;
}

static void usage(FILE *f)
{
    fprintf(f,
            "usage: " APP " [-a] [directory]\n"
            "  WinDirStat-style disk usage explorer for the terminal.\n"
            "  -a    also scan other volumes (external disks, images, network shares);\n"
            "        default: only the APFS container/disk holding the directory\n"
            "  -h    show this help\n"
            "  -V    print version\n");
}

static void setup_locale(void)
{
    setlocale(LC_CTYPE, "");
    if (MB_CUR_MAX == 1 && !setlocale(LC_CTYPE, "en_US.UTF-8"))
        setlocale(LC_CTYPE, "UTF-8");
}

static void raise_nofile(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rlim_t want = OPEN_MAX;
        if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < want) want = rl.rlim_max;
        if (rl.rlim_cur < want) {
            rl.rlim_cur = want;
            setrlimit(RLIMIT_NOFILE, &rl);
        }
    }
}

int main(int argc, char **argv)
{
    const char *arg = ".";
    char real[PATH_MAX];
    struct stat st;
    struct sigaction sa;
    Node *r;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) U.allvol = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(stdout); return 0; }
        else if (!strcmp(argv[i], "-V")) { puts(APP " " VERSION); return 0; }
        else if (argv[i][0] == '-' && argv[i][1]) { usage(stderr); return 2; }
        else arg = argv[i];
    }
    if (!realpath(arg, real)) {
        fprintf(stderr, APP ": %s: %s\n", arg, strerror(errno));
        return 1;
    }
    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, APP ": %s: not a directory\n", real);
        return 1;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, APP ": needs an interactive terminal\n");
        return 1;
    }

    setup_locale();
    raise_nofile();
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fatal_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    update_size();
    term_raw();

    r = run_scan(real, real, &U.nerr);
    if (!r) return 0;
    U.root = U.cur = r;
    update_free();
    rebuild(NULL);
    anim_restart();

    for (;;) {
        double now = now_s();
        int k;
        frame_begin(&U.fb);
        draw_main(&U.fb, now);
        if (U.help) draw_help(&U.fb);
        frame_end(&U.fb);
        k = read_key(animating(now) ? 16 : 50);
        if (k == K_NONE) continue;
        if (U.help) { U.help = 0; continue; }
        if (!handle_key(k)) break;
    }
    term_restore();
    return 0;
}
