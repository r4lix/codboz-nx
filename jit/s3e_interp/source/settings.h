/* settings.h -- sdmc:/switch/boz/config.txt as a small key=value store.
 *
 * One file holds every user-facing setting: the Play Online keys net.c reads
 * at boot (multiplayer_server, player_name) and the controls/display keys the
 * in-game menu changes. Lines the store does not understand -- comments,
 * blank lines, keys from other versions -- are kept exactly where they are, so
 * a hand-edited file survives the menu saving over it. */
#ifndef SETTINGS_H
#define SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_PATH "sdmc:/switch/boz/config.txt"

/* Read the file (missing is fine: every key then takes its default). */
void settings_load(void);

/* The value for key, or fallback when the key is absent. */
const char *settings_get(const char *key, const char *fallback);
int         settings_get_int(const char *key, int fallback);

/* Change or add a key in memory; settings_save writes the file. */
void settings_set(const char *key, const char *value);
void settings_set_int(const char *key, int value);

/* Non-zero on success. Only writes when something changed. */
int settings_save(void);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_H */
