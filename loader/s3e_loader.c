/* s3e_loader.c -- loader for Marmalade "XE3U" .s3e images */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "s3e_loader.h"

static char g_err[256];

const char *s3e_error(void) { return g_err; }

static int fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof g_err, fmt, ap);
  va_end(ap);
  return -1;
}

/* The image is stored at an odd file offset, so every word access is
 * potentially unaligned. */
static uint32_t rd32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

static uint16_t rd16(const uint8_t *p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}

static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* ARM data-processing immediate: 8-bit value rotated right by 2*rot. */
static uint32_t rotimm(uint32_t x) {
  uint32_t v = x & 0xFF, rot = ((x >> 8) & 0xF) * 2;
  return rot ? ((v >> rot) | (v << (32 - rot))) : v;
}

static void read_header(S3eHeader *h, const uint8_t *f) {
  uint32_t *dst = (uint32_t *)h;
  for (int i = 0; i < 19; i++)
    dst[i] = rd32(f + 4 * i);
}

int s3e_load(const uint8_t *file, size_t size, uint32_t load_base,
             S3eImage *out) {
  memset(out, 0, sizeof *out);
  g_err[0] = 0;

  if (size < 0x4c)
    return fail("file too small (%zu bytes)", size);

  S3eHeader *h = &out->hdr;
  read_header(h, file);

  if (h->magic != S3E_MAGIC)
    return fail("bad magic 0x%08x (want 'XE3U')", h->magic);
  if (h->image_size == 0 || h->data_end_rva > h->image_size)
    return fail("nonsense sizes: image=%u data_end=%u", h->image_size,
                h->data_end_rva);
  /* The identity that pins K. */
  if (h->data_end_file != h->data_end_rva + h->image_off)
    return fail("layout check failed: +0x20=0x%x != +0x18+0x14=0x%x",
                h->data_end_file, h->data_end_rva + h->image_off);
  if ((size_t)h->image_off + h->data_end_rva > size)
    return fail("image runs past EOF");

  if (load_base == 0)
    load_base = h->link_base;

  out->image_alloc = h->image_size + S3E_IMAGE_SLACK;
  out->image = calloc(1, out->image_alloc);
  if (!out->image)
    return fail("out of memory allocating %u bytes", out->image_alloc);
  out->image_size = h->image_size;
  out->load_base = load_base;
  out->entry = load_base; /* entry is RVA 0, ARM mode */

  memcpy(out->image, file + h->image_off, h->data_end_rva); /* BSS stays zero */

  /* ---- import names --------------------------------------------------- */
  if ((size_t)h->import_off + 10 > size) {
    s3e_unload(out);
    return fail("import table past EOF");
  }
  uint32_t count = rd16(file + h->import_off + 8);
  if (count == 0 || count > 8192) {
    s3e_unload(out);
    return fail("implausible import count %u", count);
  }
  out->import_count = count;
  out->import_names = calloc(count, sizeof(char *));
  if (!out->import_names) {
    s3e_unload(out);
    return fail("out of memory for %u imports", count);
  }

  size_t o = (size_t)h->import_off + 10;
  for (uint32_t i = 0; i < count; i++) {
    size_t start = o;
    while (o < size && file[o])
      o++;
    if (o >= size) {
      s3e_unload(out);
      return fail("import name %u runs past EOF", i);
    }
    out->import_names[i] = malloc(o - start + 1);
    if (!out->import_names[i]) {
      s3e_unload(out);
      return fail("out of memory for import name %u", i);
    }
    memcpy(out->import_names[i], file + start, o - start);
    out->import_names[i][o - start] = 0;
    o++;
  }

  /* ---- typed block chain ---------------------------------------------- */
  /* {u32 tag, u32 bytes, u32 count} + payload, running from just past the
   * names up to the image. Located by the identity bytes == 12 + 4*count on
   * the first (relocation) block, which skips the few padding bytes. */
  size_t blk = 0;
  for (size_t p = o; p + 12 <= (size_t)h->image_off && p < o + 16; p++) {
    if (rd32(file + p + 4) == 12 + 4 * rd32(file + p + 8)) {
      blk = p;
      break;
    }
  }
  if (!blk) {
    s3e_unload(out);
    return fail("could not locate the block chain after the import names");
  }

  size_t reloc = 0, syms = 0;
  uint32_t nreloc = 0, nsyms = 0;
  for (size_t p = blk; p + 12 <= (size_t)h->image_off;) {
    uint32_t tag = rd32(file + p), nbytes = rd32(file + p + 4);
    uint32_t cnt = rd32(file + p + 8);
    if (nbytes < 12 || p + nbytes > (size_t)h->image_off)
      break;
    if (tag == S3E_BLOCK_RELOC) {
      reloc = p + 12;
      nreloc = cnt;
    } else if (tag == S3E_BLOCK_SYMS) {
      syms = p + 12;
      nsyms = cnt;
    }
    p += nbytes;
  }
  if (!reloc) {
    s3e_unload(out);
    return fail("no relocation block (tag %u) in the chain", S3E_BLOCK_RELOC);
  }

  /* ---- relocations ----------------------------------------------------- */
  /* Each entry is an RVA whose word holds link_base + target; rebase it. */
  for (uint32_t i = 0; i < nreloc; i++) {
    uint32_t t = rd32(file + reloc + 4 * i);
    if (t + 4 > h->data_end_rva)
      continue;
    uint32_t w = rd32(out->image + t);
    if (w < h->link_base || w >= h->link_base + h->image_size)
      continue;
    wr32(out->image + t, w - h->link_base + load_base);
    out->relocs_applied++;
  }

  /* ---- PLT stubs -> GOT slots ------------------------------------------ */
  /* add ip,pc,#A / add ip,ip,#B / ldr pc,[ip,#C]!  =>  slot = stub+8+A+B+C */
  uint32_t cap = 512, n = 0;
  uint32_t *slots = malloc(cap * sizeof *slots);
  if (!slots) {
    s3e_unload(out);
    return fail("out of memory scanning the PLT");
  }
  for (uint32_t r = 0; r + 12 <= h->data_end_rva; r += 4) {
    uint32_t a = rd32(out->image + r);
    if ((a & 0xFFFFF000) != 0xE28FC000)
      continue;
    uint32_t b = rd32(out->image + r + 4), c = rd32(out->image + r + 8);
    if ((b & 0xFFFFF000) != 0xE28CC000 || (c & 0xFFFFF000) != 0xE5BCF000)
      continue;
    if (n == cap) {
      uint32_t *t = realloc(slots, (cap *= 2) * sizeof *slots);
      if (!t) {
        free(slots);
        s3e_unload(out);
        return fail("out of memory growing the GOT list");
      }
      slots = t;
    }
    slots[n++] = (r + 8) + rotimm(a) + rotimm(b) + (c & 0xFFF);
  }
  if (n == 0) {
    free(slots);
    s3e_unload(out);
    return fail("found no PLT stubs");
  }

  /* sort ascending and drop duplicates */
  for (uint32_t i = 1; i < n; i++) {
    uint32_t v = slots[i], j = i;
    while (j && slots[j - 1] > v) {
      slots[j] = slots[j - 1];
      j--;
    }
    slots[j] = v;
  }
  uint32_t uniq = 0;
  for (uint32_t i = 0; i < n; i++)
    if (i == 0 || slots[i] != slots[i - 1])
      slots[uniq++] = slots[i];

  out->got_count = uniq;
  out->got_rva = slots;
  out->got_import = malloc(uniq * sizeof *out->got_import);
  if (!out->got_import) {
    s3e_unload(out);
    return fail("out of memory for the GOT map");
  }
  for (uint32_t i = 0; i < uniq; i++)
    out->got_import[i] = -1;

  /* ---- name the slots from the tag-4 block ----------------------------- */
  /* 6-byte records {u16 0x41, u16 A, u16 import}; A is a 16-bit offset within
   * the GOT's own 64 KB page. */
  if (syms) {
    uint32_t page = slots[0] & ~0xFFFFu;
    for (uint32_t i = 0; i < nsyms; i++) {
      uint32_t a = rd16(file + syms + 2 + 6 * i);
      uint32_t s = rd16(file + syms + 4 + 6 * i);
      if (s >= count)
        continue;
      uint32_t want = page + a;
      /* binary search the slot list */
      uint32_t lo = 0, hi = uniq;
      while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (slots[mid] < want)
          lo = mid + 1;
        else
          hi = mid;
      }
      if (lo < uniq && slots[lo] == want)
        out->got_import[lo] = (int32_t)s;
    }
  }

  return 0;
}

int s3e_bind(S3eImage *img, S3eResolve resolve, void *user) {
  if (!img->image || !resolve)
    return fail("s3e_bind called on an unloaded image");
  img->imports_bound = 0;
  for (uint32_t i = 0; i < img->got_count; i++) {
    int32_t si = img->got_import[i];
    const char *name = si >= 0 ? img->import_names[si] : NULL;
    uint32_t addr = resolve(name, si >= 0 ? (uint32_t)si : 0xFFFFFFFFu, i, user);
    if (!addr)
      continue; /* leave the image's PLT0 pointer in place */
    wr32(img->image + img->got_rva[i], addr);
    img->imports_bound++;
  }
  return 0;
}

void s3e_unload(S3eImage *img) {
  if (img->import_names) {
    for (uint32_t i = 0; i < img->import_count; i++)
      free(img->import_names[i]);
    free(img->import_names);
  }
  free(img->got_rva);
  free(img->got_import);
  free(img->image);
  memset(img, 0, sizeof *img);
}
