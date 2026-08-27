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
    int      writable;
    char     name[128];
} Slot;

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

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
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

/* Fills `real` with a path, and sets *off/*size. Returns 1 on success. */
static int resolve(const char *name, char *real, size_t rn,
                   uint32_t *off, uint32_t *size) {
    char n[PATH_MAX_], cand[PATH_MAX_];
    const char *base;
    const Entry *e;
    long fs;

    norm(name, n, sizeof n);
    base = basename_of(n);

    snprintf(cand, sizeof cand, "%s/save/%s", g_root, n);
    if ((fs = file_size(cand)) >= 0)
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
    char real[PATH_MAX_];
    uint32_t off, size;
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
        g_slot[i].writable = writable;
        snprintf(g_slot[i].name, sizeof g_slot[i].name, "%s", name);
        g_slot[i].handle = g_next_handle;
        g_next_handle += 4u;
        g_err = S3E_FILE_ERR_NONE;
        return g_slot[i].handle;
    }
    fclose(fh);
    return 0;
}

uint32_t s3e_vfs_open(const char *name, const char *mode) {
    char real[PATH_MAX_], n[PATH_MAX_];
    uint32_t off = 0, size = 0;
    FILE *fh;

    if (mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'))) {
        long fs;
        norm(name, n, sizeof n);
        snprintf(real, sizeof real, "%s/save/%s", g_root, n);
        mkdir_parents(real, 0);
        fs = file_size(real);
        fh = fopen(real, (strchr(mode, 'w') || fs < 0) ? "w+b" : "r+b");
        if (!fh) {
            g_err = S3E_FILE_ERR_NOT_FOUND;
            return 0;
        }
        if (strchr(mode, 'a'))
            fseek(fh, 0, SEEK_END);
        fs = file_size(real);
        return slot_new(fh, 0, fs > 0 ? (uint32_t)fs : 0u, name, 1);
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
    return slot_new(fh, off, size, name, 0);
}

uint32_t s3e_vfs_read(uint32_t h, void *dst, uint32_t n) {
    Slot *s = slot_of(h);
    size_t got;
    if (!s || !n)
        return 0;
    if (s->pos >= s->size)
        return 0;
    if (n > s->size - s->pos)
        n = s->size - s->pos;
    if (fseek(s->fh, (long)(s->base + s->pos), SEEK_SET) != 0)
        return 0;
    got = fread(dst, 1, n, s->fh);
    s->pos += (uint32_t)got;
    return (uint32_t)got;
}

uint32_t s3e_vfs_write(uint32_t h, const void *src, uint32_t n) {
    Slot *s = slot_of(h);
    size_t put;
    if (!s || !s->writable || !n)
        return 0;
    if (fseek(s->fh, (long)(s->base + s->pos), SEEK_SET) != 0)
        return 0;
    put = fwrite(src, 1, n, s->fh);
    s->pos += (uint32_t)put;
    if (s->pos > s->size)
        s->size = s->pos;
    return (uint32_t)put;
}

int s3e_vfs_seek(uint32_t h, int32_t off, uint32_t origin) {
    Slot *s = slot_of(h);
    int64_t p;
    if (!s)
        return 1;
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
    return s ? s->size : 0;
}

void s3e_vfs_close(uint32_t h) {
    Slot *s = slot_of(h);
    if (!s)
        return;
    if (s->fh)
        fclose(s->fh);
    s->used = 0;
    s->fh = NULL;
}

int s3e_vfs_error(void) { return g_err; }

int s3e_vfs_delete(const char *name) {
    char real[PATH_MAX_], n[PATH_MAX_];
    norm(name, n, sizeof n);
    snprintf(real, sizeof real, "%s/save/%s", g_root, n);
    return remove(real) == 0 ? 0 : 1;
}

int s3e_vfs_mkdir(const char *name) {
    char real[PATH_MAX_], n[PATH_MAX_];
    norm(name, n, sizeof n);
    snprintf(real, sizeof real, "%s/save/%s", g_root, n);
    mkdir_parents(real, 1);
    return 0;
}
