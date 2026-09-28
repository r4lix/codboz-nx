/* s3e_files.h -- the file layer behind the s3eFile* imports.
 *
 * Ported from run_boz.py's VFS so the C harness resolves exactly the same
 * bytes the Unicorn reference does. Every file the game can open is a byte
 * range of a real file on disk, so this needs no DTRZ parser: it reads the
 * flat index produced by loader/mkfileidx.py.
 *
 * Handles are guest-visible pointers (0xF1000000 + 4*slot), matching the
 * reference harness so traces line up.
 */
#ifndef S3E_FILES_H
#define S3E_FILES_H

#include <stdint.h>

#define S3E_FILE_HANDLE_BASE 0xF1000000u

/* Marmalade error codes the game actually checks. */
#define S3E_FILE_ERR_NONE      0
#define S3E_FILE_ERR_NOT_FOUND 3

/* `root` holds boz_files.idx and the archives it names, plus the save
 * sandbox. Returns the number of index entries, or -1 if the index is
 * missing (the layer then still serves loose files from root). */
int s3e_vfs_init(const char *root);

/* Which texture build the mounted archives actually hold: "etc", "dxt",
 * "atitc" or "gles1". Feeds [RESMANAGER] ResBuildStyle, which the resource
 * manager turns into data-<style>/ paths and a blackops_<style>.dz -- so it
 * has to name a pack that is really there, never a hardcoded preference.
 * Valid after s3e_vfs_init(); "gles1" when no pack was mounted. */
const char *s3e_vfs_build_style(void);

uint32_t s3e_vfs_open(const char *name, const char *mode);
uint32_t s3e_vfs_read(uint32_t h, void *dst, uint32_t n);
uint32_t s3e_vfs_write(uint32_t h, const void *src, uint32_t n);
int      s3e_vfs_seek(uint32_t h, int32_t off, uint32_t origin);
uint32_t s3e_vfs_tell(uint32_t h);
uint32_t s3e_vfs_size(uint32_t h);
void     s3e_vfs_close(uint32_t h);
int      s3e_vfs_flush(uint32_t h);   /* s3eFileFlush: 0 on success */
int      s3e_vfs_exists(const char *name);
int      s3e_vfs_error(void);
int      s3e_vfs_delete(const char *name);
int      s3e_vfs_mkdir(const char *name);

/* Save files are written to the card from a background thread; this waits
 * until every queued write has landed. Call before the process exits. */
void     s3e_vfs_sync(void);

#endif /* S3E_FILES_H */
