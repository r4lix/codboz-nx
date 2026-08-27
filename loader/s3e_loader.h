/* s3e_loader.h -- loader for Marmalade "XE3U" .s3e images
 *
 * Format constants were recovered from the CoD:BOZ payload and verified by
 * executing the result under an ARM emulator; see REVERSING.md.
 *
 * The loader produces a flat guest image plus a list of GOT slots to fill, and
 * leaves execution to the caller. The guest is ARMv7-A and the entry stub is
 * ARM, not Thumb, so any CPU backend needs interworking.
 */
#ifndef S3E_LOADER_H
#define S3E_LOADER_H

#include <stddef.h>
#include <stdint.h>

#define S3E_MAGIC 0x55334558u /* 'XE3U' */
#define S3E_IMAGE_SLACK 0x10000u

/* Typed blocks follow the import names, each {u32 tag, u32 bytes, u32 count}. */
#define S3E_BLOCK_RELOC 1u
#define S3E_BLOCK_SYMS  4u

/* 19 little-endian words at offset 0. The header mixes FILE offsets with RVAs;
 * the comments say which. unknownNN are unclassified and unused. */
typedef struct {
  uint32_t magic;            /* +0x00  'XE3U'                                */
  uint32_t unknown04;        /* +0x04  0x42800, lands mid-function           */
  uint32_t version;          /* +0x08                                        */
  uint32_t import_off;       /* +0x0c  FILE offset of the import table       */
  uint32_t unknown10;        /* +0x10  points INTO the relocation list, not  */
                             /*        at its start -- do not use it         */
  uint32_t image_off;        /* +0x14  FILE offset of RVA 0 (the constant K) */
  uint32_t data_end_rva;     /* +0x18  end of initialised data, as an RVA    */
  uint32_t image_size;       /* +0x1c  total bytes to allocate (incl. BSS)   */
  uint32_t data_end_file;    /* +0x20  == data_end_rva + image_off           */
  uint32_t unknown24;
  uint32_t unknown28;
  uint32_t config_off;       /* +0x2c  FILE offset of the ICF text blob      */
  uint32_t config_len;       /* +0x30                                        */
  uint32_t link_base;        /* +0x34  0x4a000000 -- the image's INTENDED    */
                             /*        load address, not a placeholder       */
  uint32_t unknown38;
  uint32_t unknown3c;
  uint32_t unknown40;
  uint32_t default_impl_rva; /* +0x44  391 Thumb pointers, one default        */
                             /*        implementation per import             */
  uint32_t unknown48;
} S3eHeader;

typedef struct {
  uint8_t  *image;         /* image_size bytes; RVA r lives at image[r]      */
  uint32_t  image_size;    /* logical size from the header      */
  /* The image is allocated with slack past image_size: the guest memset
   * clears buffers ending exactly at the image end and overshoots by a
   * few bytes. A real loader maps page-aligned so that never faults on
   * hardware; an exact-size allocation does. Map image_alloc. */
  uint32_t  image_alloc;
  uint32_t  load_base;     /* guest address of RVA 0                         */
  uint32_t  entry;         /* guest address of the entry stub (ARM mode)     */

  uint32_t  import_count;  /* names in the import table                      */
  char    **import_names;

  /* Imports are reached through an ARM ELF PLT. These are the GOT slots the
   * stubs load from; filling them is the loader's import contract. */
  uint32_t  got_count;
  uint32_t *got_rva;       /* RVA of each slot, ascending                    */
  int32_t  *got_import;    /* index into import_names, or -1 if unnamed      */

  uint32_t  relocs_applied;
  uint32_t  imports_bound;

  S3eHeader hdr;
} S3eImage;

/* Return the guest address to store in this GOT slot, or 0 to leave the
 * image's own PLT0 pointer in place. */
typedef uint32_t (*S3eResolve)(const char *name, uint32_t import_index,
                              uint32_t got_index, void *user);

/* `file` must be the DECOMPRESSED image. A .s3e on disk is a raw LZMA-alone
 * stream (13-byte header: props 0x5d, 4-byte dict size, 8-byte output size).
 *
 * Pass load_base = 0 to use the image's link base, which is what it expects;
 * loading anywhere else currently fails because some pointers reach RVAs
 * through absolute link-base values that no relocation entry covers. */
int  s3e_load(const uint8_t *file, size_t size, uint32_t load_base, S3eImage *out);
int  s3e_bind(S3eImage *img, S3eResolve resolve, void *user);
void s3e_unload(S3eImage *img);

const char *s3e_error(void);

#endif /* S3E_LOADER_H */
