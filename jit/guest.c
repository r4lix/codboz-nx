/* guest.c -- guest memory regions and status reporting. */

#include <string.h>

#include "guest.h"

int guest_mem_add(GuestMem *m, uint32_t base, uint32_t size, uint8_t *host,
                  int writable) {
    if (m->count >= GUEST_MAX_REGIONS)
        return -1;
    GuestRegion *r = &m->region[m->count++];
    r->base = base;
    r->size = size;
    r->host = host;
    r->writable = writable;
    return 0;
}

void *guest_ptr(const GuestMem *m, uint32_t addr, uint32_t len) {
    for (int i = 0; i < m->count; i++) {
        const GuestRegion *r = &m->region[i];
        uint32_t off = addr - r->base;
        if (addr >= r->base && off < r->size) {
            /* reject spans that run off the end rather than silently clamping */
            if ((uint64_t)off + len > (uint64_t)r->size)
                return NULL;
            return r->host + off;
        }
    }
    return NULL;
}

static const GuestRegion *find(const GuestMem *m, uint32_t addr) {
    for (int i = 0; i < m->count; i++) {
        const GuestRegion *r = &m->region[i];
        if (addr >= r->base && addr - r->base < r->size)
            return r;
    }
    return NULL;
}

/* Guest loads must never become host pointer casts: the image is mapped from an
 * odd file offset, so every access is potentially unaligned. memcpy keeps that
 * correct and lets the compiler pick the right instruction. */
#define LOAD(bits, type)                                                       \
    int guest_ld##bits(const GuestMem *m, uint32_t addr, uint32_t *out) {      \
        const void *p = guest_ptr(m, addr, sizeof(type));                      \
        type v;                                                                \
        if (!p)                                                                \
            return 0;                                                          \
        memcpy(&v, p, sizeof v);                                               \
        *out = (uint32_t)v;                                                    \
        return 1;                                                              \
    }

LOAD(32, uint32_t)
LOAD(16, uint16_t)
LOAD(8, uint8_t)

#define STORE(bits, type)                                                      \
    int guest_st##bits(GuestMem *m, uint32_t addr, uint32_t v) {               \
        const GuestRegion *r = find(m, addr);                                  \
        type t = (type)v;                                                      \
        uint32_t off;                                                          \
        void *p;                                                               \
        if (!r || !r->writable)                                                \
            return 0;                                                          \
        off = addr - r->base;                                                  \
        if (sizeof(type) > r->size - off)                                      \
            return 0;                                                          \
        p = r->host + off;                                                     \
        memcpy(p, &t, sizeof t);                                               \
        return 1;                                                              \
    }

STORE(32, uint32_t)
STORE(16, uint16_t)
STORE(8, uint8_t)

const char *guest_status_str(GuestStatus s) {
    switch (s) {
    case GUEST_OK:           return "ok";
    case GUEST_HALTED:       return "halted";
    case GUEST_FAULT_MEM:    return "unmapped memory access";
    case GUEST_FAULT_UNDEF:  return "undefined/unimplemented instruction";
    case GUEST_STEP_LIMIT:   return "step limit";
    }
    return "?";
}
