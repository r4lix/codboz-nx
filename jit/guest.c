/* guest.c -- guest memory regions and status reporting.
 *
 * The accessors themselves are inline in guest.h; what is left here is the
 * cache-miss path. See the comment on guest_ptr() for why.
 */

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
    /* Point the write entry at the first writable region there is, so the
     * inline fast path has something valid to test before the first miss. */
    if (writable && !m->region[m->wcache].writable)
        m->wcache = m->count - 1;
    return 0;
}

/* Regions never overlap, so the first containing region is the only one. A
 * span that starts inside a region but runs off its end is rejected rather
 * than silently clamped, and the scan simply finds nothing else. */
static int find_index(const GuestMem *m, uint32_t addr, uint32_t len) {
    for (int i = 0; i < m->count; i++) {
        const GuestRegion *r = &m->region[i];
        uint32_t off = addr - r->base;
        if (off < r->size && len <= r->size - off)
            return i;
    }
    return -1;
}

/* The const cast is deliberate: `cache` is advisory metadata, not part of the
 * mapping the caller is observing, and every read of it is bounds-checked at
 * the use site. Keeping the loads const lets the interpreter pass a const
 * GuestMem around without giving up the cache. */
void *guest_ptr_slow(const GuestMem *m, uint32_t addr, uint32_t len) {
    int i = find_index(m, addr, len);
    if (i < 0)
        return NULL;
    ((GuestMem *)m)->cache = i;
    return m->region[i].host + (addr - m->region[i].base);
}

/* Refreshes the fetch cache rather than the data one, so an instruction fetch
 * can never evict the region a load is about to use, or vice versa. */
void *guest_ifetch_slow(const GuestMem *m, uint32_t addr, uint32_t len) {
    int i = find_index(m, addr, len);
    if (i < 0)
        return NULL;
    ((GuestMem *)m)->icache = i;
    return m->region[i].host + (addr - m->region[i].base);
}

void *guest_wptr_slow(GuestMem *m, uint32_t addr, uint32_t len) {
    int i = find_index(m, addr, len);
    if (i < 0 || !m->region[i].writable)
        return NULL;
    m->wcache = i;   /* kept apart from the read cache; see GuestMem::wcache */
    return m->region[i].host + (addr - m->region[i].base);
}

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
