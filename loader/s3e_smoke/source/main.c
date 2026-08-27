/* s3e loader smoke test -- run the .s3e loader on real Switch hardware.
 *
 * Reads the DECOMPRESSED image from the SD card, loads it, and prints what the
 * loader found. Cross-check every number against the PC reference run
 * (loader/s3e_loader_check.py): they must match exactly.
 *
 * This does not execute any guest code -- the game is ARM32 and the Switch is
 * AArch64-only, so running it needs a JIT that does not exist yet. What this
 * does exercise is every line of Switch-native loader code, in particular the
 * unaligned 32-bit reads (the image sits at an odd file offset), a ~4.9 MB
 * allocation under Horizon, the typed block chain, 52k relocations, the PLT
 * scan and the tag-4 symbol naming.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "s3e_loader.h"

static const char *const kPaths[] = {
    "sdmc:/switch/boz/boz.s3e.unpacked",
    "sdmc:/switch/boz.s3e.unpacked",
    "sdmc:/boz.s3e.unpacked",
};

static unsigned char *slurp(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    rewind(f);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(buf);
        return NULL;
    }
    *out_size = got;
    return buf;
}

/* Bind nothing: returning 0 leaves the image's own PLT0 pointer in each slot.
 * We only want to prove the bind pass walks the table without faulting. */
static uint32_t resolve_none(const char *name, uint32_t import_index,
                             uint32_t got_index, void *user) {
    (void)name;
    (void)import_index;
    (void)got_index;
    (void)user;
    return 0;
}

static void run(void) {
    size_t size = 0;
    unsigned char *file = NULL;
    const char *used = NULL;

    for (size_t i = 0; i < sizeof kPaths / sizeof *kPaths; i++) {
        file = slurp(kPaths[i], &size);
        if (file) {
            used = kPaths[i];
            break;
        }
        printf("  not found: %s\n", kPaths[i]);
    }
    if (!file) {
        printf("\nNo image found. Copy boz.s3e.unpacked to one of the\n"
               "paths listed above, then run this again.\n");
        return;
    }
    printf("read %s\n     %lu bytes\n\n", used, (unsigned long)size);

    S3eImage img;
    if (s3e_load(file, size, 0, &img) != 0) {
        printf("s3e_load FAILED: %s\n", s3e_error());
        free(file);
        return;
    }

    printf("magic         : 0x%08x\n", (unsigned)img.hdr.magic);
    printf("K (image_off) : 0x%06x   expect 0x039483\n", (unsigned)img.hdr.image_off);
    printf("link base     : 0x%08x   expect 0x4a000000\n", (unsigned)img.hdr.link_base);
    printf("image size    : 0x%06x   expect 0x4a7dc8\n", (unsigned)img.image_size);
    printf("load base     : 0x%08x\n", (unsigned)img.load_base);
    printf("entry         : 0x%08x (ARM)\n", (unsigned)img.entry);
    printf("relocations   : %u        expect 52075\n", (unsigned)img.relocs_applied);
    printf("import names  : %u        expect 391\n", (unsigned)img.import_count);
    printf("GOT slots     : %u        expect 381\n", (unsigned)img.got_count);

    unsigned named = 0;
    for (uint32_t i = 0; i < img.got_count; i++)
        if (img.got_import[i] >= 0)
            named++;
    printf("named slots   : %u        expect 378\n\n", named);

    /* The entry stub must be the ARM prologue identified on the PC. This is
     * the real unaligned-access check: the words come out of the mapped image
     * that was memcpy'd from an odd file offset. */
    uint32_t w0 = 0, w1 = 0;
    memcpy(&w0, img.image + 0, 4);
    memcpy(&w1, img.image + 4, 4);
    printf("entry words   : %08x %08x\n", (unsigned)w0, (unsigned)w1);
    printf("                expect e59f008c e92d4010  [%s]\n\n",
           (w0 == 0xE59F008CU && w1 == 0xE92D4010U) ? "OK" : "MISMATCH");

    printf("first GOT slots:\n");
    for (uint32_t i = 0; i < img.got_count && i < 5; i++) {
        int32_t si = img.got_import[i];
        printf("  0x%06x = %s\n", (unsigned)img.got_rva[i],
               si >= 0 ? img.import_names[si] : "<unnamed>");
    }
    printf("  expect 0x411020 = eglQueryString\n\n");

    s3e_bind(&img, resolve_none, NULL);
    s3e_unload(&img);
    free(file);
    printf("bind + unload OK\n");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    printf("s3e loader smoke test\n\n");
    run();
    printf("\nPress + to exit.\n");

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
