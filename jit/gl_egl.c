/* gl_egl.c -- hand-written EGL entry points that replace the generated ones.
 *
 * The generated thunks in gl_thunks.c cannot be correct for EGL. The guest is
 * 32-bit ARM, so every argument and return value is 32 bits wide, but on
 * aarch64 EGLDisplay, EGLConfig, EGLSurface and EGLContext are all `void *`
 * (EGL/egl.h:59-64) -- 64 bits. The generated form
 *
 *     c->r[0] = (uint32_t)eglGetDisplay((EGLNativeDisplayType)ga(c, m, 0));
 *
 * truncates the display to its low word on the way out, and the next call
 * zero-extends that half-pointer back into a 64-bit address that is not the
 * _EGLDisplay object. Mesa rejects it with EGL_BAD_DISPLAY, the game calls
 * eglGetError, gives up, and falls back to its software rasteriser. That is
 * exactly the four-call sequence measured on hardware:
 * eglGetCurrentContext, eglGetDisplay(0), eglInitialize, eglGetError.
 *
 * NOTHING HERE MAY CALL eglGetError(). It clears the error as it reads it, and
 * the guest's initialiser (RVA 0x2c752c) decides whether EGL came up by doing
 *     bl eglGetError ; cmp r0, #0x3000 ; beq success
 * right after eglInitialize. A log line that consumed the error first would
 * hand the guest EGL_SUCCESS and send it down the success path after a failed
 * init. te_GetError below is the guest's own call and is the only reader.
 *
 * 21 of the 23 EGL entry points this image imports are affected; only
 * eglGetError and eglBindAPI pass nothing but scalars.
 *
 * Rather than regenerate gl_thunks.c -- which would put the 224 working GL
 * thunks at risk for the sake of 23 EGL ones -- these are hand-written and
 * gl_find_thunk() consults them first.
 *
 * The fix is a handle table: the guest is handed a small tagged token, and the
 * real pointer never leaves this file. The one rule that matters is that 0 is
 * a fixed point in BOTH directions, because every EGL sentinel the guest can
 * see -- EGL_NO_DISPLAY, EGL_NO_CONTEXT, EGL_NO_SURFACE, EGL_DEFAULT_DISPLAY --
 * is numerically 0, and the guest tests handles against 0. If 0 were allocated
 * a slot, a failed call would read as success; if a real handle mapped to 0,
 * success would read as failure. (The latter is a live hazard with the current
 * code: a mesa pointer whose low word happens to be zero turns a SUCCESSFUL
 * eglGetDisplay into EGL_NO_DISPLAY as far as the guest is concerned.)
 */
#ifdef __SWITCH__

#include <switch.h>
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "gl_thunks.h"

/* main.c owns the NWindow. A libnx Framebuffer (which is what consoleInit
 * installs) and an EGL window surface cannot share one NWindow: framebufferCreate
 * and the EGL backend both call nwindowSetDimensions/nwindowConfigureBuffer,
 * and the second one gets LibnxError_AlreadyInitialized. So the window has to
 * be taken away from its current owner before eglCreateWindowSurface. */
extern void *egl_take_window(void);

/* Counts a GL frame so the input state machine still advances -- see main.c. */
extern void egl_frame_presented(void);

/* ----------------------------------------------------------- guest access */

static uint32_t ga(GuestCpu *c, GuestMem *m, unsigned slot) {
    uint32_t v = 0;
    if (slot < 4)
        return c->r[slot];
    guest_ld32(m, c->r[GUEST_SP] + 4u * (slot - 4u), &v);
    return v;
}

static void *gp(GuestMem *m, uint32_t addr) {
    return addr ? guest_ptr(m, addr, 1) : NULL;
}

static int g_log = 1;
static int g_dumped_gl;
#define ELOG(...) do { if (g_log) printf(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------ handle table */

/* A stray token is immediately recognisable in a register dump, and the tag
 * makes a guest-invented value fail the lookup instead of indexing the table. */
#define TOK_TAG 0x0E610000u
#define TOK_MAX 192u        /* one display, a context, a surface or two, and a
                             * token for every config the game enumerates */
#define CFG_MAX 128u        /* scratch for eglGetConfigs, a separate limit */

/* Kinds are checked on every lookup. The guest is a shipped game and will not
 * confuse a display with a surface, so this exists to catch OUR marshalling
 * mistakes loudly instead of resolving a token to a valid-but-wrong pointer
 * and rendering nothing for a reason nobody can see. */
typedef enum { K_ANY = 0, K_DPY, K_CFG, K_CTX, K_SFC } Kind;

static const char *kind_name(Kind k) {
    switch (k) {
    case K_DPY: return "display";
    case K_CFG: return "config";
    case K_CTX: return "context";
    case K_SFC: return "surface";
    default:    return "handle";
    }
}

typedef struct {
    void *host;
    void *owner;        /* the display a surface/context belongs to */
    unsigned char live;
    unsigned char kind;
} Slot;

static Slot g_slot[TOK_MAX];
static unsigned g_slots;
static int g_exhausted;

/* Host -> guest. NULL is 0 and is never allocated a slot. A handle already in
 * the table keeps its token: EGL hands back the same EGLDisplay for repeated
 * eglGetDisplay calls, and the guest may compare them. */
static uint32_t tok_out(void *h, void *owner, Kind k) {
    unsigned i;
    if (!h)
        return 0;
    for (i = 0; i < g_slots; i++)
        if (g_slot[i].live && g_slot[i].host == h && g_slot[i].kind == k)
            return TOK_TAG | (i + 1u);
    if (g_slots >= TOK_MAX) {
        if (!g_exhausted) {
            g_exhausted = 1;
            printf("  [egl  ] handle table full (%u) -- refusing to alias\n",
                   (unsigned)TOK_MAX);
        }
        return 0;              /* never hand back someone else's handle */
    }
    g_slot[g_slots].host = h;
    g_slot[g_slots].owner = owner;
    g_slot[g_slots].live = 1;
    g_slot[g_slots].kind = (unsigned char)k;
    g_slots++;
    return TOK_TAG | g_slots;  /* index + 1, so a token is never 0 */
}

/* Guest -> host. Returns 0 for a token that is not ours or has been retired;
 * the caller must fail the whole call rather than substitute NULL, because
 * EGL treats NULL as a meaningful sentinel and a bogus handle silently turned
 * into EGL_NO_SURFACE would unbind the context instead of raising an error. */
static int tok_in(uint32_t t, void **out, Kind want) {
    unsigned i;
    if (!t) {                  /* EGL_NO_DISPLAY / _CONTEXT / _SURFACE */
        *out = NULL;
        return 1;
    }
    if ((t & 0xFFFF0000u) != TOK_TAG)
        return 0;
    i = t & 0xFFFFu;
    if (i == 0u || i > g_slots || !g_slot[i - 1u].live)
        return 0;
    if (want != K_ANY && g_slot[i - 1u].kind != want) {
        ELOG("  [egl  ] token %08x is a %s, used as a %s\n", (unsigned)t,
             kind_name((Kind)g_slot[i - 1u].kind), kind_name(want));
        return 0;
    }
    *out = g_slot[i - 1u].host;
    return 1;
}

/* Destroy retires the slot, it does not free it. EGL destroys are deferred:
 * destroying the bound surface or current context only flags the object, which
 * stays live until eglMakeCurrent unbinds it. Recycling the index would let a
 * later create take it, so the guest's stale handle would resolve to a
 * different LIVE object -- wrong rendering with no error anywhere. Retired
 * means a stale use fails the lookup, which is what a conformant driver does. */
static void tok_retire(uint32_t t) {
    unsigned i;
    if (!t || (t & 0xFFFF0000u) != TOK_TAG)
        return;
    i = t & 0xFFFFu;
    if (i && i <= g_slots)
        g_slot[i - 1u].live = 0;
}

static void tok_retire_owned_by(void *dpy) {
    unsigned i;
    for (i = 0; i < g_slots; i++)
        if (g_slot[i].owner == dpy || g_slot[i].host == dpy)
            g_slot[i].live = 0;
}

/* ------------------------------------------------------------- entry points */

#define BAD_HANDLE(what)                                                      \
    do {                                                                      \
        ELOG("  [egl  ] %s: bad handle from guest\n", (what));                \
        c->r[0] = (uint32_t)EGL_FALSE;                                        \
        return;                                                               \
    } while (0)

static void te_GetDisplay(GuestCpu *c, GuestMem *m, void *u) {
    uint32_t arg = ga(c, m, 0);
    EGLDisplay d;
    (void)u; (void)m;
    /* Measured: the guest passes 0, i.e. EGL_DEFAULT_DISPLAY. Any other value
     * is a 32-bit guest number and cannot be a host native display, so use the
     * default rather than fabricating a pointer out of it. */
    if (arg)
        ELOG("  [egl  ] GetDisplay(%08x) is not EGL_DEFAULT_DISPLAY; "
             "using the default anyway\n", (unsigned)arg);
    d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    c->r[0] = tok_out(d, d, K_DPY);
    ELOG("  [egl  ] GetDisplay -> host %p token %08x%s\n", d,
         (unsigned)c->r[0], d ? "" : "  (EGL_NO_DISPLAY!)");
}

static void te_Initialize(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy;
    EGLint *maj = (EGLint *)gp(m, ga(c, m, 1));
    EGLint *min = (EGLint *)gp(m, ga(c, m, 2));
    EGLBoolean r;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY))
        BAD_HANDLE("Initialize");
    r = eglInitialize((EGLDisplay)dpy, maj, min);
    c->r[0] = (uint32_t)r;
    ELOG("  [egl  ] Initialize(%08x) -> %d  version %d.%d\n",
         (unsigned)ga(c, m, 0), (int)r, maj ? (int)*maj : -1,
         min ? (int)*min : -1);
}

static void te_Terminate(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY))
        BAD_HANDLE("Terminate");
    c->r[0] = (uint32_t)eglTerminate((EGLDisplay)dpy);
    /* Only on success, and never for NULL -- eglGetCurrentContext records its
     * token with a NULL owner, so sweeping owner==NULL would retire live
     * handles that this display never owned. */
    if (c->r[0] && dpy)
        tok_retire_owned_by(dpy);
    ELOG("  [egl  ] Terminate -> %d\n", (int)c->r[0]);
}

static void te_GetError(GuestCpu *c, GuestMem *m, void *u) {
    (void)m; (void)u;
    c->r[0] = (uint32_t)eglGetError();
    ELOG("  [egl  ] GetError -> %04x\n", (unsigned)c->r[0]);
}

static void te_BindAPI(GuestCpu *c, GuestMem *m, void *u) {
    (void)u;
    c->r[0] = (uint32_t)eglBindAPI((EGLenum)ga(c, m, 0));
    ELOG("  [egl  ] BindAPI(%04x) -> %d\n", (unsigned)ga(c, m, 0),
         (int)c->r[0]);
}

static void te_QueryString(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy;
    const char *s;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY)) {
        c->r[0] = 0;
        return;
    }
    s = eglQueryString((EGLDisplay)dpy, (EGLint)ga(c, m, 1));
    c->r[0] = gl_return_cstr(m, s);
    ELOG("  [egl  ] QueryString(%04x) -> \"%s\"\n", (unsigned)ga(c, m, 1),
         s ? s : "(null)");
}

/* The only entry point that moves an ARRAY of handles. The host writes 8-byte
 * EGLConfigs; the guest's slots are 4 bytes. Handing the guest buffer straight
 * to EGL -- which is what the generated thunk does -- both strides wrong (the
 * guest reads element 1 as the high half of element 0) and overruns the guest
 * allocation by a factor of two. So collect host-side and write tokens back. */
static void te_GetConfigs(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy;
    EGLConfig tmp[CFG_MAX];
    uint32_t garr = ga(c, m, 1);
    EGLint want = (EGLint)ga(c, m, 2);
    EGLint *nret = (EGLint *)gp(m, ga(c, m, 3));
    EGLint n = 0, i;
    EGLBoolean r;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY))
        BAD_HANDLE("GetConfigs");
    if (want < 0)
        want = 0;
    if (want > (EGLint)CFG_MAX) {
        /* Never truncate silently: the guest would believe it had seen every
         * config and pick from a set we quietly cut short. */
        ELOG("  [egl  ] GetConfigs: guest asked for %d configs, capping at %u\n",
             (int)want, (unsigned)CFG_MAX);
        want = (EGLint)CFG_MAX;
    }
    r = eglGetConfigs((EGLDisplay)dpy, garr ? tmp : NULL,
                      garr ? want : 0, &n);
    if (r && garr)
        for (i = 0; i < n && i < want; i++)
            guest_st32(m, garr + 4u * (uint32_t)i, tok_out(tmp[i], dpy, K_CFG));
    if (nret)
        *nret = n;
    c->r[0] = (uint32_t)r;
    ELOG("  [egl  ] GetConfigs(want=%d) -> %d, %d configs\n", (int)want,
         (int)r, (int)n);
}

static void te_GetConfigAttrib(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy, *cfg;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY) || !tok_in(ga(c, m, 1), &cfg, K_CFG))
        BAD_HANDLE("GetConfigAttrib");
    c->r[0] = (uint32_t)eglGetConfigAttrib((EGLDisplay)dpy, (EGLConfig)cfg,
                                           (EGLint)ga(c, m, 2),
                                           (EGLint *)gp(m, ga(c, m, 3)));
}

static void te_CreateContext(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy, *cfg, *share;
    EGLContext ctx;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY) || !tok_in(ga(c, m, 1), &cfg, K_CFG) ||
        !tok_in(ga(c, m, 2), &share, K_CTX)) {
        ELOG("  [egl  ] CreateContext: bad handle from guest\n");
        c->r[0] = 0;
        return;
    }
    ctx = eglCreateContext((EGLDisplay)dpy, (EGLConfig)cfg,
                           (EGLContext)share,
                           (const EGLint *)gp(m, ga(c, m, 3)));
    c->r[0] = tok_out(ctx, dpy, K_CTX);
    ELOG("  [egl  ] CreateContext -> host %p token %08x\n", ctx,
         (unsigned)c->r[0]);
}

static void te_DestroyContext(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy, *ctx;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY) || !tok_in(ga(c, m, 1), &ctx, K_CTX))
        BAD_HANDLE("DestroyContext");
    c->r[0] = (uint32_t)eglDestroyContext((EGLDisplay)dpy, (EGLContext)ctx);
    if (c->r[0])
        tok_retire(ga(c, m, 1));
}

static void te_QueryContext(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy, *ctx;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY) || !tok_in(ga(c, m, 1), &ctx, K_CTX))
        BAD_HANDLE("QueryContext");
    c->r[0] = (uint32_t)eglQueryContext((EGLDisplay)dpy, (EGLContext)ctx,
                                        (EGLint)ga(c, m, 2),
                                        (EGLint *)gp(m, ga(c, m, 3)));
}

static void te_GetCurrentContext(GuestCpu *c, GuestMem *m, void *u) {
    EGLContext k = eglGetCurrentContext();
    (void)m; (void)u;
    c->r[0] = tok_out(k, NULL, K_CTX);
}

static void te_GetCurrentDisplay(GuestCpu *c, GuestMem *m, void *u) {
    EGLDisplay d = eglGetCurrentDisplay();
    (void)m; (void)u;
    c->r[0] = tok_out(d, d, K_DPY);
}

static void te_GetCurrentSurface(GuestCpu *c, GuestMem *m, void *u) {
    EGLSurface s = eglGetCurrentSurface((EGLint)ga(c, m, 0));
    (void)u;
    c->r[0] = tok_out(s, NULL, K_SFC);
}

static void te_CreateWindowSurface(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy, *cfg, *win;
    EGLSurface s;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY) || !tok_in(ga(c, m, 1), &cfg, K_CFG)) {
        ELOG("  [egl  ] CreateWindowSurface: bad handle from guest\n");
        c->r[0] = 0;
        return;
    }
    /* The guest's native-window argument is an Android handle from another
     * lifetime; it means nothing here. Take the real NWindow off whoever holds
     * it -- the console at startup, or the software-surface Framebuffer -- and
     * pass that. libnx refuses a second binder on one NWindow, so the release
     * has to happen before the call, not after. */
    ELOG("  [egl  ] CreateWindowSurface: guest window %08x ignored\n",
         (unsigned)ga(c, m, 2));
    win = egl_take_window();
    s = eglCreateWindowSurface((EGLDisplay)dpy, (EGLConfig)cfg,
                               (EGLNativeWindowType)win,
                               (const EGLint *)gp(m, ga(c, m, 3)));
    c->r[0] = tok_out(s, dpy, K_SFC);
    ELOG("  [egl  ] CreateWindowSurface -> host %p token %08x\n", s,
         (unsigned)c->r[0]);
}

static void te_CreatePbufferSurface(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy, *cfg;
    EGLSurface s;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY) || !tok_in(ga(c, m, 1), &cfg, K_CFG)) {
        c->r[0] = 0;
        return;
    }
    s = eglCreatePbufferSurface((EGLDisplay)dpy, (EGLConfig)cfg,
                                (const EGLint *)gp(m, ga(c, m, 2)));
    c->r[0] = tok_out(s, dpy, K_SFC);
    ELOG("  [egl  ] CreatePbufferSurface -> token %08x\n", (unsigned)c->r[0]);
}

static void te_DestroySurface(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy, *s;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY) || !tok_in(ga(c, m, 1), &s, K_SFC))
        BAD_HANDLE("DestroySurface");
    c->r[0] = (uint32_t)eglDestroySurface((EGLDisplay)dpy, (EGLSurface)s);
    if (c->r[0])
        tok_retire(ga(c, m, 1));
}

static void te_QuerySurface(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy, *s;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY) || !tok_in(ga(c, m, 1), &s, K_SFC))
        BAD_HANDLE("QuerySurface");
    c->r[0] = (uint32_t)eglQuerySurface((EGLDisplay)dpy, (EGLSurface)s,
                                        (EGLint)ga(c, m, 2),
                                        (EGLint *)gp(m, ga(c, m, 3)));
}

/* All four handles are resolved before the call and any single bad one fails
 * the whole thing: EGL requires draw, read and ctx to be either all EGL_NO_*
 * or all real, so quietly turning one bad token into NULL would synthesise an
 * illegal mixed call. */
static void te_MakeCurrent(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy, *draw, *read, *ctx;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY) || !tok_in(ga(c, m, 1), &draw, K_SFC) ||
        !tok_in(ga(c, m, 2), &read, K_SFC) || !tok_in(ga(c, m, 3), &ctx, K_CTX))
        BAD_HANDLE("MakeCurrent");
    c->r[0] = (uint32_t)eglMakeCurrent((EGLDisplay)dpy, (EGLSurface)draw,
                                       (EGLSurface)read, (EGLContext)ctx);
    ELOG("  [egl  ] MakeCurrent(draw=%08x ctx=%08x) -> %d\n",
         (unsigned)ga(c, m, 1), (unsigned)ga(c, m, 3), (int)c->r[0]);
    if (c->r[0] && !g_dumped_gl) {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        g_dumped_gl = 1;
        printf("  [gl   ] vendor   %s\n", (const char *)glGetString(GL_VENDOR));
        printf("  [gl   ] renderer %s\n", (const char *)glGetString(GL_RENDERER));
        printf("  [gl   ] version  %s\n", (const char *)glGetString(GL_VERSION));
        /* long; print in slices so nxlink does not drop it */
        if (ext) {
            size_t n = strlen(ext), o2;
            for (o2 = 0; o2 < n; o2 += 180)
                printf("  [gl   ] ext %.180s\n", ext + o2);
        }
    }
}

static void te_SwapBuffers(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy, *s;
    static int shown;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY) || !tok_in(ga(c, m, 1), &s, K_SFC))
        BAD_HANDLE("SwapBuffers");
    c->r[0] = (uint32_t)eglSwapBuffers((EGLDisplay)dpy, (EGLSurface)s);
    if (c->r[0])
        egl_frame_presented();
    if (shown < 3) {
        shown++;
        ELOG("  [egl  ] SwapBuffers -> %d  (GL IS PRESENTING)\n",
             (int)c->r[0]);
    }
}

static void te_BindTexImage(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy, *s;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY) || !tok_in(ga(c, m, 1), &s, K_SFC))
        BAD_HANDLE("BindTexImage");
    c->r[0] = (uint32_t)eglBindTexImage((EGLDisplay)dpy, (EGLSurface)s,
                                        (EGLint)ga(c, m, 2));
}

static void te_ReleaseTexImage(GuestCpu *c, GuestMem *m, void *u) {
    void *dpy, *s;
    (void)u;
    if (!tok_in(ga(c, m, 0), &dpy, K_DPY) || !tok_in(ga(c, m, 1), &s, K_SFC))
        BAD_HANDLE("ReleaseTexImage");
    c->r[0] = (uint32_t)eglReleaseTexImage((EGLDisplay)dpy, (EGLSurface)s,
                                           (EGLint)ga(c, m, 2));
}

/* A host function pointer cannot be handed to a 32-bit guest, and the guest
 * would call it with the ARM ABI regardless. 0 is the conformant answer for an
 * unavailable entry point, and every caller has to handle it. */
static void te_GetProcAddress(GuestCpu *c, GuestMem *m, void *u) {
    static int shown;
    const char *nm = (const char *)gp(m, ga(c, m, 0));
    (void)u;
    c->r[0] = 0;
    if (shown < 8) {
        shown++;
        ELOG("  [egl  ] GetProcAddress(\"%s\") -> 0 (host pointer is "
             "unrepresentable in a 32-bit guest)\n", nm ? nm : "?");
    }
}

/* ------------------------------------------------- viewport rescaling -----
 *
 * The game believes the screen is 480x320 -- that is what s3eSurfaceGetInt
 * reports -- so it calls glViewport(0, 0, 480, 320) and draws into the bottom
 * left corner of our 1280x720 window, leaving the rest of the panel showing
 * whatever was cleared last.
 *
 * Rather than lie about the screen size (480x320 is 3:2 and the panel is 16:9,
 * so the game's own layout would stretch), map its viewport into the same
 * letterboxed box the software path already uses: 480x320 scaled by 9/4 gives
 * 1080x720, centred at x=100. glScissor is in the same coordinate space and
 * must be scaled identically or clipping cuts the wrong region.
 *
 * GL measures both from the bottom left, so y needs no flip. */
#define VP_W 480
#define VP_H 320
#define VP_DST_W 1080
#define VP_DST_H 720
#define VP_X0 ((1280 - VP_DST_W) / 2)

static int vp_map_x(int v) { return VP_X0 + (v * VP_DST_W) / VP_W; }
static int vp_map_y(int v) { return (v * VP_DST_H) / VP_H; }
static int vp_map_w(int v) { return (v * VP_DST_W) / VP_W; }
static int vp_map_h(int v) { return (v * VP_DST_H) / VP_H; }

static void te_Viewport(GuestCpu *c, GuestMem *m, void *u) {
    static int shown;
    int x = (int)ga(c, m, 0), y = (int)ga(c, m, 1);
    int w = (int)ga(c, m, 2), h = (int)ga(c, m, 3);
    (void)u;
    if (shown < 4) {
        shown++;
        ELOG("  [gl   ] Viewport(%d,%d,%d,%d) -> (%d,%d,%d,%d)\n", x, y, w, h,
             vp_map_x(x), vp_map_y(y), vp_map_w(w), vp_map_h(h));
    }
    glViewport(vp_map_x(x), vp_map_y(y), vp_map_w(w), vp_map_h(h));
}

static void te_Scissor(GuestCpu *c, GuestMem *m, void *u) {
    int x = (int)ga(c, m, 0), y = (int)ga(c, m, 1);
    int w = (int)ga(c, m, 2), h = (int)ga(c, m, 3);
    (void)u;
    glScissor(vp_map_x(x), vp_map_y(y), vp_map_w(w), vp_map_h(h));
}

/* --------------------------------------------- bulk transfers and VBOs ----
 *
 * Two defects the generated thunks share, both found by auditing this layer
 * once GL started working:
 *
 * 1. gp() passes len=1 to guest_ptr(). guest_ptr DOES validate a full extent
 *    (guest.c:19-31), but with len=1 that check is unsatisfiable, so it is
 *    dead on every GL pointer call site. The host driver then reads or writes
 *    the real span with no bound at all, and every guest region is an
 *    exact-size calloc with no guard page -- so a large glTexImage2D near the
 *    end of a region walks off the host heap. gpn() passes the true span.
 *
 * 2. glVertexPointer/glColorPointer/glTexCoordPointer/glNormalPointer and
 *    glDrawElements take an argument whose meaning depends on GL state: a
 *    client address when no buffer is bound, a byte OFFSET when one is. The
 *    generated thunks translate it as an address either way, so with a VBO
 *    bound a small offset like 0 or 12 becomes a bogus guest pointer. This
 *    game does use buffer objects -- there is a real glGenBuffers/glBindBuffer/
 *    glBufferData routine at RVA 0x2d90a8 -- so the binding is tracked here
 *    and offsets are passed through untranslated.
 */
static uint32_t g_buf_array, g_buf_elem;   /* currently bound buffer names */

static void *gpn(GuestMem *m, uint32_t addr, uint64_t nbytes, const char *who) {
    void *p;
    if (!addr)
        return NULL;                    /* the guest meant NULL: preserve it */
    if (!nbytes)
        return gp(m, addr);             /* extent genuinely unknown */
    if (nbytes > 0xFFFFFFFFu)
        nbytes = 0xFFFFFFFFu;
    p = guest_ptr(m, addr, (uint32_t)nbytes);
    if (!p)
        printf("  [gl   ] %s: guest buffer %08x + %llu bytes is short or "
               "unmapped -- call dropped\n", who, (unsigned)addr,
               (unsigned long long)nbytes);
    return p;
}

/* GL_UNPACK_ALIGNMENT, tracked so the row stride matches what the driver will
 * actually read. 4 is the GL default. */
static uint32_t g_unpack = 4;

static uint32_t pixel_bytes(uint32_t format, uint32_t type) {
    switch (type) {
    case 0x8363:                        /* UNSIGNED_SHORT_5_6_5   */
    case 0x8033:                        /* UNSIGNED_SHORT_4_4_4_4 */
    case 0x8034:                        /* UNSIGNED_SHORT_5_5_5_1 */
        return 2;
    case 0x1401:                        /* UNSIGNED_BYTE */
        switch (format) {
        case 0x1906: return 1;          /* ALPHA           */
        case 0x1907: return 3;          /* RGB             */
        case 0x1908: return 4;          /* RGBA            */
        case 0x1909: return 1;          /* LUMINANCE       */
        case 0x190A: return 2;          /* LUMINANCE_ALPHA */
        default:     return 4;
        }
    default:
        return 4;                       /* conservative */
    }
}

static uint64_t image_bytes(uint32_t w, uint32_t hgt, uint32_t format,
                            uint32_t type) {
    uint64_t bpp, row, align, stride;
    if (!w || !hgt)
        return 0;
    bpp = pixel_bytes(format, type);
    row = (uint64_t)w * bpp;
    align = g_unpack ? g_unpack : 1;
    stride = ((row + align - 1) / align) * align;
    return stride * (hgt - 1) + row;    /* the last row carries no padding */
}

static void te_PixelStorei(GuestCpu *c, GuestMem *m, void *u) {
    (void)u;
    if ((uint32_t)ga(c, m, 0) == 0x0CF5u)          /* GL_UNPACK_ALIGNMENT */
        g_unpack = (uint32_t)ga(c, m, 1);
    glPixelStorei((GLenum)ga(c, m, 0), (GLint)ga(c, m, 1));
}

static void te_BindBuffer(GuestCpu *c, GuestMem *m, void *u) {
    uint32_t target = (uint32_t)ga(c, m, 0), name = (uint32_t)ga(c, m, 1);
    (void)u;
    if (target == 0x8892u)                          /* ARRAY_BUFFER */
        g_buf_array = name;
    else if (target == 0x8893u)                     /* ELEMENT_ARRAY_BUFFER */
        g_buf_elem = name;
    glBindBuffer((GLenum)target, (GLuint)name);
}

/* With a buffer bound the argument is an offset and must NOT be translated.
 * With none bound the extent is not knowable until the draw call, so this
 * still degrades to gp() there. */
static const void *array_arg(GuestMem *m, uint32_t v) {
    if (g_buf_array)
        return (const void *)(uintptr_t)v;
    return gp(m, v);
}

static void te_VertexPointer(GuestCpu *c, GuestMem *m, void *u) {
    (void)u;
    glVertexPointer((GLint)ga(c, m, 0), (GLenum)ga(c, m, 1),
                    (GLsizei)ga(c, m, 2), array_arg(m, ga(c, m, 3)));
}

static void te_ColorPointer(GuestCpu *c, GuestMem *m, void *u) {
    (void)u;
    glColorPointer((GLint)ga(c, m, 0), (GLenum)ga(c, m, 1),
                   (GLsizei)ga(c, m, 2), array_arg(m, ga(c, m, 3)));
}

static void te_TexCoordPointer(GuestCpu *c, GuestMem *m, void *u) {
    (void)u;
    glTexCoordPointer((GLint)ga(c, m, 0), (GLenum)ga(c, m, 1),
                      (GLsizei)ga(c, m, 2), array_arg(m, ga(c, m, 3)));
}

static void te_NormalPointer(GuestCpu *c, GuestMem *m, void *u) {
    (void)u;
    glNormalPointer((GLenum)ga(c, m, 0), (GLsizei)ga(c, m, 1),
                    array_arg(m, ga(c, m, 2)));
}

static void te_DrawElements(GuestCpu *c, GuestMem *m, void *u) {
    uint32_t count = (uint32_t)ga(c, m, 1), type = (uint32_t)ga(c, m, 2);
    uint32_t idx = (uint32_t)ga(c, m, 3);
    const void *p;
    (void)u;
    if (g_buf_elem) {
        p = (const void *)(uintptr_t)idx;           /* an offset, not a pointer */
    } else {
        uint32_t isz = (type == 0x1401u) ? 1u : 2u; /* UNSIGNED_BYTE else SHORT */
        p = gpn(m, idx, (uint64_t)count * isz, "glDrawElements");
        if (!p && idx)
            return;                                 /* short buffer: do not draw */
    }
    glDrawElements((GLenum)ga(c, m, 0), (GLsizei)count, (GLenum)type, p);
}

static void te_BufferData(GuestCpu *c, GuestMem *m, void *u) {
    uint32_t size = (uint32_t)ga(c, m, 1);
    const void *p = gpn(m, ga(c, m, 2), size, "glBufferData");
    (void)u;
    glBufferData((GLenum)ga(c, m, 0), (GLsizeiptr)size, p, (GLenum)ga(c, m, 3));
}

/* ------------------------------------------ texture upload diagnostics ----
 *
 * The menu renders its text but every textured quad is flat white, which is
 * what GL gives you for a texture that was never successfully defined. This
 * game uploads through BOTH glTexImage2D and glCompressedTexImage2D, and a
 * 2011 GLES1 Android title ships compressed art -- ETC1 (0x8D64) or PVRTC
 * (0x8C00..0x8C03). If mesa/nouveau does not expose that format the upload is
 * rejected and the texture samples white, while the uncompressed font atlas
 * keeps working. That is exactly the split on screen.
 *
 * These log rather than fix: nothing here calls glGetError, because the game
 * imports glGetError itself and reading it would clear the error before the
 * game saw it -- the same trap eglGetError set earlier. */
static const char *fmt_name(unsigned f) {
    switch (f) {
    case 0x8D64: return "ETC1_RGB8_OES";
    case 0x8C00: return "PVRTC_4BPPV1_RGB";
    case 0x8C01: return "PVRTC_2BPPV1_RGB";
    case 0x8C02: return "PVRTC_4BPPV1_RGBA";
    case 0x8C03: return "PVRTC_2BPPV1_RGBA";
    case 0x83F0: return "S3TC_DXT1_RGB";
    case 0x83F1: return "S3TC_DXT1_RGBA";
    case 0x8B90: return "PALETTE4_RGB8_OES";
    case 0x8B91: return "PALETTE4_RGBA8_OES";
    case 0x8B92: return "PALETTE4_R5_G6_B5_OES";
    case 0x8B93: return "PALETTE4_RGBA4_OES";
    case 0x8B94: return "PALETTE4_RGB5_A1_OES";
    case 0x8B95: return "PALETTE8_RGB8_OES";
    case 0x8B96: return "PALETTE8_RGBA8_OES";
    case 0x8B97: return "PALETTE8_R5_G6_B5_OES";
    case 0x8B98: return "PALETTE8_RGBA4_OES";
    case 0x8B99: return "PALETTE8_RGB5_A1_OES";
    default:     return "?";
    }
}

/* Nouveau advertises GL_OES_compressed_paletted_texture, but the title's
 * 0x8B99 uploads still produced undefined white textures. Paletted OES data is
 * simple enough to decode without depending on the driver: a 16/256-entry
 * palette followed by packed 4-bit or byte indices. Return an RGBA8 image for
 * the ordinary glTexImage2D path. */
static unsigned char *expand_palette(unsigned f, int width, int height,
                                     const unsigned char *src, uint32_t bytes) {
    uint32_t colors, entry_bytes, pixels, palette_bytes, index_bytes, i;
    unsigned char *dst;
    int kind;

    if (f < 0x8B90u || f > 0x8B99u || width <= 0 || height <= 0)
        return NULL;
    colors = f < 0x8B95u ? 16u : 256u;
    kind = (int)((f - 0x8B90u) % 5u);
    entry_bytes = kind == 0 ? 3u : kind == 1 ? 4u : 2u;
    if ((uint64_t)(uint32_t)width * (uint32_t)height > 0x3fffffffu)
        return NULL;
    pixels = (uint32_t)width * (uint32_t)height;
    palette_bytes = colors * entry_bytes;
    index_bytes = colors == 16u ? (pixels + 1u) / 2u : pixels;
    if (bytes < palette_bytes + index_bytes)
        return NULL;
    dst = (unsigned char *)malloc((size_t)pixels * 4u);
    if (!dst)
        return NULL;

    for (i = 0; i < pixels; i++) {
        uint32_t idx = colors == 16u
                     ? ((src[palette_bytes + i / 2u] >> ((i & 1u) ? 0 : 4)) & 15u)
                     : src[palette_bytes + i];
        const unsigned char *p = src + idx * entry_bytes;
        unsigned char *q = dst + i * 4u;
        if (kind == 0 || kind == 1) {          /* RGB8 / RGBA8 */
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2];
            q[3] = kind == 1 ? p[3] : 255u;
        } else {
            uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
            if (kind == 2) {                  /* R5_G6_B5 */
                uint32_t r = (v >> 11) & 31u, g = (v >> 5) & 63u, b = v & 31u;
                q[0] = (unsigned char)((r << 3) | (r >> 2));
                q[1] = (unsigned char)((g << 2) | (g >> 4));
                q[2] = (unsigned char)((b << 3) | (b >> 2)); q[3] = 255u;
            } else if (kind == 3) {           /* RGBA4 */
                q[0] = (unsigned char)(((v >> 12) & 15u) * 17u);
                q[1] = (unsigned char)(((v >> 8) & 15u) * 17u);
                q[2] = (unsigned char)(((v >> 4) & 15u) * 17u);
                q[3] = (unsigned char)((v & 15u) * 17u);
            } else {                          /* RGB5_A1 */
                uint32_t r = (v >> 11) & 31u, g = (v >> 6) & 31u;
                uint32_t b = (v >> 1) & 31u;
                q[0] = (unsigned char)((r << 3) | (r >> 2));
                q[1] = (unsigned char)((g << 3) | (g >> 2));
                q[2] = (unsigned char)((b << 3) | (b >> 2));
                q[3] = (v & 1u) ? 255u : 0u;
            }
        }
    }
    return dst;
}

/* Texture upload census. A first-N log cannot answer "does the game ever upload
 * real art" -- the first uploads are the engine's 2x2 default and a cap hides
 * everything after. This tracks distinct sizes and a running total instead, and
 * reports on a cadence so a long run stays readable. */
typedef struct { uint16_t w, h; uint32_t fmt; uint32_t n; } TexSize;
static TexSize g_tex_size[24];
static unsigned g_tex_kinds;
static uint32_t g_tex_plain, g_tex_comp, g_tex_big;

static void tex_note(uint32_t w, uint32_t h, uint32_t fmt, int compressed) {
    unsigned i;
    if (compressed)
        g_tex_comp++;
    else
        g_tex_plain++;
    if (w >= 64u && h >= 64u)
        g_tex_big++;
    for (i = 0; i < g_tex_kinds; i++)
        if (g_tex_size[i].w == (uint16_t)w && g_tex_size[i].h == (uint16_t)h &&
            g_tex_size[i].fmt == fmt) {
            g_tex_size[i].n++;
            return;
        }
    if (g_tex_kinds < 24u) {
        g_tex_size[g_tex_kinds].w = (uint16_t)w;
        g_tex_size[g_tex_kinds].h = (uint16_t)h;
        g_tex_size[g_tex_kinds].fmt = fmt;
        g_tex_size[g_tex_kinds].n = 1;
        g_tex_kinds++;
        printf("  [tex  ] NEW %ux%u %s 0x%04x  (totals: %u plain, %u compressed,"
               " %u >=64px)\n", (unsigned)w, (unsigned)h,
               compressed ? "compressed" : "plain", (unsigned)fmt,
               (unsigned)g_tex_plain, (unsigned)g_tex_comp,
               (unsigned)g_tex_big);
    }
}

static void te_CompressedTexImage2D(GuestCpu *c, GuestMem *m, void *u) {
    static unsigned seen[8];
    static int nseen, calls;
    unsigned f = (unsigned)ga(c, m, 2);
    int i;
    (void)u;
    for (i = 0; i < nseen; i++)
        if (seen[i] == f)
            break;
    if (i == nseen && nseen < 8) {
        seen[nseen++] = f;
        printf("  [tex  ] compressed format 0x%04x (%s) %dx%d level %d, "
               "%d bytes\n", f, fmt_name(f), (int)ga(c, m, 3), (int)ga(c, m, 4),
               (int)ga(c, m, 1), (int)ga(c, m, 6));
    }
    calls++;
    tex_note((uint32_t)ga(c, m, 3), (uint32_t)ga(c, m, 4), f, 1);
    {   /* imageSize is given explicitly for a compressed upload, so the extent
         * is exactly known and needs no format table. */
        uint32_t bytes = (uint32_t)ga(c, m, 6);
        const void *p = gpn(m, ga(c, m, 7), bytes, "glCompressedTexImage2D");
        if (!p && ga(c, m, 7))
            return;
        if (f >= 0x8B90u && f <= 0x8B99u && ga(c, m, 1) == 0 && p) {
            int width = (int)ga(c, m, 3), height = (int)ga(c, m, 4);
            unsigned char *rgba = expand_palette(f, width, height,
                                                 (const unsigned char *)p, bytes);
            if (rgba) {
                glTexImage2D((GLenum)ga(c, m, 0), 0, GL_RGBA, width, height,
                             (GLint)ga(c, m, 5), GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                free(rgba);
                if (calls <= 8)
                    printf("  [tex  ] expanded %s in software to RGBA8\n",
                           fmt_name(f));
                return;
            }
            printf("  [tex  ] palette expansion failed; forwarding to driver\n");
        }
        glCompressedTexImage2D((GLenum)ga(c, m, 0), (GLint)ga(c, m, 1),
                               (GLenum)f, (GLsizei)ga(c, m, 3),
                               (GLsizei)ga(c, m, 4), (GLint)ga(c, m, 5),
                               (GLsizei)bytes, p);
    }
}

static void te_TexImage2D(GuestCpu *c, GuestMem *m, void *u) {
    (void)u;
    if ((int)ga(c, m, 1) == 0)          /* census the base level only */
        tex_note((uint32_t)ga(c, m, 3), (uint32_t)ga(c, m, 4),
                 (uint32_t)ga(c, m, 2), 0);
    {
        uint64_t bytes = image_bytes((uint32_t)ga(c, m, 3), (uint32_t)ga(c, m, 4),
                                     (uint32_t)ga(c, m, 6), (uint32_t)ga(c, m, 7));
        const void *p = gpn(m, ga(c, m, 8), bytes, "glTexImage2D");
        if (!p && ga(c, m, 8))
            return;                     /* a NULL data pointer is legal -- it
                                         * defines the level without content */
        glTexImage2D((GLenum)ga(c, m, 0), (GLint)ga(c, m, 1), (GLint)ga(c, m, 2),
                     (GLsizei)ga(c, m, 3), (GLsizei)ga(c, m, 4),
                     (GLint)ga(c, m, 5), (GLenum)ga(c, m, 6),
                     (GLenum)ga(c, m, 7), p);
    }
}

/* Not previously overridden. allocate-with-NULL then fill via glTexSubImage2D
 * is an ordinary upload pattern, and if this game uses it then every real
 * texture went past the instrumentation unseen. It also needs the same extent
 * validation as glTexImage2D. */
static void te_TexSubImage2D(GuestCpu *c, GuestMem *m, void *u) {
    static uint32_t subs;
    uint32_t w = (uint32_t)ga(c, m, 4), h = (uint32_t)ga(c, m, 5);
    uint64_t bytes = image_bytes(w, h, (uint32_t)ga(c, m, 6),
                                 (uint32_t)ga(c, m, 7));
    const void *p = gpn(m, ga(c, m, 8), bytes, "glTexSubImage2D");
    (void)u;
    if (++subs <= 4u || (subs % 200u) == 0u)
        printf("  [tex  ] sub %ux%u at (%d,%d) fmt 0x%04x  (%u calls)\n",
               (unsigned)w, (unsigned)h, (int)ga(c, m, 2), (int)ga(c, m, 3),
               (unsigned)ga(c, m, 6), (unsigned)subs);
    if (!p && ga(c, m, 8))
        return;
    glTexSubImage2D((GLenum)ga(c, m, 0), (GLint)ga(c, m, 1), (GLint)ga(c, m, 2),
                    (GLint)ga(c, m, 3), (GLsizei)w, (GLsizei)h,
                    (GLenum)ga(c, m, 6), (GLenum)ga(c, m, 7), p);
}

/* ------------------------------------------------------------------ lookup */

static const struct { const char *name; GuestHleFn fn; } g_egl_overrides[] = {
    { "eglGetDisplay",           te_GetDisplay },
    { "eglInitialize",           te_Initialize },
    { "eglTerminate",            te_Terminate },
    { "eglGetError",             te_GetError },
    { "eglBindAPI",              te_BindAPI },
    { "eglQueryString",          te_QueryString },
    { "eglGetConfigs",           te_GetConfigs },
    { "eglGetConfigAttrib",      te_GetConfigAttrib },
    { "eglCreateContext",        te_CreateContext },
    { "eglDestroyContext",       te_DestroyContext },
    { "eglQueryContext",         te_QueryContext },
    { "eglGetCurrentContext",    te_GetCurrentContext },
    { "eglGetCurrentDisplay",    te_GetCurrentDisplay },
    { "eglGetCurrentSurface",    te_GetCurrentSurface },
    { "eglCreateWindowSurface",  te_CreateWindowSurface },
    { "eglCreatePbufferSurface", te_CreatePbufferSurface },
    { "eglDestroySurface",       te_DestroySurface },
    { "eglQuerySurface",         te_QuerySurface },
    { "eglMakeCurrent",          te_MakeCurrent },
    { "eglSwapBuffers",          te_SwapBuffers },
    { "eglBindTexImage",         te_BindTexImage },
    { "eglReleaseTexImage",      te_ReleaseTexImage },
    { "eglGetProcAddress",       te_GetProcAddress },
    /* Not EGL, but they need the same hand-written treatment: the generated
     * versions pass the game's 480x320 coordinates straight through. */
    { "glViewport",              te_Viewport },
    { "glScissor",               te_Scissor },
    { "glTexImage2D",            te_TexImage2D },
    { "glCompressedTexImage2D",  te_CompressedTexImage2D },
    { "glTexSubImage2D",         te_TexSubImage2D },
    { "glPixelStorei",           te_PixelStorei },
    { "glBindBuffer",            te_BindBuffer },
    { "glBufferData",            te_BufferData },
    { "glVertexPointer",         te_VertexPointer },
    { "glColorPointer",          te_ColorPointer },
    { "glTexCoordPointer",       te_TexCoordPointer },
    { "glNormalPointer",         te_NormalPointer },
    { "glDrawElements",          te_DrawElements },
    { NULL, NULL }
};

GuestHleFn egl_find_override(const char *name) {
    unsigned i;
    if (!name)
        return NULL;
    for (i = 0; g_egl_overrides[i].name; i++)
        if (strcmp(g_egl_overrides[i].name, name) == 0)
            return g_egl_overrides[i].fn;
    return NULL;
}

#endif /* __SWITCH__ */
