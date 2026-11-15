/* Nintendo 3DS backend (libctru). */
#include "plat.h"

#include "../core/log.h"
#include "../game/prelude.h"
#include "script.h"

#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* libctru reads this for the main thread, which runs the game. The game
 * was written for a 2 MB machine with its stack at the top of RAM. */
u32 __stacksize__ = 0x80000;

/* Sound goes to the DSP in chunks; a few are queued ahead of playback. */
#define AUDIO_CHUNK 1024 /* sample pairs */
#define AUDIO_BUFFERS 6

#define AUDIO_CUSHION 2 /* chunks of silence played first whenever the queue has run dry */
/* Sound is made by the clock (plat_audio_due), so it plays at its own rate. */
#define AUDIO_RATE 44100.0f
/* After a longer stall (loading, the HOME menu) the missed sound is skipped. */
#define AUDIO_MAX_CATCH_UP_US 100000u

static ndspWaveBuf s_wave[AUDIO_BUFFERS], s_silence;
static int16_t *s_audio_memory;
static int s_audio_buffer, s_audio_fill;
static bool s_audio_ok;

static uint16_t s_top[TOP_W * TOP_H];
static uint16_t s_bot[BOT_W * BOT_H];
static bool s_new3ds;

#define CRASH_STACK_WORDS 96
#define TEXT_START 0x00100000u

extern char __exidx_start[]; /* just past the program code, from the linker script */

static uint8_t s_crash_handler_stack[0x2000] __attribute__((aligned(8)));

/* Set once plat_fatal is showing its message, possibly on the worker. */
static bool s_fatal;
static Thread s_fatal_thread;

static void stop_if_fatal(void)
{
    while (__atomic_load_n(&s_fatal, __ATOMIC_ACQUIRE) && threadGetCurrent() != s_fatal_thread) {
        svcSleepThread(1000000000LL);
    }
}

/* Null pointers.
 *
 * On the PlayStation address 0 is ordinary RAM, so the game reads (and now
 * and then writes) through null pointers without harm; here it faults. A
 * fault in the first 64 KiB from a plain ARM load or store is finished by
 * hand against the same bytes of the PlayStation RAM image, then the program
 * resumes. The places are listed in log.txt (see plat_frame_begin). */
#define NULL_WINDOW 0x10000u
#define NULL_PCS 16

static u32 s_null_pcs[NULL_PCS];
static int s_null_count, s_null_logged;

static u32 reg_value(const CpuRegisters *r, unsigned n)
{
    return n == 15 ? r->pc + 8 : n == 14 ? r->lr : n == 13 ? r->sp : r->r[n];
}

static void set_reg(CpuRegisters *r, unsigned n, u32 v)
{
    if (n == 13) r->sp = v;
    else if (n == 14) r->lr = v;
    else r->r[n] = v;
}

static uint8_t *null_target(u32 addr, u32 size)
{
    return addr < NULL_WINDOW && addr + size <= NULL_WINDOW ? psx_ram + addr : NULL;
}

/* Carries out the access of the ARM instruction at regs->pc against the
 * RAM image. Returns false for anything it does not handle. */
static bool emulate_null_access(CpuRegisters *r)
{
    u32 op = *(const u32 *)r->pc;
    unsigned rn = op >> 16 & 15, rd = op >> 12 & 15;
    bool p = op >> 24 & 1, u = op >> 23 & 1, w = op >> 21 & 1, l = op >> 20 & 1;
    u32 base, offset, addr, size, value = 0;
    bool sign = false, dual = false;
    uint8_t *m;

    /* A conditional instruction that faulted had its condition met. */
    if ((r->cpsr & 0x20) || op >> 28 == 0xF || rn == 15 || rd == 15) {
        return false; /* Thumb, unconditional space, or pc involved: not seen in game code */
    }
    base = reg_value(r, rn);
    if ((op >> 26 & 3) == 1) { /* LDR/STR/LDRB/STRB */
        size = op >> 22 & 1 ? 1 : 4;
        if (op >> 25 & 1) {
            unsigned shift = op >> 7 & 31, type = op >> 5 & 3;
            u32 rm = reg_value(r, op & 15);

            if (op >> 4 & 1) {
                return false;
            }
            if (type == 0) offset = rm << shift;
            else if (type == 1) offset = shift ? rm >> shift : 0;
            else if (type == 2) offset = (u32)((int32_t)rm >> (shift ? shift : 31));
            else if (shift) offset = rm >> shift | rm << (32 - shift);
            else return false; /* RRX */
        } else {
            offset = op & 0xFFF;
        }
    } else if ((op >> 25 & 7) == 0 && (op & 0x90) == 0x90 && (op >> 5 & 3)) { /* halfword, signed, dual */
        unsigned sh = op >> 5 & 3;

        offset = op >> 22 & 1 ? (op >> 4 & 0xF0) | (op & 15) : reg_value(r, op & 15);
        if (l) {
            size = sh == 2 ? 1 : 2;
            sign = sh != 1;
        } else if (sh == 1) {
            size = 2;
        } else {
            size = 4; /* LDRD (sh 2) / STRD (sh 3) */
            dual = true;
            l = sh == 2;
            if (rd & 1) return false;
        }
    } else {
        return false;
    }
    addr = p ? (u ? base + offset : base - offset) : base;
    if (!(m = null_target(addr, dual ? 8 : size))) {
        return false;
    }
    if (l) {
        if (size == 1) value = sign ? (u32)(int8_t)m[0] : m[0];
        else if (size == 2) value = m[0] | m[1] << 8, value = sign ? (u32)(int16_t)value : value;
        else memcpy(&value, m, 4);
        set_reg(r, rd, value);
        if (dual) {
            memcpy(&value, m + 4, 4);
            set_reg(r, rd + 1, value);
        }
    } else {
        value = reg_value(r, rd);
        if (size == 1) m[0] = (uint8_t)value;
        else if (size == 2) m[0] = (uint8_t)value, m[1] = (uint8_t)(value >> 8);
        else memcpy(m, &value, 4);
        if (dual) {
            value = reg_value(r, rd + 1);
            memcpy(m + 4, &value, 4);
        }
    }
    if ((!p || w) && !(l && (rn == rd || (dual && rn == rd + 1)))) {
        set_reg(r, rn, u ? base + offset : base - offset);
    }
    r->pc += 4;
    return true;
}

/* Restores every register from `r` and carries on there. The faulting
 * thread's stack just below its sp is free, so the pc goes there. */
__attribute__((naked, noreturn)) static void resume(__attribute__((unused)) CpuRegisters *r)
{
    __asm__ volatile("ldr r1, [r0, #52]\n\t" /* sp */
                     "ldr r2, [r0, #60]\n\t" /* pc */
                     "str r2, [r1, #-4]\n\t"
                     "ldr r2, [r0, #64]\n\t" /* cpsr */
                     "msr cpsr_fs, r2\n\t" /* flags and GE bits */
                     "ldr lr, [r0, #56]\n\t"
                     "mov sp, r1\n\t"
                     "ldm r0, {r0-r12}\n\t"
                     "ldr pc, [sp, #-4]\n\t");
}

/* A processor exception (bad pointer, undefined instruction): record what
 * tools/symbolize.py needs to name the place, then stop with a message. */
static void on_crash(ERRF_ExceptionInfo *excep, CpuRegisters *regs)
{
    static bool crashed;
    const u32 *sp = (const u32 *)regs->sp;
    char stack[400] = "";
    int i, len = 0;

    if (excep->type == ERRF_EXCEPTION_DATA_ABORT && excep->far < NULL_WINDOW && !crashed) {
        u32 pc = regs->pc;

        if (emulate_null_access(regs)) {
            int n = __atomic_load_n(&s_null_count, __ATOMIC_RELAXED);

            for (i = 0; i < n && s_null_pcs[i] != pc; i++) {
            }
            if (i == n && n < NULL_PCS) {
                s_null_pcs[n] = pc;
                __atomic_store_n(&s_null_count, n + 1, __ATOMIC_RELEASE);
            }
            resume(regs);
        }
    }
    if (crashed) {
        svcExitProcess();
    }
    crashed = true;
    log_line("crash: exception %d at pc %08lx lr %08lx, fault address %08lx, status %08lx", excep->type, regs->pc, regs->lr,
             excep->far, excep->fsr);
    log_line("  pc %08lx lr %08lx sp %08lx r0 %08lx r1 %08lx r2 %08lx r3 %08lx r12 %08lx", regs->pc, regs->lr, regs->sp,
             regs->r[0], regs->r[1], regs->r[2], regs->r[3], regs->r[12]);
    /* Return addresses still on the stack give the chain of callers. */
    for (i = 0; i < CRASH_STACK_WORDS && len < (int)sizeof stack - 10; i++) {
        if (sp[i] >= TEXT_START && sp[i] < (u32)__exidx_start) {
            len += snprintf(stack + len, sizeof stack - (size_t)len, " %08lx", sp[i]);
        }
    }
    log_line("  stack%s", stack);
    plat_fatal("The game crashed. Details are in log.txt in the lom3ds folder.");
}

/* Reads script.txt from the data directory, if there is one (see script.h). */
void plat_load_script(void)
{
    static char text[16384];
    char path[128];
    size_t n, i, kept = 0;
    FILE *f;

    snprintf(path, sizeof path, "%s/script.txt", plat_data_dir());
    f = fopen(path, "r");
    if (!f) {
        return;
    }
    n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    /* Line breaks and spaces are for the writer's convenience only. */
    for (i = 0; i < n; i++) {
        if (text[i] != '\n' && text[i] != '\r' && text[i] != ' ') {
            text[kept++] = text[i];
        }
    }
    text[kept] = 0;
    script_start(text);
    log_line("script.txt found: %u characters of scripted input", (unsigned)kept);
}

static void audio_init(void)
{
    float mix[12] = {1.0f, 1.0f};
    int i;

    if (R_FAILED(ndspInit())) {
        /* On a console this needs the DSP firmware dumped to the SD card. */
        log_line("no sound: the DSP could not be started (is sdmc:/3ds/dspfirm.cdc present?)");
        return;
    }
    s_audio_memory = linearAlloc((AUDIO_BUFFERS + AUDIO_CUSHION) * AUDIO_CHUNK * 2 * sizeof(int16_t));
    if (!s_audio_memory) {
        log_line("no sound: out of memory for sound buffers");
        ndspExit();
        return;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, AUDIO_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetMix(0, mix);
    for (i = 0; i < AUDIO_BUFFERS; i++) {
        s_wave[i].data_pcm16 = s_audio_memory + i * AUDIO_CHUNK * 2;
        s_wave[i].nsamples = AUDIO_CHUNK;
        s_wave[i].status = NDSP_WBUF_DONE;
    }
    s_silence.data_pcm16 = s_audio_memory + AUDIO_BUFFERS * AUDIO_CHUNK * 2;
    s_silence.nsamples = AUDIO_CUSHION * AUDIO_CHUNK;
    s_silence.status = NDSP_WBUF_DONE;
    memset(s_silence.data_pcm16, 0, AUDIO_CUSHION * AUDIO_CHUNK * 4);
    DSP_FlushDataCache(s_silence.data_pcm16, AUDIO_CUSHION * AUDIO_CHUNK * 4);
    s_audio_ok = true;
}

static bool wave_idle(const ndspWaveBuf *w)
{
    return w->status == NDSP_WBUF_DONE || w->status == NDSP_WBUF_FREE;
}

/* True when nothing is playing or waiting to: the next chunk would start at
 * once and run out before the one after it is ready. */
static bool audio_queue_empty(void)
{
    int i;

    for (i = 0; i < AUDIO_BUFFERS; i++) {
        if (!wave_idle(&s_wave[i])) {
            return false;
        }
    }
    return wave_idle(&s_silence);
}

int plat_audio_due(int nominal)
{
    static uint64_t last, carry; /* carry: microseconds times the rate, not yet made */
    uint64_t now = plat_time_us(), us = now - last;
    int n;

    if (!last || us > AUDIO_MAX_CATCH_UP_US) {
        last = now;
        return nominal;
    }
    last = now;
    carry += us * (uint64_t)AUDIO_RATE;
    n = (int)(carry / 1000000u);
    carry %= 1000000u;
    return n;
}

void plat_audio_submit(const int16_t *samples, int frames)
{
    if (!s_audio_ok) {
        return;
    }
    while (frames > 0) {
        ndspWaveBuf *w = &s_wave[s_audio_buffer];
        int n = AUDIO_CHUNK - s_audio_fill;

        /* Every buffer still waiting to play: the game is ahead of the
         * speaker, so this sound is dropped. */
        if (!wave_idle(w)) {
            return;
        }
        if (n > frames) {
            n = frames;
        }
        memcpy(w->data_pcm16 + s_audio_fill * 2, samples, (size_t)n * 4);
        s_audio_fill += n;
        samples += n * 2;
        frames -= n;
        if (s_audio_fill == AUDIO_CHUNK) {
            DSP_FlushDataCache(w->data_pcm16, AUDIO_CHUNK * 4);
            if (audio_queue_empty()) {
                ndspChnWaveBufAdd(0, &s_silence);
            }
            ndspChnWaveBufAdd(0, w);
            s_audio_buffer = (s_audio_buffer + 1) % AUDIO_BUFFERS;
            s_audio_fill = 0;
        }
    }
}

#define SOC_BUFFER_SIZE 0x100000

static char s_program_path[256] = "sdmc:/3ds/lom3ds.3dsx";

const char *plat_program_path(void)
{
    return s_program_path;
}

bool plat_is_installed(void)
{
    return !envIsHomebrew();
}

bool plat_install_package(const char *path, char *err, size_t err_size)
{
    static u8 buffer[1 << 16];
    FILE *f = fopen(path, "rb");
    Handle cia;
    Result rc;
    u32 offset = 0, written;
    size_t n;

    if (!f) {
        snprintf(err, err_size, "could not open the downloaded package");
        return false;
    }
    rc = amInit();
    if (R_SUCCEEDED(rc)) {
        rc = AM_StartCiaInstall(MEDIATYPE_SD, &cia);
        if (R_SUCCEEDED(rc)) {
            while (R_SUCCEEDED(rc) && (n = fread(buffer, 1, sizeof buffer, f)) > 0) {
                rc = FSFILE_Write(cia, &written, offset, buffer, (u32)n, 0);
                offset += (u32)n;
            }
            if (R_SUCCEEDED(rc)) {
                rc = AM_FinishCiaInstall(cia);
            } else {
                AM_CancelCIAInstall(cia);
            }
        }
        amExit();
    }
    fclose(f);
    if (R_FAILED(rc)) {
        snprintf(err, err_size, "the system refused the package (error %08lX)", (unsigned long)rc);
        return false;
    }
    return true;
}

bool plat_net_start(char *err, size_t err_size)
{
    static bool started;
    void *buffer;

    if (started) {
        return true;
    }
    buffer = memalign(0x1000, SOC_BUFFER_SIZE);
    if (!buffer || R_FAILED(socInit(buffer, SOC_BUFFER_SIZE))) {
        free(buffer);
        snprintf(err, err_size, "the network could not be started (is wireless on?)");
        return false;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    started = true;
    return true;
}

/* The certificate bundle linked into the program (see the Makefile). */
extern const unsigned char ca_bundle[], ca_bundle_end[];

const void *plat_ca_bundle(size_t *size)
{
    *size = (size_t)(ca_bundle_end - ca_bundle);
    return ca_bundle;
}

void plat_init(int argc, char **argv)
{
    /* The Homebrew Launcher passes the path the program was loaded from. */
    if (argc > 0 && argv[0] && !strncmp(argv[0], "sdmc:/", 6) && strlen(argv[0]) < sizeof s_program_path) {
        strcpy(s_program_path, argv[0]);
    }
    gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    audio_init();
    threadOnException(on_crash, s_crash_handler_stack + sizeof s_crash_handler_stack, WRITE_DATA_TO_HANDLER_STACK);
    APT_CheckNew3DS(&s_new3ds);
    hidSetRepeatParameters(20, 5);
    mkdir("sdmc:/3ds", 0777);
    mkdir(plat_data_dir(), 0777);
}

/* Buttons keep their libctru bit; the circle pad is folded into the d-pad. */
static uint32_t fold_keys(uint32_t k)
{
    uint32_t out = k & (PK_A | PK_B | PK_SELECT | PK_START | PK_R | PK_L | PK_X | PK_Y | PK_ZL | PK_ZR);

    if (k & KEY_RIGHT) out |= PK_RIGHT;
    if (k & KEY_LEFT) out |= PK_LEFT;
    if (k & KEY_UP) out |= PK_UP;
    if (k & KEY_DOWN) out |= PK_DOWN;
    return out;
}

void plat_exit(void)
{
    if (s_audio_ok) {
        ndspExit();
    }
    gfxExit();
}

bool plat_frame_begin(PlatInput *in)
{
    touchPosition tp;
    uint32_t held, down, rep;

    stop_if_fatal();
    while (s_null_logged < __atomic_load_n(&s_null_count, __ATOMIC_ACQUIRE)) {
        log_line("read through a null pointer at pc %08lx, given PlayStation RAM as the console would",
                 s_null_pcs[s_null_logged++]);
    }
    if (!aptMainLoop()) {
        return false;
    }
    hidScanInput();
    held = hidKeysHeld();
    down = hidKeysDown();
    rep = hidKeysDownRepeat();
    hidTouchRead(&tp);

    in->held = fold_keys(held);
    in->down = fold_keys(down);
    in->repeat = fold_keys(rep);
    in->touching = (held & KEY_TOUCH) != 0;
    in->touch_down = (down & KEY_TOUCH) != 0;
    in->touch_x = tp.px;
    in->touch_y = tp.py;

    /* A script, if one is running, plays alongside the real controls. */
    if (script_active()) {
        PlatInput scripted;
        bool shot, vram;

        if (!script_step(&scripted, &shot, &vram)) {
            log_line("script finished");
            return false;
        }
        in->held |= scripted.held;
        in->down |= scripted.down;
        in->repeat |= scripted.repeat;
        if (scripted.touch_down) {
            in->touching = in->touch_down = true;
            in->touch_x = scripted.touch_x;
            in->touch_y = scripted.touch_y;
        }
    }
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

/* The LCD framebuffers are stored rotated: 240 pixels per column, columns
 * left to right, each column running bottom to top. */
static void blit_rotated(gfxScreen_t screen, const uint16_t *src, int w)
{
    u16 fw, fh;
    uint16_t *dst = (uint16_t *)gfxGetFramebuffer(screen, GFX_LEFT, &fw, &fh);
    int x, y;

    for (x = 0; x < w; x++) {
        uint16_t *col = dst + x * 240 + 239;
        const uint16_t *s = src + x;
        for (y = 0; y < 240; y++) {
            *col-- = *s;
            s += w;
        }
    }
}

/* The upper screen is double-buffered and swapped only when it has a new
 * picture, so an unchanged frame costs nothing and never shows a stale
 * buffer. The lower screen changes rarely and has a single buffer. */
static void show(gfxScreen_t screen, const uint16_t *src, int w)
{
    u16 fw, fh;
    u8 *fb = gfxGetFramebuffer(screen, GFX_LEFT, &fw, &fh);

    blit_rotated(screen, src, w);
    GSPGPU_FlushDataCache(fb, (u32)fw * fh * 2);
    gfxScreenSwapBuffers(screen, false);
}

uint32_t plat_present(bool top_changed, bool bottom_changed)
{
    uint64_t start = plat_time_us();
    uint32_t spent;

    stop_if_fatal();
    if (top_changed) {
        show(GFX_TOP, s_top, TOP_W);
    }
    if (bottom_changed) {
        show(GFX_BOTTOM, s_bot, BOT_W);
    }
    spent = (uint32_t)(plat_time_us() - start);
    gspWaitForVBlank();
    return spent;
}

#define WORKER_STACK 0x4000
#define WORKER_PRIORITY 0x18 /* above the main thread's 0x30: sound must not starve */

static LightEvent s_worker_event;
static void (*s_worker_fn)(void);

static uint8_t s_worker_crash_stack[0x2000] __attribute__((aligned(8)));

static void worker_entry(void *arg)
{
    (void)arg;
    /* Exception handlers are per thread: without this a fault in the worker
     * would go unrecorded and the game would simply stop. */
    threadOnException(on_crash, s_worker_crash_stack + sizeof s_worker_crash_stack, WRITE_DATA_TO_HANDLER_STACK);
    s_worker_fn();
}

/* A New 3DS has two cores applications may use freely besides the first;
 * the original 3DS has none, so there the caller keeps the work. */
bool plat_worker_start(void (*fn)(void))
{
    if (!s_new3ds) {
        return false;
    }
    s_worker_fn = fn;
    LightEvent_Init(&s_worker_event, RESET_ONESHOT);
    return threadCreate(worker_entry, NULL, WORKER_STACK, WORKER_PRIORITY, 2, true) != NULL;
}

void plat_worker_sleep(void)
{
    LightEvent_WaitTimeout(&s_worker_event, 1000000000LL);
}

void plat_worker_wake(void)
{
    LightEvent_Signal(&s_worker_event);
}

void plat_sleep_us(uint32_t us)
{
    svcSleepThread((s64)us * 1000);
}

uint64_t plat_time_us(void)
{
    /* Whole seconds and the rest apart: the plain product overflows 64 bits
     * after some hours. */
    uint64_t tick = svcGetSystemTick();

    return tick / SYSCLOCK_ARM11 * 1000000ULL + tick % SYSCLOCK_ARM11 * 1000000ULL / SYSCLOCK_ARM11;
}

void plat_fatal(const char *message)
{
    /* The other thread, if this is not the one the game runs on, stops at
     * its next frame (see plat_frame_begin and plat_present). */
    s_fatal_thread = threadGetCurrent();
    __atomic_store_n(&s_fatal, true, __ATOMIC_RELEASE);
    consoleInit(GFX_BOTTOM, NULL);
    printf("\n Legend of Mana 3DS stopped:\n\n %s\n\n Press START to quit.\n", message);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        /* Only the lower screen: the upper one keeps its last picture. */
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
        gspWaitForVBlank();
    }
    gfxExit();
    exit(1);
}

const char *plat_data_dir(void)
{
    return "sdmc:/3ds/lom3ds";
}

const char *plat_vram_dump_path(void)
{
    return NULL;
}

bool plat_is_new3ds(void)
{
    return s_new3ds;
}

void plat_set_speedup(bool on)
{
    if (s_new3ds) {
        osSetSpeedupEnable(on);
    }
}
