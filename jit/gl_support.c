/* gl_support.c -- see gl_thunks.h. */
#include "gl_thunks.h"

#include <stdio.h>
#include <string.h>

static uint32_t (*g_alloc)(uint32_t);

void gl_set_allocator(uint32_t (*alloc)(uint32_t)) { g_alloc = alloc; }

GuestHleFn gl_find_thunk(const char *name) {
    unsigned i;
    if (!name)
        return NULL;
#ifdef __SWITCH__
    {   /* EGL first: the generated thunks for it are unfixable in place. */
        GuestHleFn egl = egl_find_override(name);
        if (egl)
            return egl;
    }
#endif
    for (i = 0; g_gl_thunks[i].name; i++)
        if (strcmp(g_gl_thunks[i].name, name) == 0)
            return g_gl_thunks[i].fn;
    return NULL;
}

/* GL hands back a handful of static strings (vendor, renderer, version,
 * extensions) and does so on every query, so cache by host pointer instead of
 * burning guest heap each call. */
#define CSTR_CACHE 16
static struct { const char *host; uint32_t guest; } g_cstr[CSTR_CACHE];
static unsigned g_cstr_n;

uint32_t gl_return_cstr(GuestMem *m, const char *s) {
    uint32_t addr;
    size_t len, i;

    if (!s || !g_alloc)
        return 0;
    for (i = 0; i < g_cstr_n; i++)
        if (g_cstr[i].host == s)
            return g_cstr[i].guest;

    len = strlen(s);
    addr = g_alloc((uint32_t)len + 1u);
    if (!addr)
        return 0;
    for (i = 0; i <= len; i++)
        guest_st8(m, addr + (uint32_t)i, (uint32_t)(unsigned char)s[i]);

    if (g_cstr_n < CSTR_CACHE) {
        g_cstr[g_cstr_n].host = s;
        g_cstr[g_cstr_n].guest = addr;
        g_cstr_n++;
    }
    return addr;
}

/* Declared only by the GLES2/3 headers, so absent from libGLESv1_CM. This is
 * the gles1 build of the game, which should never call them -- but say so if
 * it does, rather than silently returning 0 and drawing nothing. */
void gl_unimplemented(const char *name) {
    static int shown;
    if (shown < 16) {
        printf("  [gl   ] %s is GLES2/3-only, not in this build\n", name);
        shown++;
    }
}

void gl_unsupported_ptr_return(const char *name) {
    static int shown;
    if (shown < 8) {
        printf("  [gl   ] %s returns a host pointer -- unrepresentable, "
               "returning 0\n", name);
        shown++;
    }
}
