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
    /* Point the denormalised copy at something real from the moment a region
     * exists. It is otherwise written only by the slow paths, and the inline
     * guest_ptr never reaches them while it keeps hitting region[cache] -- so
     * a run where every access happened to land in region 0 would leave these
     * at zero. The interpreter would not care, since it reads region[cache]
     * itself. JIT code used to read these and now uses the slot tables, but
     * they are still initialised here: a null host and a zero size are a trap
     * worth not leaving armed for whatever reads them next. */
    if (m->count == 1) {
        m->cache_base = base;
        m->cache_size = size;
        m->cache_host = host;
        m->cache_writable = (uint32_t)writable;
    }
    /* Point the write entry at the first writable region there is, for the
     * same reason. */
    if (writable && !m->wcache_host) {
        m->wcache = m->count - 1;
        m->wcache_base = base;
        m->wcache_size = size;
        m->wcache_host = host;
    }
    m->undo_log = guest_undo_log;
    guest_slots_build(m);
    return 0;
}

GuestSlot guest_rslot[GUEST_SLOT_COUNT];
GuestSlot guest_wslot[GUEST_SLOT_COUNT];

/* Rebuild both slot tables from scratch. Called on every guest_mem_add, which
 * happens four times at startup and never again, so simplicity beats being
 * incremental.
 *
 * A region claims a slot only when it covers that slot's first byte. The
 * alternative -- claiming any slot the region touches -- would let a region
 * starting mid-slot be selected for an address BELOW its base, and `addr -
 * base` would then wrap to a huge offset. That rejects correctly for almost
 * every such address, but for the three bytes immediately below base it wraps
 * back through zero into a small offset that passes the bounds test, and the
 * access would read from the wrong place. Leaving that leading partial slot
 * empty costs nothing here (all four regions are 16 MB aligned) and keeps the
 * emitted test to a single unsigned compare. */
void guest_slots_build(GuestMem *m) {
    uint32_t s;
    int i;
    memset(guest_rslot, 0, sizeof guest_rslot);
    memset(guest_wslot, 0, sizeof guest_wslot);
    for (s = 0; s < GUEST_SLOT_COUNT; s++) {
        uint64_t slot_start = (uint64_t)s << GUEST_SLOT_SHIFT;
        for (i = 0; i < m->count; i++) {
            const GuestRegion *r = &m->region[i];
            uint64_t end = (uint64_t)r->base + r->size;
            if ((uint64_t)r->base > slot_start || end <= slot_start)
                continue;       /* does not cover this slot's first byte */
            guest_rslot[s].base = r->base;
            guest_rslot[s].size = r->size;
            guest_rslot[s].host = r->host;
            if (r->writable) {
                guest_wslot[s].base = r->base;
                guest_wslot[s].size = r->size;
                guest_wslot[s].host = r->host;
            }
            break;              /* regions never overlap */
        }
    }
    m->rslot = guest_rslot;
    m->wslot = guest_wslot;
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
/* Set the data cache index and the denormalised copy together, so JIT code
 * reading the copy can never see a region the interpreter is not using. */
static void set_data_cache(GuestMem *m, int i) {
    m->cache = i;
    m->cache_base = m->region[i].base;
    m->cache_size = m->region[i].size;
    m->cache_host = m->region[i].host;
    m->cache_writable = (uint32_t)m->region[i].writable;
}

void *guest_ptr_slow(const GuestMem *m, uint32_t addr, uint32_t len) {
    int i = find_index(m, addr, len);
    if (i < 0)
        return NULL;
    set_data_cache((GuestMem *)m, i);
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

/* The write cache, kept apart from the read one; see GuestMem::wcache. */
static void set_write_cache(GuestMem *m, int i) {
    m->wcache = i;
    m->wcache_base = m->region[i].base;
    m->wcache_size = m->region[i].size;
    m->wcache_host = m->region[i].host;
}

void *guest_wptr_slow(GuestMem *m, uint32_t addr, uint32_t len) {
    int i = find_index(m, addr, len);
    if (i < 0 || !m->region[i].writable)
        return NULL;
    set_write_cache(m, i);
    return m->region[i].host + (addr - m->region[i].base);
}

GuestUndo guest_undo_log[GUEST_UNDO_MAX];

/* Capture the bytes a store is about to overwrite. Reads through the region
 * table directly rather than guest_ptr, because guest_ptr's cache is about to
 * be updated by the store itself and borrowing it here would be one more thing
 * to reason about on a path that only exists to keep a check honest. */
void guest_undo_record(GuestMem *m, uint32_t addr, uint32_t size) {
    int i;
    GuestUndo *u;
    if (m->undo_n >= GUEST_UNDO_MAX) {
        m->undo_overflow++;     /* the block is too long to check; see caller */
        return;
    }
    i = find_index(m, addr, size);
    if (i < 0)
        return;                 /* unmapped: the store will fault anyway */
    u = &guest_undo_log[m->undo_n++];
    u->addr = addr;
    u->size = size;
    u->old = 0;
    memcpy(&u->old, m->region[i].host + (addr - m->region[i].base), size);
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
