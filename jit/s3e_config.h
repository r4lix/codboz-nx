/* s3e_config.h -- the ICF answers behind s3eConfigGetInt/s3eConfigGetString.
 *
 * The .s3e carries its own ICF blob, but it is the *system* config: it holds
 * Windows and WP8 branches guarded by {COND} lines, and serving it wholesale
 * faults startup. So this answers "not set" for everything except the keys
 * below, and lets the game fall back to its own defaults everywhere else.
 *
 * The values come from the PortMaster ARMHF port
 * (github.com/Producdevity/cod-boz-port, MIT, src/s3e_config.c), which runs
 * the same 1.0.x image to a playable state, so they are validated end to end
 * rather than guessed. Two of them are load-bearing:
 *
 *   [GX] NumTPages          the stock 16/0 budget aborts with "Out of T-Pages"
 *   [RESMANAGER] ResBuildStyle
 *                           picks the texture build. The resource manager asks
 *                           for data-<style>/ paths and blackops_<style>.dz;
 *                           with the key unset it takes the software branch
 *                           (data-sw/), which is why only the engine's 2x2
 *                           default texture ever reached GL.
 *
 * ResBuildStyle is not in the table because it must name a pack that is
 * actually mounted -- s3e_vfs_build_style() derives it, and each harness feeds
 * it in through s3e_config_set_build_style() after s3e_vfs_init().
 *
 * loader/run_boz.py parses S3E_CONFIG_TABLE straight out of this header so the
 * Unicorn reference and the two C harnesses cannot drift apart and report
 * false divergences. Keep every entry on one line, in exactly the
 * X("section", "key", "value") form, and use block comments only -- a
 * line comment would swallow the backslash continuation.
 */
#ifndef S3E_CONFIG_H
#define S3E_CONFIG_H

/* Sections and keys are matched case-insensitively, as Marmalade's ICF
 * lookup is. Values are stored as text and parsed on demand, so one table
 * serves both s3eConfigGetInt and s3eConfigGetString.
 *
 * ENABLED: the six keys the hardware runs up to r17 were built on, plus the
 * dynamic ResBuildStyle. The four [GX] entries are not optional -- without
 * them the engine exhausts its stock 16 texture pages loading
 * bootstrap.group.bin. */
#define S3E_CONFIG_TABLE(X)                                                    \
    X("GX",        "NumTPages",                       "512")                   \
    X("GX",        "NumTPagesNoMipMap",               "256")                   \
    X("GX",        "NumTPageFreeRects",               "8192")                  \
    X("GX",        "MaxTexturesPerTPage",             "512")                   \
    X("GAME",      "ResourceDownloader",              "0")                     \
    X("GAME",      "LowMemoryDevice",                 "0")

/* PARKED: answered as "not set", so the game keeps its own defaults.
 *
 * r18 enabled all of these at once and corrupted a resource-manager vector
 * before the first present, ending in a realloc of 0x818b8500 bytes. That is
 * a real observation and the reason they are off -- but it was never
 * bisected, and only the two bucket sizes can plausibly move the heap enough
 * to cause it. The rest are off by association. To bisect, promote ONE entry
 * into S3E_CONFIG_TABLE above and rerun; start with FlashBucketSize, then
 * GuiBucketSize.
 *
 * Parked keys must stay in this list rather than being deleted or filtered
 * out inside s3e_config_get(): every consumer reads these two macros, so a
 * key that is enabled in one place and blocked in another makes the Unicorn
 * reference and the C harnesses answer the SDK differently, and the
 * differential reports that as a CPU divergence. */
#define S3E_CONFIG_PARKED(X)                                                   \
    X("GAME",      "EnableGC",                        "0")                     \
    X("GAME",      "EnableAndroidMarketBilling",      "0")                     \
    X("GAME",      "VoiceChatEnabled",                "0")                     \
    X("GAME",      "LowEndDevice",                    "0")                     \
    X("GAME",      "GuiBucketSize",                   "2500000")               \
    X("GAME",      "FrontendMemoryWarningLevel",      "0")                     \
    X("GAME",      "OnlineAccount",                   "NONE")                  \
    X("GAME",      "OnlineUseGameCenterMM",           "0")                     \
    X("GAME",      "MatchmakingSearchAndPublishMode", "0")                     \
    X("FLASH",     "FlashBucketSize",                 "32000000")              \
    X("FLASH",     "FlashBucketHeapAnalyse",          "0")                     \
    X("Demonware", "OnlineAccount",                   "NONE")                  \
    X("Demonware", "LSGServer",                       "")                      \
    X("Demonware", "AuthServer",                      "")                      \
    X("Demonware", "STUNServer",                      "")                      \
    X("ONLINE",    "dispatcher",                      "")

/* Load the game's own ICF -- the text blob at header +0x2c, 17 KB of it.
 *
 * This was previously answered "not set" wholesale, on the grounds that
 * serving it faulted startup. That was true but the cause was the {COND}
 * blocks: the file carries Windows, WP8, QNX and per-device Android sections
 * alongside the ones that apply, and serving all of them is what broke. With
 * conditions honoured, only {} and {OS=ANDROID} are taken.
 *
 * It matters because the file holds the engine's real sizes -- VertCacheSizeHW
 * 120000, DataCacheSizeHW 5000000, MaxTextureStages 32, NumMemBuckets 14 --
 * and without them the game falls back to compile-time defaults. A vertex
 * array sized by a too-small default, filled from an asset whose element count
 * comes from the data, is what overruns at RVA 0x2431ec.
 *
 * Returns the number of keys taken. Overrides in S3E_CONFIG_TABLE still win;
 * parked keys are left to whatever the ICF says, since the reason they are
 * parked is the PortMaster values, not the game's own. */
int s3e_config_load_icf(const char *text, unsigned len);

/* Extra keys from the card, applied after the game's ICF; later
 * definitions win, so these override it. Returns the total entry count. */
int s3e_config_load_overrides(const char *text, unsigned len);

/* The value for [section] key, or NULL when the key is not set -- which the
 * caller must answer with S3E_RESULT_ERROR (1), never with a zeroed buffer and
 * success. Parked keys return NULL unless the ICF supplies them. */
const char *s3e_config_get(const char *section, const char *key);

/* The value a parked key *would* have had, or NULL if it is not parked. Only
 * for logging: it tells you which unanswered key is a deliberate decision
 * rather than one nobody has looked at yet. */
const char *s3e_config_parked(const char *section, const char *key);

/* Names the mounted texture pack: "etc", "dxt", "atitc" or "gles1". Call once
 * at startup with s3e_vfs_build_style(); defaults to "gles1". */
void s3e_config_set_build_style(const char *style);
const char *s3e_config_build_style(void);

#endif /* S3E_CONFIG_H */
