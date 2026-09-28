/* menu.h -- the in-game settings menu (hold "-" for 2 seconds).
 *
 * Drawn with Dear ImGui straight into the game's frame just before it is
 * presented, in the style of Fizeau's overlay. While it is open the game
 * receives no buttons, sticks or touches; they drive the menu instead. */
#ifndef MENU_H
#define MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Once per frame from the key update, with the raw pad state. Handles the
 * long press that opens and closes the menu and feeds navigation. */
void menu_input(uint64_t held, int lstick_x, int lstick_y);

/* Non-zero while the menu is open, and for the frames after it closes until
 * every button is released (so the button that closed it is not also given
 * to the game). */
int menu_blocks_input(void);
int menu_is_open(void);

/* Open (1) or close (0) directly -- the control socket's SND MENU, for
 * testing without holding a button. */
void menu_set_open(int open);

/* From the eglSwapBuffers thunk, on the GL thread, with the surface size. */
void menu_render(int surface_w, int surface_h);

/* Implemented by the port (main.c): the live settings the menu edits.
 * Keys are the config.txt names. */
int  port_setting_get(const char *key);
void port_setting_set(const char *key, int value);
const char *port_build_label(void);

/* The Advanced tab edits settings that only take effect at launch, so it
 * shows what this run is actually using rather than what is in the file:
 * a comma-separated list like "dynarmic 32 MB, profilers". */
const char *port_runtime_label(void);

/* Non-zero when the previous launch died before it finished starting, and
 * every advanced setting was therefore ignored for this run. */
int  port_safe_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* MENU_H */
