/* s3e_files.c -- see s3e_files.h.
 *
 * Resolution order mirrors run_boz.py's vfs_resolve():
 *   1. the save sandbox   <root>/save/<name>
 *   2. a loose file       <root>/<name>
 *   3. a loose file       <root>/<basename>
 *   4. the archive index, by full name then by basename
 *   5. any archive at all, for a .dz the game names but we do not have
 *      (this copy of the data is the gles1 build under a variant name)
 *
 * The APK extract-on-demand step from the reference is deliberately dropped:
 * mkfileidx.py already pulled what it holds into the index on the PC.
 */
#include "s3e_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(p) mkdir((p), 0777)
#endif

#define MAX_SLOTS   64
#define MAX_ARCH    8
#define PATH_MAX_   512

typedef struct {
    char     *name;         /* lowered, owned */
    uint32_t  arch, off, size;
} Entry;

typedef struct {
    int      used;
    uint32_t handle;        /* never reused -- see slot_new() */
    FILE    *fh;
    uint32_t base, size, pos;
    /* Where the FILE* itself is, which is not the same thing as pos: pos is the
     * guest's cursor inside its slice of the archive. Tracking it lets read and
     * write skip the fseek when the stream is already in the right place, and
     * that matters more than it looks -- fseek discards the stdio buffer, so
     * seeking before every read turned a million sequential s3eFileReads into a
     * million SD-card round trips. -1 means "unknown, seek before touching". */
    long     fpos;
    int      last_write;    /* direction of the last transfer; see seek_to() */
    int      writable;
    char     name[128];
    struct SaveFile *sf;    /* a save file served from memory; fh is NULL */
} Slot;

/* Every open, write, close and delete on a save file is logged, capped: saves
 * are the one place a wrong byte means lost progress, and without this there
 * was no way to see the order the game touches them in. */
#define SAVE_TRACE_MAX 200
static int     g_save_trace;

static char    g_root[PATH_MAX_];
static char   *g_arch[MAX_ARCH];
static int     g_narch;
static Entry  *g_index;
static int     g_nindex;
static Slot    g_slot[MAX_SLOTS];
static int     g_err = S3E_FILE_ERR_NONE;
/* Handles increment forever and are never recycled, matching the reference
 * harness. Deriving the handle from the slot index instead would hand the same
 * value out after a close, which is functionally fine but makes the return
 * value differ from the reference and breaks the differential. */
static uint32_t g_next_handle = S3E_FILE_HANDLE_BASE;

/* ------------------------------------------------------------- utilities */

static void lower_slashes(char *s) {
    for (; *s; s++) {
        if (*s == '\\')
            *s = '/';
        else if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
    }
}

/* Guest paths arrive with backslashes and "./" prefixes; normalise both. */
static void norm(const char *in, char *out, size_t n) {
    size_t i = 0;
    while (*in == '.' && (in[1] == '/' || in[1] == '\\'))
        in += 2;
    while (*in == '/' || *in == '\\')
        in++;
    for (; in[i] && i + 1 < n; i++)
        out[i] = in[i];
    out[i] = 0;
    lower_slashes(out);
}

static const char *basename_of(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void join(char *out, size_t n, const char *a, const char *b) {
    snprintf(out, n, "%s/%s", a, b);
}

/* stat, not fopen. Opening a file to measure it fails on Horizon whenever any
 * handle already has it open for writing -- which the game's save code does
 * constantly -- so an existing save looked absent: the existence check said
 * no, and the write path picked "w+b" and truncated it. */
static long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !(st.st_mode & S_IFREG))
        return -1;
    return (long)st.st_size;
}

/* mkdir -p over a path whose last component may be a file name. */
static void mkdir_parents(char *path, int include_last) {
    char *p;
    for (p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        MKDIR(path);
        *p = '/';
    }
    if (include_last)
        MKDIR(path);
}

/* ------------------------------------------------------------ save cache
 *
 * The game autosaves every few seconds during play, and each autosave opens,
 * writes and closes about eight small files. On the Switch every one of those
 * opens and closes is an SD-card metadata operation of 1-2 ms, all on the
 * guest thread: measured at 13.7 ms of opens, 10.8 ms of closes and 2-4 ms of
 * existence checks in one frame, every ~5 seconds -- a 50-60 ms hitch that
 * was the dominant stutter in gameplay.
 *
 * So the save sandbox lives in memory. Everything under <root>/save is loaded
 * once at startup (a few KB), opens/reads/writes/existence checks of save
 * files are served from here, and a close or flush hands a snapshot to a
 * writer thread that puts it on the card. A newer snapshot of the same file
 * replaces one still waiting; deletes queue behind writes in order.
 *
 * Handles on one save file share its entry, which is what a real file does:
 * the game's flush-then-reopen pattern sees its own writes.
 *
 * Once the directory has been read, a miss here is authoritative, which also
 * removes the stat() that every ordinary asset open used to pay first. If the
 * scan could not finish (too many files, or no directory walker), misses fall
 * back to the card.
 *
 * On the host there is no writer thread: snapshots are written immediately. */
#define MAX_SAVES      64
#define SAVE_LOAD_MAX  (8u << 20)

typedef struct SaveFile {
    int       used;
    int       present;        /* exists, as far as the game can tell */
    char      n[PATH_MAX_];   /* normalised name under save/ */
    uint8_t  *data;
    uint32_t  size, cap;
} SaveFile;

static SaveFile g_save[MAX_SAVES];
static int      g_save_cached;    /* save/ fully loaded: misses are real */

static SaveFile *save_find(const char *n) {
    int i;
    for (i = 0; i < MAX_SAVES; i++)
        if (g_save[i].used && strcmp(g_save[i].n, n) == 0)
            return &g_save[i];
    return NULL;
}

static SaveFile *save_add(const char *n) {
    int i;
    for (i = 0; i < MAX_SAVES; i++) {
        if (g_save[i].used)
            continue;
        memset(&g_save[i], 0, sizeof g_save[i]);
        g_save[i].used = 1;
        snprintf(g_save[i].n, sizeof g_save[i].n, "%s", n);
        return &g_save[i];
    }
    return NULL;
}

static int save_reserve(SaveFile *e, uint32_t need) {
    uint8_t *p;
    uint32_t cap;
    if (need <= e->cap)
        return 1;
    cap = e->cap ? e->cap : 4096u;
    while (cap < need)
        cap *= 2u;
    p = (uint8_t *)realloc(e->data, cap);
    if (!p)
        return 0;
    e->data = p;
    e->cap = cap;
    return 1;
}

/* Read one file from the card into a new entry. */
static SaveFile *save_load(const char *n, const char *path) {
    FILE *f = fopen(path, "rb");
    SaveFile *e;
    long len;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    e = len >= 0 ? save_add(n) : NULL;
    if (e && save_reserve(e, (uint32_t)len + 1u) &&
        fread(e->data, 1, (size_t)len, f) == (size_t)len) {
        e->size = (uint32_t)len;
        e->present = 1;
    } else if (e) {
        free(e->data);
        e->used = 0;
        e = NULL;
    }
    fclose(f);
    return e;
}

#ifndef _WIN32
#include <dirent.h>

/* Load <root>/save/<rel> recursively. Returns 0 when something did not fit,
 * in which case misses must not be trusted. */
static int save_scan(const char *rel, uint32_t *total) {
    char dir[PATH_MAX_], path[PATH_MAX_], sub[PATH_MAX_];
    DIR *d;
    struct dirent *de;
    int ok = 1;
    snprintf(dir, sizeof dir, "%s/save%s%s", g_root, rel[0] ? "/" : "", rel);
    d = opendir(dir);
    if (!d)
        return 1;                       /* no save directory yet: all misses */
    while ((de = readdir(d)) != NULL) {
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", de->d_name);
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            ok &= save_scan(sub, total);
        } else if (S_ISREG(st.st_mode)) {
            char n[PATH_MAX_];
            norm(sub, n, sizeof n);
            *total += (uint32_t)st.st_size;
            if (*total > SAVE_LOAD_MAX || !save_load(n, path))
                ok = 0;
        }
    }
    closedir(d);
    return ok;
}
#endif

static void save_load_all(void) {
    uint32_t total = 0;
    int i, n = 0;
    for (i = 0; i < MAX_SAVES; i++) {
        free(g_save[i].data);
        memset(&g_save[i], 0, sizeof g_save[i]);
    }
#ifndef _WIN32
    g_save_cached = save_scan("", &total);
#else
    g_save_cached = 0;
#endif
    for (i = 0; i < MAX_SAVES; i++)
        n += g_save[i].used;
    printf("  [save ] %d file(s), %u bytes held in memory%s\n", n,
           (unsigned)total, g_save_cached ? "" : " (partial: misses go to the card)");
}

/* The entry for a save name: cached, or read from the card when the cache is
 * not authoritative, or (create) a new empty one. NULL when none. */
static SaveFile *save_get(const char *n, int create) {
    SaveFile *e = save_find(n);
    if (!e && !g_save_cached) {
        char path[PATH_MAX_];
        snprintf(path, sizeof path, "%s/save/%s", g_root, n);
        if (file_size(path) >= 0)
            e = save_load(n, path);
    }
    if (!e && create)
        e = save_add(n);
    return e;
}

/* ---- writing it back ---- */

typedef struct {
    int       op;             /* SJ_WRITE or SJ_DELETE */
    char      n[PATH_MAX_];
    uint8_t  *data;
    uint32_t  size;
} SaveJob;

enum { SJ_WRITE = 1, SJ_DELETE = 2 };
#define MAX_JOBS 64

static void job_run(SaveJob *j) {
    char path[PATH_MAX_];
    snprintf(path, sizeof path, "%s/save/%s", g_root, j->n);
    if (j->op == SJ_DELETE) {
        remove(path);
        return;
    }
    mkdir_parents(path, 0);
    {
        FILE *f = fopen(path, "wb");
        if (!f || (j->size && fwrite(j->data, 1, j->size, f) != j->size))
            printf("  [save ] WRITE FAILED: %s\n", j->n);
        if (f)
            fclose(f);
    }
}

#ifdef __SWITCH__
#include <pthread.h>
#include <unistd.h>

static SaveJob         g_jobs[MAX_JOBS];
static int             g_njobs, g_job_busy, g_writer_up;
static pthread_mutex_t g_job_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_job_cv = PTHREAD_COND_INITIALIZER;

static void *writer_main(void *arg) {
    (void)arg;
    for (;;) {
        SaveJob j;
        pthread_mutex_lock(&g_job_mu);
        while (!g_njobs)
            pthread_cond_wait(&g_job_cv, &g_job_mu);
        j = g_jobs[0];
        memmove(&g_jobs[0], &g_jobs[1], (size_t)(g_njobs - 1) * sizeof g_jobs[0]);
        g_njobs--;
        g_job_busy = 1;
        pthread_mutex_unlock(&g_job_mu);
        job_run(&j);
        free(j.data);
        pthread_mutex_lock(&g_job_mu);
        g_job_busy = 0;
        pthread_mutex_unlock(&g_job_mu);
    }
    return NULL;
}

static void writer_start(void) {
    pthread_t t;
    if (g_writer_up)
        return;
    g_writer_up = pthread_create(&t, NULL, writer_main, NULL) == 0;
    if (g_writer_up)
        pthread_detach(t);
    else
        printf("  [save ] no writer thread; saves are written inline\n");
}

/* Queue a job, taking ownership of j->data. A write replaces the data of a
 * write to the same file still waiting at the end of the queue for it --
 * only the newest contents matter. A full queue, or no thread, runs inline. */
static void job_post(SaveJob *j) {
    int i;
    if (g_writer_up) {
        pthread_mutex_lock(&g_job_mu);
        for (i = g_njobs - 1; i >= 0; i--) {
            if (strcmp(g_jobs[i].n, j->n) != 0)
                continue;
            if (g_jobs[i].op == SJ_WRITE && j->op == SJ_WRITE) {
                free(g_jobs[i].data);
                g_jobs[i].data = j->data;
                g_jobs[i].size = j->size;
                pthread_mutex_unlock(&g_job_mu);
                return;
            }
            break;
        }
        if (g_njobs < MAX_JOBS) {
            g_jobs[g_njobs++] = *j;
            pthread_cond_signal(&g_job_cv);
            pthread_mutex_unlock(&g_job_mu);
            return;
        }
        pthread_mutex_unlock(&g_job_mu);
    }
    job_run(j);
    free(j->data);
}

void s3e_vfs_sync(void) {
    int left = 1;
    while (g_writer_up && left) {
        pthread_mutex_lock(&g_job_mu);
        left = g_njobs || g_job_busy;
        pthread_mutex_unlock(&g_job_mu);
        if (left)
            usleep(2000);
    }
}
#else
static void writer_start(void) {}
static void job_post(SaveJob *j) {
    job_run(j);
    free(j->data);
}
void s3e_vfs_sync(void) {}
#endif

static void save_commit(const SaveFile *e) {
    SaveJob j;
    memset(&j, 0, sizeof j);
    j.op = SJ_WRITE;
    snprintf(j.n, sizeof j.n, "%s", e->n);
    j.size = e->size;
    j.data = (uint8_t *)malloc(e->size ? e->size : 1u);
    if (!j.data) {
        printf("  [save ] out of memory saving %s\n", e->n);
        return;
    }
    memcpy(j.data, e->data, e->size);
    job_post(&j);
}

/* ------------------------------------------------------------------ init */

static int read_u32(FILE *f, uint32_t *v) {
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4)
        return 0;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
    return 1;
}

static int read_u16(FILE *f, uint32_t *v) {
    unsigned char b[2];
    if (fread(b, 1, 2, f) != 2)
        return 0;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8);
    return 1;
}

int s3e_vfs_init(const char *root) {
    char path[PATH_MAX_];
    char magic[4];
    uint32_t ver, na, ne, i;
    FILE *f;

    snprintf(g_root, sizeof g_root, "%s", root);
    memset(g_slot, 0, sizeof g_slot);
    g_next_handle = S3E_FILE_HANDLE_BASE;
    g_err = S3E_FILE_ERR_NONE;
    save_load_all();
    writer_start();

    join(path, sizeof path, root, "boz_files.idx");
    f = fopen(path, "rb");
    if (!f)
        return -1;

    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "BOZI", 4) != 0 ||
        !read_u32(f, &ver) || ver != 1 ||
        !read_u32(f, &na) || !read_u32(f, &ne) ||
        na > MAX_ARCH) {
        fclose(f);
        return -1;
    }

    for (i = 0; i < na; i++) {
        uint32_t len;
        if (!read_u16(f, &len) || len >= PATH_MAX_)
            break;
        g_arch[i] = (char *)malloc(len + 1);
        if (!g_arch[i] || fread(g_arch[i], 1, len, f) != len)
            break;
        g_arch[i][len] = 0;
    }
    g_narch = (int)i;

    g_index = (Entry *)calloc(ne ? ne : 1, sizeof(Entry));
    if (!g_index) {
        fclose(f);
        return -1;
    }
    for (i = 0; i < ne; i++) {
        uint32_t len;
        Entry *e = &g_index[i];
        if (!read_u32(f, &e->arch) || !read_u32(f, &e->off) ||
            !read_u32(f, &e->size) || !read_u16(f, &len) || len >= 256)
            break;
        e->name = (char *)malloc(len + 1);
        if (!e->name || fread(e->name, 1, len, f) != len)
            break;
        e->name[len] = 0;
    }
    g_nindex = (int)i;
    fclose(f);
    return g_nindex;
}

/* Mount order decides, exactly as it does for file resolution: mkfileidx.py
 * writes the archives in the order it mounted them and the index resolves
 * first-mount-wins, so the first pack named here is the one whose bytes the
 * game will actually get. blackops_loader.dz matches nothing and is skipped. */
const char *s3e_vfs_build_style(void) {
    static const struct {
        const char *needle, *style;
    } packs[] = {
        { "blackops_etc",   "etc"   },
        { "blackops_dxt",   "dxt"   },
        { "blackops_atitc", "atitc" },
        { "blackops_gles1", "gles1" },
    };
    int i, p;

    for (i = 0; i < g_narch; i++) {
        char low[PATH_MAX_];
        snprintf(low, sizeof low, "%s", basename_of(g_arch[i]));
        lower_slashes(low);
        for (p = 0; p < (int)(sizeof packs / sizeof packs[0]); p++)
            if (strstr(low, packs[p].needle))
                return packs[p].style;
    }
    return "gles1";
}

/* ------------------------------------------------------------- resolving */

static const Entry *index_find(const char *lowered) {
    int i;
    for (i = 0; i < g_nindex; i++)
        if (strcmp(g_index[i].name, lowered) == 0)
            return &g_index[i];
    return NULL;
}

/* The index stores archive paths relative to the root (as they sit on the PC).
 * On the SD card the same files are laid out flat, so fall back to the
 * basename. Returns 1 and fills `out` when the archive is readable. */
static int archive_path(uint32_t ai, char *out, size_t n) {
    join(out, n, g_root, g_arch[ai]);
    if (file_size(out) >= 0)
        return 1;
    join(out, n, g_root, basename_of(g_arch[ai]));
    return file_size(out) >= 0;
}

/* Remembered resolve() answers, found or not.
 *
 * Resolving a name that is not a save costs up to two SD stat() calls and an
 * index scan, and the game asks the same questions over and over: each
 * autosave checks four files for existence, 7-8 ms of card access in one
 * frame, and every asset open pays the stats again. Outside the save
 * sandbox -- which the save cache answers first -- nothing the port can see
 * changes while it runs, so the answer is kept. Only used once the save
 * cache is authoritative, because until then resolve() also looks in save/,
 * whose contents do change. */
#define MEMO_BUCKETS 1024u

typedef struct Memo {
    struct Memo *next;
    char        *name;        /* normalised */
    char        *real;        /* NULL = not found */
    uint32_t     off, size;
} Memo;

static Memo *g_memo[MEMO_BUCKETS];

static uint32_t memo_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h & (MEMO_BUCKETS - 1u);
}

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static const Memo *memo_get(const char *n) {
    const Memo *m;
    for (m = g_memo[memo_hash(n)]; m; m = m->next)
        if (strcmp(m->name, n) == 0)
            return m;
    return NULL;
}

static void memo_put(const char *n, const char *real, uint32_t off,
                     uint32_t size) {
    Memo *m = (Memo *)calloc(1, sizeof *m);
    uint32_t h = memo_hash(n);
    if (!m)
        return;
    m->name = dup_str(n);
    m->real = real ? dup_str(real) : NULL;
    if (!m->name || (real && !m->real)) {
        free(m->name);
        free(m->real);
        free(m);
        return;
    }
    m->off = off;
    m->size = size;
    m->next = g_memo[h];
    g_memo[h] = m;
}

static int resolve_uncached(const char *name, char *real, size_t rn,
                            uint32_t *off, uint32_t *size);

/* Fills `real` with a path, and sets *off/*size. Returns 1 on success. */
static int resolve(const char *name, char *real, size_t rn,
                   uint32_t *off, uint32_t *size) {
    char n[PATH_MAX_];
    const Memo *m;
    int ok;
    if (!g_save_cached)
        return resolve_uncached(name, real, rn, off, size);
    norm(name, n, sizeof n);
    m = memo_get(n);
    if (m) {
        if (!m->real)
            return 0;
        snprintf(real, rn, "%s", m->real);
        *off = m->off;
        *size = m->size;
        return 1;
    }
    ok = resolve_uncached(name, real, rn, off, size);
    memo_put(n, ok ? real : NULL, ok ? *off : 0u, ok ? *size : 0u);
    return ok;
}

static int resolve_uncached(const char *name, char *real, size_t rn,
                            uint32_t *off, uint32_t *size) {
    char n[PATH_MAX_], cand[PATH_MAX_];
    const char *base;
    const Entry *e;
    long fs;

    norm(name, n, sizeof n);
    base = basename_of(n);

    /* Save files are answered by the cache before resolve() is reached; only
     * an incomplete cache needs the card asked. */
    snprintf(cand, sizeof cand, "%s/save/%s", g_root, n);
    if (!g_save_cached && (fs = file_size(cand)) >= 0)
        goto loose;
    join(cand, sizeof cand, g_root, n);
    if ((fs = file_size(cand)) >= 0)
        goto loose;
    join(cand, sizeof cand, g_root, base);
    if ((fs = file_size(cand)) >= 0)
        goto loose;

    e = index_find(n);
    if (!e)
        e = index_find(base);
    if (e) {
        if ((uint32_t)e->arch >= (uint32_t)g_narch)
            return 0;
        if (!archive_path(e->arch, real, rn))
            return 0;
        *off = e->off;
        *size = e->size;
        return 1;
    }

    /* A .dz the game names but we do not have is the main archive under a
     * variant name; serve the first mounted archive whole.
     *
     * Except a *_hires one. The image carries eight asset-pack names (RVA
     * 0x3b3952): gles1/dxt/atitc/etc, each in a standard and a _hires tier. It
     * asks for blackops_hires.dz -- the uncompressed hi-res pack -- and every
     * pack that actually exists is standard resolution, so substituting ours
     * hands it an archive whose hi-res resource names are absent. Refusing the
     * name instead lets the game fall back to a tier we can satisfy; if it has
     * no fallback we learn that too, which the silent substitution hid.
     *
     * BOZ_ANY_DZ=1 restores the old behaviour. */
    if (strlen(n) > 3 && strcmp(n + strlen(n) - 3, ".dz") == 0 && g_narch > 0) {
        if (strstr(n, "_hires") && !getenv("BOZ_ANY_DZ")) {
            printf("  [file ] refusing %s: no hi-res pack exists here\n", n);
            return 0;
        }
        if (!archive_path(0, real, rn))
            return 0;
        fs = file_size(real);
        *off = 0;
        *size = (uint32_t)fs;
        return 1;
    }
    return 0;

loose:
    snprintf(real, rn, "%s", cand);
    *off = 0;
    *size = (uint32_t)fs;
    return 1;
}

int s3e_vfs_exists(const char *name) {
    char real[PATH_MAX_], n[PATH_MAX_];
    uint32_t off, size;
    const SaveFile *e;
    norm(name, n, sizeof n);
    e = save_get(n, 0);
    if (e && e->present)
        return 1;
    return resolve(name, real, sizeof real, &off, &size) ? 1 : 0;
}

/* ------------------------------------------------------------- handles */

static Slot *slot_of(uint32_t h) {
    int i;
    if (h < S3E_FILE_HANDLE_BASE)
        return NULL;
    for (i = 0; i < MAX_SLOTS; i++)
        if (g_slot[i].used && g_slot[i].handle == h)
            return &g_slot[i];
    return NULL;
}

static uint32_t slot_new(FILE *fh, uint32_t base, uint32_t size,
                         const char *name, int writable) {
    int i;
    for (i = 0; i < MAX_SLOTS; i++) {
        if (g_slot[i].used)
            continue;
        g_slot[i].used = 1;
        g_slot[i].fh = fh;
        g_slot[i].base = base;
        g_slot[i].size = size;
        g_slot[i].pos = 0;
        g_slot[i].fpos = -1;
        g_slot[i].last_write = 0;
        g_slot[i].writable = writable;
        g_slot[i].sf = NULL;
        snprintf(g_slot[i].name, sizeof g_slot[i].name, "%s", name);
        g_slot[i].handle = g_next_handle;
        g_next_handle += 4u;
        g_err = S3E_FILE_ERR_NONE;
        return g_slot[i].handle;
    }
    if (fh)
        fclose(fh);
    return 0;
}

/* A handle on a save file held in memory. */
static uint32_t slot_new_save(SaveFile *e, const char *name, int writable) {
    uint32_t h = slot_new(NULL, 0, e->size, name, writable);
    Slot *s = slot_of(h);
    if (s)
        s->sf = e;
    return h;
}

/* SD access is much slower per call than per byte, and the game reads and
 * writes in small pieces -- so give every stream a buffer large enough that
 * those pieces coalesce. Must run before the first operation on the stream,
 * hence right after fopen. A failure here is not worth reporting: the stream
 * still works, just with the default buffer. */
static void set_buffer(FILE *fh) {
    setvbuf(fh, NULL, _IOFBF, 64u * 1024u);
}

/* Seek only when the stream is not already there. The game reads its archives
 * strictly forwards in small pieces, so in the common case this does nothing
 * and stdio serves the read from its buffer.
 *
 * `writing` is not a detail: on an update stream ("r+b"/"w+b") C requires a
 * seek or flush between a write and a following read, and vice versa. Skipping
 * the seek because the position already matched would breach exactly that
 * rule, so a change of direction forces one through. */
static int seek_to(Slot *s, long want, int writing) {
    if (s->fpos == want && s->last_write == writing)
        return 1;
    if (fseek(s->fh, want, SEEK_SET) != 0) {
        s->fpos = -1;           /* stream position is now anyone's guess */
        return 0;
    }
    s->fpos = want;
    s->last_write = writing;
    return 1;
}

uint32_t s3e_vfs_open(const char *name, const char *mode) {
    char real[PATH_MAX_], n[PATH_MAX_];
    uint32_t off = 0, size = 0;
    FILE *fh;

    norm(name, n, sizeof n);
    if (mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'))) {
        /* A save file, in memory (see the save cache). "w" truncates, and so
         * does opening one that does not exist -- the "w+b"/"r+b" choice the
         * stdio version made. The card is written when the handle closes. */
        SaveFile *e = save_get(n, 1);
        uint32_t h;
        if (!e) {
            g_err = S3E_FILE_ERR_NOT_FOUND;
            return 0;
        }
        if (strchr(mode, 'w') || !e->present)
            e->size = 0;
        e->present = 1;
        h = slot_new_save(e, name, 1);
        if (h && strchr(mode, 'a'))
            slot_of(h)->pos = e->size;
        if (g_save_trace < SAVE_TRACE_MAX) {
            g_save_trace++;
            printf("  [save ] open %s (%s) -> %08x size=%u\n", name, mode,
                   (unsigned)h, (unsigned)e->size);
        }
        return h;
    }
    {
        SaveFile *e = save_get(n, 0);
        if (e && e->present)
            return slot_new_save(e, name, 0);
    }

    if (!resolve(name, real, sizeof real, &off, &size)) {
        g_err = S3E_FILE_ERR_NOT_FOUND;
        return 0;
    }
    fh = fopen(real, "rb");
    if (!fh) {
        g_err = S3E_FILE_ERR_NOT_FOUND;
        return 0;
    }
    set_buffer(fh);
    return slot_new(fh, off, size, name, 0);
}

uint32_t s3e_vfs_read(uint32_t h, void *dst, uint32_t n) {
    Slot *s = slot_of(h);
    size_t got;
    if (!s || !n)
        return 0;
    if (s->sf) {
        const SaveFile *e = s->sf;
        if (s->pos >= e->size)
            return 0;
        if (n > e->size - s->pos)
            n = e->size - s->pos;
        memcpy(dst, e->data + s->pos, n);
        s->pos += n;
        return n;
    }
    if (s->pos >= s->size)
        return 0;
    if (n > s->size - s->pos)
        n = s->size - s->pos;
    if (!seek_to(s, (long)(s->base + s->pos), 0))
        return 0;
    got = fread(dst, 1, n, s->fh);
    s->pos += (uint32_t)got;
    s->fpos += (long)got;
    return (uint32_t)got;
}

uint32_t s3e_vfs_write(uint32_t h, const void *src, uint32_t n) {
    Slot *s = slot_of(h);
    size_t put;
    if (!s || !s->writable || !n)
        return 0;
    if (s->sf) {
        SaveFile *e = s->sf;
        if (!save_reserve(e, s->pos + n))
            return 0;
        memcpy(e->data + s->pos, src, n);
        s->pos += n;
        if (s->pos > e->size)
            e->size = s->pos;
        return n;
    }
    if (!seek_to(s, (long)(s->base + s->pos), 1))
        return 0;
    if (g_save_trace < SAVE_TRACE_MAX) {
        g_save_trace++;
        printf("  [save ] write %08x %s pos=%u n=%u\n", (unsigned)h, s->name,
               (unsigned)s->pos, (unsigned)n);
    }
    put = fwrite(src, 1, n, s->fh);
    s->pos += (uint32_t)put;
    s->fpos += (long)put;
    if (s->pos > s->size)
        s->size = s->pos;
    return (uint32_t)put;
}

int s3e_vfs_seek(uint32_t h, int32_t off, uint32_t origin) {
    Slot *s = slot_of(h);
    int64_t p;
    if (!s)
        return 1;
    if (s->sf)
        s->size = s->sf->size;
    switch (origin) {
    case 1:  p = (int64_t)s->pos + off; break;      /* SEEK_CUR */
    case 2:  p = (int64_t)s->size + off; break;     /* SEEK_END */
    default: p = off; break;                        /* SEEK_SET */
    }
    if (p < 0)
        p = 0;
    if (p > (int64_t)s->size)
        p = s->size;
    s->pos = (uint32_t)p;
    return 0;
}

uint32_t s3e_vfs_tell(uint32_t h) {
    Slot *s = slot_of(h);
    return s ? s->pos : 0;
}

uint32_t s3e_vfs_size(uint32_t h) {
    Slot *s = slot_of(h);
    if (s && s->sf)
        return s->sf->size;
    return s ? s->size : 0;
}

void s3e_vfs_close(uint32_t h) {
    Slot *s = slot_of(h);
    if (!s)
        return;
    if (s->writable && g_save_trace < SAVE_TRACE_MAX) {
        g_save_trace++;
        printf("  [save ] close %08x %s size=%u\n", (unsigned)h, s->name,
               (unsigned)(s->sf ? s->sf->size : s->size));
    }
    if (s->sf && s->writable)
        save_commit(s->sf);
    if (s->fh)
        fclose(s->fh);
    s->used = 0;
    s->fh = NULL;
    s->sf = NULL;
}

/* s3eFileFlush. It used to be a no-op in the HLE, which with buffered save
 * streams meant a flush followed by a read-back through another handle found
 * nothing written. */
int s3e_vfs_flush(uint32_t h) {
    Slot *s = slot_of(h);
    if (s && s->sf) {
        if (s->writable)
            save_commit(s->sf);
        return 0;
    }
    if (!s || !s->fh)
        return 1;
    return fflush(s->fh) == 0 ? 0 : 1;
}

int s3e_vfs_error(void) { return g_err; }

int s3e_vfs_delete(const char *name) {
    char real[PATH_MAX_], n[PATH_MAX_];
    norm(name, n, sizeof n);
    snprintf(real, sizeof real, "%s/save/%s", g_root, n);
    {
        SaveFile *e = save_get(n, 0);
        int rc;
        if (e) {
            SaveJob j;
            rc = e->present ? 0 : 1;
            e->present = 0;
            e->size = 0;
            memset(&j, 0, sizeof j);
            j.op = SJ_DELETE;
            snprintf(j.n, sizeof j.n, "%s", n);
            job_post(&j);
        } else {
            rc = remove(real) == 0 ? 0 : 1;
        }
        if (g_save_trace < SAVE_TRACE_MAX) {
            g_save_trace++;
            printf("  [save ] delete %s -> %s\n", name, rc ? "failed" : "ok");
        }
        return rc;
    }
}

int s3e_vfs_mkdir(const char *name) {
    char real[PATH_MAX_], n[PATH_MAX_];
    norm(name, n, sizeof n);
    snprintf(real, sizeof real, "%s/save/%s", g_root, n);
    mkdir_parents(real, 1);
    return 0;
}
