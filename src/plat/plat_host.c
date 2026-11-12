/* Headless Linux backend for tests and screenshots.
 *
 *   lom3ds-host [--data DIR] [--out DIR] [--script STEPS] [--wav FILE]
 *
 * --wav records the sound the game produces.
 *
 * STEPS is a script as described in script.h.
 * The program quits when the script runs out. */
#include "plat.h"

#include "script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint16_t s_top[TOP_W * TOP_H];
static uint16_t s_bot[BOT_W * BOT_H];
static const char *s_data = "sdmc/3ds/lom3ds";
static const char *s_out = ".";
static FILE *s_wav;
static uint32_t s_wav_bytes;
static bool s_want_shot;
static bool s_want_vram;
static int s_vram_count;
static int s_shot_count;

void plat_init(int argc, char **argv)
{
    int i;

    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--data")) {
            s_data = argv[i + 1];
        } else if (!strcmp(argv[i], "--out")) {
            s_out = argv[i + 1];
        } else if (!strcmp(argv[i], "--script")) {
            script_start(argv[i + 1]);
        } else if (!strcmp(argv[i], "--wav")) {
            s_wav = fopen(argv[i + 1], "wb");
            if (!s_wav) {
                perror(argv[i + 1]);
                exit(1);
            }
            fseek(s_wav, 44, SEEK_SET); /* the header is written last */
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            exit(2);
        }
    }
}

/* Updating is tried out on the host by naming a file to replace. */
const char *plat_program_path(void)
{
    return getenv("LOM3DS_UPDATE_TARGET");
}

bool plat_net_start(char *err, size_t err_size)
{
    (void)err;
    (void)err_size;
    return true;
}

const void *plat_ca_bundle(size_t *size)
{
    *size = 0;
    return NULL;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void plat_exit(void)
{
    if (s_wav) {
        /* 16-bit stereo PCM at 44100 Hz. */
        uint8_t h[44] = "RIFF....WAVEfmt ";

        put32(h + 4, 36 + s_wav_bytes);
        put32(h + 16, 16);
        h[20] = 1;
        h[22] = 2;
        put32(h + 24, 44100);
        put32(h + 28, 44100 * 4);
        h[32] = 4;
        h[34] = 16;
        memcpy(h + 36, "data", 4);
        put32(h + 40, s_wav_bytes);
        fseek(s_wav, 0, SEEK_SET);
        fwrite(h, 1, sizeof h, s_wav);
        fclose(s_wav);
        s_wav = NULL;
    }
}

void plat_audio_submit(const int16_t *samples, int frames)
{
    if (s_wav) {
        s_wav_bytes += (uint32_t)fwrite(samples, 1, (size_t)frames * 4, s_wav);
    }
}

void plat_load_script(void)
{
    /* The host build takes its script from the command line. */
}

bool plat_frame_begin(PlatInput *in)
{
    bool shot, vram;

    if (!script_step(in, &shot, &vram)) {
        return false;
    }
    s_want_shot |= shot;
    s_want_vram |= vram;
    return true;
}

uint16_t *plat_fb_top(void)
{
    return s_top;
}

uint16_t *plat_fb_bottom(void)
{
    return s_bot;
}

static void write_ppm(const char *name, const uint16_t *px, int w, int h)
{
    char path[512];
    FILE *f;
    int i;

    snprintf(path, sizeof path, "%s/shot%02d_%s.ppm", s_out, s_shot_count, name);
    f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        uint16_t c = px[i];
        uint8_t rgb[3];
        rgb[0] = (uint8_t)((c >> 11) * 255 / 31);
        rgb[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
        rgb[2] = (uint8_t)((c & 31) * 255 / 31);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

uint32_t plat_present(bool top_changed, bool bottom_changed)
{
    (void)top_changed;
    (void)bottom_changed;
    if (s_want_shot) {
        write_ppm("top", s_top, TOP_W, TOP_H);
        write_ppm("bot", s_bot, BOT_W, BOT_H);
        s_shot_count++;
        s_want_shot = false;
    }
    return 0;
}

/* The host build stays single-threaded so that runs are repeatable. */
bool plat_worker_start(void (*fn)(void))
{
    (void)fn;
    return false;
}

void plat_worker_sleep(void)
{
}

void plat_worker_wake(void)
{
}

void plat_sleep_us(uint32_t us)
{
    (void)us;
}

uint64_t plat_time_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

void plat_fatal(const char *message)
{
    (void)message; /* already in the log, which the host build echoes */
    abort();
}

const char *plat_data_dir(void)
{
    return s_data;
}

const char *plat_vram_dump_path(void)
{
    static char path[512];

    if (!s_want_vram) {
        return NULL;
    }
    s_want_vram = false;
    snprintf(path, sizeof path, "%s/vram%02d.ppm", s_out, s_vram_count++);
    return path;
}

/* LOM3DS_OLD3DS=1 makes the host build stand in for the original 3DS. */
bool plat_is_new3ds(void)
{
    return getenv("LOM3DS_OLD3DS") == NULL;
}

void plat_set_speedup(bool on)
{
    (void)on;
}
