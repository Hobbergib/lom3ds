/* Platform layer: the only interface the rest of the port uses to reach the
 * console. Two backends implement it: plat_3ds.c (libctru) and plat_host.c
 * (headless Linux build used for tests and screenshots). */
#ifndef PLAT_H
#define PLAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TOP_W 400
#define TOP_H 240
#define BOT_W 320
#define BOT_H 240

/* Key bits. The values match libctru's KEY_* so the 3DS backend is a mask. */
enum {
    PK_A = 1u << 0,
    PK_B = 1u << 1,
    PK_SELECT = 1u << 2,
    PK_START = 1u << 3,
    PK_RIGHT = 1u << 4,
    PK_LEFT = 1u << 5,
    PK_UP = 1u << 6,
    PK_DOWN = 1u << 7,
    PK_R = 1u << 8,
    PK_L = 1u << 9,
    PK_X = 1u << 10,
    PK_Y = 1u << 11,
    PK_ZL = 1u << 14,
    PK_ZR = 1u << 15
};

typedef struct {
    uint32_t held;   /* keys currently down */
    uint32_t down;   /* keys pressed this frame */
    uint32_t repeat; /* pressed this frame, or auto-repeating while held */
    bool touching;   /* stylus on the lower screen */
    bool touch_down; /* stylus landed this frame */
    int touch_x, touch_y;
} PlatInput;

void plat_init(int argc, char **argv);
void plat_exit(void);

/* Starts a scripted run if the data directory holds one (see script.h).
 * Call once the log is open. */
void plat_load_script(void);

/* Polls input. Returns false when the application should quit. */
bool plat_frame_begin(PlatInput *in);

/* Row-major RGB565 drawing surfaces, TOP_W x TOP_H and BOT_W x BOT_H. */
uint16_t *plat_fb_top(void);
uint16_t *plat_fb_bottom(void);

/* Waits for the next vertical blank, first showing each surface that was
 * redrawn since the last call; copying one to its screen is costly, so say
 * honestly. Returns the microseconds spent copying, not counting the wait. */
uint32_t plat_present(bool top_changed, bool bottom_changed);

/* Directory holding the disc image, config and saves (no trailing slash). */
const char *plat_data_dir(void);

/* Queues stereo 16-bit sound, `frames` sample pairs at 44100 Hz. */
void plat_audio_submit(const int16_t *samples, int frames);

/* A worker on another processor core, for work that need not hold up the
 * game (sound). plat_worker_start returns false if the platform has no core
 * to spare; the caller then does the work itself. The worker function
 * never returns: it calls plat_worker_sleep when idle and is woken by
 * plat_worker_wake. */
bool plat_worker_start(void (*fn)(void));
void plat_worker_sleep(void); /* returns when woken, or after about a second */
void plat_worker_wake(void);
void plat_sleep_us(uint32_t us);

/* Microseconds from some fixed moment; for measuring how long work takes. */
uint64_t plat_time_us(void);

/* Shows an error the program cannot recover from, then quits. */
void plat_fatal(const char *message) __attribute__((noreturn));

/* Debugging: a file to write a dump of the game's video memory to, when one
 * was asked for this frame; otherwise NULL (always NULL on the console). */
const char *plat_vram_dump_path(void);

/* --- updating (src/net/update.c) ---------------------------------------- */

/* The file this program was started from, which an update replaces, or NULL
 * if it is not known or not replaceable. */
const char *plat_program_path(void);
/* Brings up networking; safe to repeat. On failure writes the reason. */
bool plat_net_start(char *err, size_t err_size);
/* Root certificates (PEM) to verify servers with, or NULL to use the
 * system's own. */
const void *plat_ca_bundle(size_t *size);

bool plat_is_new3ds(void);
/* New 3DS only: 804 MHz clock and L2 cache. No effect elsewhere. */
void plat_set_speedup(bool on);

#endif
