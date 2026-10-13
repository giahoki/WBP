/* Experimental "no fog" switch (in-game menu, wbp.ini no_fog=1), applied while playing.
 *
 * The game's fog manager (debug menu "FOG MANAGER") holds the fog values in use; its editor
 * ("Fog - FogEdit", 0x26D7C90) binds "Fog-fogAlpha" to the float at +0x8C (the constructor at
 * 0x26D7AA1 sets it to 1.0). The manager is the pointer at +0x5888 of the object behind the
 * global at 0x5940DD8 (PS4 addresses, eboot base 0x400000). Each pad sample (once per game
 * frame) writes 0 there while the switch is on, and puts the game's value back when it is
 * turned off. Whether the renderer takes the fog from there is what this experiment checks. */
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EBOOT_BASE 0x400000u
#define GLOBAL_ADDRESS 0x5940DD8u /* pointer to the object holding the managers */
#define FOG_MANAGER_OFFSET 0x5888u
#define FOG_ALPHA_OFFSET 0x8Cu

static unsigned char *image_base;

void runtime_fog_install(unsigned char *image) { image_base = image; }

/* A guest pointer that is plausible: the game's heaps live in the 0x8_0000_0000..0x20_0000_0000
 * window of this port (image and direct/flexible memory). */
static int plausible(uintptr_t p) { return p >= 0x800000000ull && p < 0x2000000000ull && !(p & 7); }

void runtime_fog_tick(void) {
    static int was_on;
    static float saved_alpha = 1.0f;
    static int reported;
    if (!image_base) return;
    const int on = bbgpu_no_fog();
    if (!on && !was_on) return;
    uintptr_t global;
    memcpy(&global, image_base + (GLOBAL_ADDRESS - EBOOT_BASE), sizeof(global));
    if (!plausible(global)) return;
    uintptr_t manager;
    memcpy(&manager, (const void *)(global + FOG_MANAGER_OFFSET), sizeof(manager));
    if (!plausible(manager)) return;
    float *alpha = (float *)(manager + FOG_ALPHA_OFFSET);
    if (on) {
        if (!was_on && *alpha != 0.0f) saved_alpha = *alpha;
        *alpha = 0.0f;
        if (!reported) {
            reported = 1;
            printf("Fog: off (fog manager %p, alpha was %.2f)\n", (void *)manager, saved_alpha);
        }
    } else {
        *alpha = saved_alpha;
        printf("Fog: back on (alpha %.2f)\n", saved_alpha);
        reported = 0;
    }
    was_on = on;
}
