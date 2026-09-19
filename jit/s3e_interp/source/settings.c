/* settings.c -- see settings.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"

#define MAX_LINES 96
#define LINE_LEN  256

/* The file as lines. A line with a key keeps it split out for lookups; any
 * other line is kept verbatim in `raw`. */
typedef struct {
    char key[48];
    char value[LINE_LEN];
    char raw[LINE_LEN];
    int  is_pair;
} Line;

static Line g_lines[MAX_LINES];
static int  g_count;
static int  g_dirty;

static void trim(char *s) {
    size_t n = strlen(s), start = 0;
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' ||
                 s[n - 1] == '\t'))
        s[--n] = 0;
    while (s[start] == ' ' || s[start] == '\t')
        start++;
    if (start)
        memmove(s, s + start, n - start + 1);
}

static Line *find(const char *key) {
    int i;
    for (i = 0; i < g_count; i++)
        if (g_lines[i].is_pair && !strcmp(g_lines[i].key, key))
            return &g_lines[i];
    return NULL;
}

void settings_load(void) {
    FILE *f = fopen(SETTINGS_PATH, "r");
    char buf[LINE_LEN];
    g_count = 0;
    g_dirty = 0;
    if (!f)
        return;
    while (g_count < MAX_LINES && fgets(buf, sizeof buf, f)) {
        Line *l = &g_lines[g_count++];
        char *eq;
        memset(l, 0, sizeof *l);
        trim(buf);
        snprintf(l->raw, sizeof l->raw, "%s", buf);
        if (buf[0] == '#' || !(eq = strchr(buf, '=')))
            continue;
        *eq = 0;
        trim(buf);
        if (!buf[0] || strlen(buf) >= sizeof l->key)
            continue;
        snprintf(l->key, sizeof l->key, "%s", buf);
        snprintf(l->value, sizeof l->value, "%s", eq + 1);
        trim(l->value);
        l->is_pair = 1;
    }
    fclose(f);
}

const char *settings_get(const char *key, const char *fallback) {
    const Line *l = find(key);
    return l ? l->value : fallback;
}

int settings_get_int(const char *key, int fallback) {
    const Line *l = find(key);
    char *end;
    long v;
    if (!l || !l->value[0])
        return fallback;
    v = strtol(l->value, &end, 10);
    return *end ? fallback : (int)v;
}

void settings_set(const char *key, const char *value) {
    Line *l = find(key);
    if (!value)
        value = "";
    if (l) {
        if (!strcmp(l->value, value))
            return;
        snprintf(l->value, sizeof l->value, "%s", value);
    } else {
        if (g_count >= MAX_LINES)
            return;
        l = &g_lines[g_count++];
        memset(l, 0, sizeof *l);
        snprintf(l->key, sizeof l->key, "%s", key);
        snprintf(l->value, sizeof l->value, "%s", value);
        l->is_pair = 1;
    }
    g_dirty = 1;
}

void settings_set_int(const char *key, int value) {
    char buf[16];
    snprintf(buf, sizeof buf, "%d", value);
    settings_set(key, buf);
}

int settings_save(void) {
    FILE *f;
    int i;
    if (!g_dirty)
        return 1;
    f = fopen(SETTINGS_PATH, "w");
    if (!f)
        return 0;
    for (i = 0; i < g_count; i++) {
        if (g_lines[i].is_pair)
            fprintf(f, "%s=%s\n", g_lines[i].key, g_lines[i].value);
        else
            fprintf(f, "%s\n", g_lines[i].raw);
    }
    fclose(f);
    g_dirty = 0;
    return 1;
}
