/* s3e_config.c -- see s3e_config.h. */
#include "s3e_config.h"

#include <stdio.h>
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

    return lookup(g_enabled, sizeof g_enabled / sizeof g_enabled[0],
                  section, key);
}

const char *s3e_config_parked(const char *section, const char *key) {
    return lookup(g_parked, sizeof g_parked / sizeof g_parked[0],
                  section ? section : "", key ? key : "");
}
