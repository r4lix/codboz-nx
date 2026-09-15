/* s3e_config.c -- see s3e_config.h. */
#include "s3e_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The game ships packs for four texture builds; "gles1" is the uncompressed
 * one every device can read, so it is the safe default when nothing else is
 * mounted. */
static char g_build_style[16] = "gles1";

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a - 'A' + 'a' : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b - 'A' + 'a' : *b;
        if (ca != cb)
            return 0;
    }
    return *a == *b;
}

void s3e_config_set_build_style(const char *style) {
    if (!style || !*style)
        return;
    snprintf(g_build_style, sizeof g_build_style, "%s", style);
}

const char *s3e_config_build_style(void) {
    return g_build_style;
}

struct config_entry {
    const char *sect, *key, *val;
};

#define S3E_CONFIG_ENTRY(s, k, v) { s, k, v },
static const struct config_entry g_enabled[] = { S3E_CONFIG_TABLE(S3E_CONFIG_ENTRY) };
static const struct config_entry g_parked[] = { S3E_CONFIG_PARKED(S3E_CONFIG_ENTRY) };
#undef S3E_CONFIG_ENTRY

static const char *lookup(const struct config_entry *t, size_t n,
                          const char *section, const char *key) {
    size_t i;
    for (i = 0; i < n; i++)
        if (ieq(section, t[i].sect) && ieq(key, t[i].key))
            return t[i].val;
    return NULL;
}

/* ------------------------------------------------------------ the game's ICF
 *
 * Parsed in place into a copy of the blob, so entries are pointers into one
 * owned buffer rather than 595 small allocations. */
#define ICF_MAX_ENTRIES 1024

static struct config_entry g_icf[ICF_MAX_ENTRIES];
static int g_icf_n;
static char *g_icf_text;

static char *trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

static void strip_quotes(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = 0;
    }
}

static const char *icf_get(const char *section, const char *key) {
    int i;
    for (i = 0; i < g_icf_n; i++)
        if (ieq(section, g_icf[i].sect) && ieq(key, g_icf[i].key))
            return g_icf[i].val;
    return NULL;
}

static void icf_set(const char *sect, const char *key, const char *val) {
    int i;
    for (i = 0; i < g_icf_n; i++)          /* later definitions win */
        if (ieq(sect, g_icf[i].sect) && ieq(key, g_icf[i].key)) {
            g_icf[i].val = val;
            return;
        }
    if (g_icf_n < ICF_MAX_ENTRIES) {
        g_icf[g_icf_n].sect = sect;
        g_icf[g_icf_n].key = key;
        g_icf[g_icf_n].val = val;
        g_icf_n++;
    }
}

/* {} is unconditional and {OS=ANDROID} is us -- this is the Android build of
 * the game. Every other platform and every per-device {ID=...} block is
 * skipped: taking them is what made serving this file fault before. Unknown
 * forms are skipped too, deliberately, so a condition nobody has read cannot
 * quietly switch settings on. */
static int icf_condition(char *cond) {
    if (!*cond)
        return 1;                       /* {} guards 186 keys */
    if (!strncmp(cond, "OS=", 3))
        return ieq(cond + 3, "ANDROID");
    /* CLASS=ANY means every device class, and guards 39 keys including
     * FreeStreamData and NumMemBuckets. Treating it as unmatched -- which the
     * first version of this did -- silently dropped all of them. */
    if (!strncmp(cond, "CLASS=", 6))
        return ieq(cond + 6, "ANY");
    /* {[SECTION] Key == value}, evaluated against what the file has set so
     * far. The file is sequential and {OS=ANDROID} sets iAndroidReleaseBuild
     * to 1 before the block that tests it, which is how those 20 keys arrive.
     * Both "Key==v" and "Key == v" spacings occur. */
    if (*cond == '[') {
        char *close = strchr(cond, ']');
        char *eq = strstr(cond, "==");
        const char *cur;
        if (!close || !eq || eq < close)
            return 0;
        *close = 0;
        *eq = 0;
        cur = icf_get(trim(cond + 1), trim(close + 1));
        return cur && ieq(cur, trim(eq + 2));
    }
    /* ID=<os> "device", ... is per-device and never us. Anything unrecognised
     * is skipped too, so an unread condition cannot switch settings on. */
    return 0;
}

/* Values may reference another key: "[UTIL] NumMemBuckets + 1". Resolved when
 * the line is read, since the ICF always defines the referent first; a
 * reference that cannot be resolved leaves the key unset rather than storing
 * something unparseable. */
static int icf_resolve(char *val, char *out, size_t out_len) {
    char *close, *sect, *rest, *op;
    long base, adj = 0;
    const char *cur;
    close = strchr(val, ']');
    if (!close)
        return 0;
    *close = 0;
    sect = trim(val + 1);
    rest = trim(close + 1);
    op = strpbrk(rest, "+-");
    if (op) {
        char *end;
        adj = strtol(trim(op + 1), &end, 0);
        if (*op == '-')
            adj = -adj;
        *op = 0;
        rest = trim(rest);
    }
    cur = icf_get(sect, rest);
    if (!cur)
        return 0;
    base = strtol(cur, NULL, 0);
    snprintf(out, out_len, "%ld", base + adj);
    return 1;
}

/* Resolved references need storage that outlives the line, and there are only
 * three in the whole file, so a small fixed pool is enough. Writing back over
 * the original value is not safe: icf_resolve NUL-terminates inside it while
 * parsing, so its length is no longer the length of what it replaces. */
static char g_icf_resolved[8][24];
static int g_icf_resolved_n;

/* The parse loop, shared by the game's own ICF and by the override file.
 * `owned` is modified in place and must outlive every key parsed from it,
 * because entries point into it rather than copying. */
static int icf_parse(char *owned) {
    char *p, *line, *next;
    const char *sect = "";
    int active = 1;

    for (p = owned; p && *p; p = next) {
        char *hash, *eq, *k, *v;
        size_t n;
        next = strchr(p, '\n');
        if (next)
            *next++ = 0;
        hash = strchr(p, '#');
        if (hash)
            *hash = 0;
        line = trim(p);
        n = strlen(line);
        if (!n)
            continue;
        if (line[0] == '{' && line[n - 1] == '}') {
            line[n - 1] = 0;
            active = icf_condition(trim(line + 1));
            continue;
        }
        if (line[0] == '[' && line[n - 1] == ']') {
            line[n - 1] = 0;
            /* Points into the owned buffer, so it stays valid for every entry
             * that follows -- a local array would leave them all naming the
             * last section in the file. */
            sect = trim(line + 1);
            continue;
        }
        if (!active)
            continue;
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        k = trim(line);
        v = trim(eq + 1);
        strip_quotes(v);
        if (!*k)
            continue;
        if (*v == '[') {
            char out[24];
            if (!icf_resolve(v, out, sizeof out) ||
                g_icf_resolved_n >= (int)(sizeof g_icf_resolved /
                                          sizeof g_icf_resolved[0]))
                continue;               /* unresolvable: leave the key unset */
            snprintf(g_icf_resolved[g_icf_resolved_n], sizeof g_icf_resolved[0],
                     "%s", out);
            v = g_icf_resolved[g_icf_resolved_n++];
        }
        icf_set(sect, k, v);
    }
    return g_icf_n;
}

int s3e_config_load_icf(const char *text, unsigned len) {
    if (!text || !len || g_icf_text)
        return 0;
    g_icf_text = (char *)malloc((size_t)len + 1);
    if (!g_icf_text)
        return 0;
    memcpy(g_icf_text, text, len);
    g_icf_text[len] = 0;
    return icf_parse(g_icf_text);
}

/* Extra keys from the card, applied AFTER the game's own ICF.
 *
 * icf_set lets later definitions win, so this overrides anything the game
 * shipped and can introduce keys it never set. It exists because the only way
 * to ask this engine a question is to change a config value and watch, and
 * doing that through a rebuild-and-deploy cycle per key is how an experiment
 * turns into an afternoon. The file is optional and absent by default. */
static char *g_icf_over;

int s3e_config_load_overrides(const char *text, unsigned len) {
    if (!text || !len || g_icf_over)
        return 0;
    g_icf_over = (char *)malloc((size_t)len + 1);
    if (!g_icf_over)
        return 0;
    memcpy(g_icf_over, text, len);
    g_icf_over[len] = 0;
    return icf_parse(g_icf_over);
}

const char *s3e_config_get(const char *section, const char *key) {
    if (!section)
        section = "";
    if (!key)
        key = "";

    /* Answered from the mounted pack rather than the table: naming a build
     * the archives do not hold leaves the resource manager with no textures
     * at all, which is worse than the software fallback. */
    if (ieq(section, "RESMANAGER") && ieq(key, "ResBuildStyle"))
        return g_build_style;

    {   /* Deliberate overrides win over the file; the file wins over nothing.
         * Parked keys are not overrides, so if the ICF names one the game gets
         * its own value -- what is parked is the PortMaster number, not the
         * key itself. */
        const char *v = lookup(g_enabled,
                               sizeof g_enabled / sizeof g_enabled[0],
                               section, key);
        if (v)
            return v;
    }
    return icf_get(section, key);
}

const char *s3e_config_parked(const char *section, const char *key) {
    return lookup(g_parked, sizeof g_parked / sizeof g_parked[0],
                  section ? section : "", key ? key : "");
}
