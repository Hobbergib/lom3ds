/* Kernel services: vertical blank, events, root counters, and the port's
 * model of interrupts.
 *
 * On the console, disc and timer handlers run as interrupts. Here they run
 * at "poll points": SDK calls the game makes while it waits (VSync above
 * all). Nothing is preempted, so handlers stay atomic as the game expects. */
#include "psx.h"

#include "../core/log.h"
#include "../plat/plat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define EVENT_COUNT 32
#define EVENT_HANDLE_BASE 0xF1000000u
#define EV_MODE_INTERRUPT 0x1000
#define EV_SPEC_INTERRUPT 0x0002
#define DESC_ROOT_COUNTER 0xF2000000u
#define RCNT_MODE_INTERRUPT 0x1000
#define RCNT_MODE_SYSTEM_CLOCK 0x0001
#define SYSTEM_CLOCK_HZ 33868800
#define FRAMES_PER_SECOND 60
#define CLOCK_CYCLES_PER_SAMPLE (SYSTEM_CLOCK_HZ / SAMPLE_RATE)
#define HBLANKS_PER_FRAME 263
/* VSync(-1) calls in a row, with no real wait between them, that count as
 * one frame: keeps time moving if the game spins on the frame counter. */
#define IDLE_POLLS_PER_FRAME 4096
/* Poll points per frame while the game sleeps in VSync: enough for the 2.5
 * sectors a frame of a real-time stream, plus command completions. */
#define POLLS_PER_FRAME 4

typedef struct {
    uint32_t desc;
    int32_t spec;
    int32_t mode;
    void (*func)(void);
    bool open, enabled, pending;
} Event;

typedef struct {
    uint32_t target;
    int32_t mode;
    bool running;
    uint32_t carry; /* clock ticks left over from the last frame */
    uint32_t value; /* what GetRCnt reports */
} RootCounter;

bool psx_in_interrupt;
const char *volatile psx_stage = "starting";

static Event s_events[EVENT_COUNT];
static RootCounter s_counters[4];
static void (*s_vsync_callback)(void);
static int s_vsync_count;
static int s_last_wait_count;
static int s_idle_polls;

void psx_fatal(const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    log_line("[frame %d] fatal: %s", s_vsync_count, msg);
    plat_fatal(msg);
}

void psx_log(const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    log_line("[frame %d] %s", s_vsync_count, msg);
}

int psx_unimplemented(const char *name)
{
    psx_log("SDK function not implemented: %s", name);
    return 0;
}

void psx_watchdog(void)
{
    static int last_count = -1, stalled_checks;
    static uint64_t last_change;
    uint64_t now = plat_time_us();

    if (s_vsync_count != last_count) {
        last_count = s_vsync_count;
        last_change = now;
        stalled_checks = 0;
    } else if (now - last_change > 3000000u * (uint64_t)(stalled_checks + 1) && stalled_checks < 3) {
        stalled_checks++;
        psx_log("watchdog: no frame finished for %u s; the game was last seen in: %s",
                (unsigned)((now - last_change) / 1000000u), psx_stage);
    }
}

void psx_poll(void)
{
    if (psx_in_interrupt) {
        return;
    }
    psx_in_interrupt = true;
    psx_cd_poll();
    psx_gpu_poll();
    psx_in_interrupt = false;
}

/* --- events ------------------------------------------------------------- */

static Event *event_from_handle(long handle)
{
    uint32_t i = (uint32_t)handle - EVENT_HANDLE_BASE;

    return i < EVENT_COUNT && s_events[i].open ? &s_events[i] : NULL;
}

long psx_OpenEvent(unsigned long desc, long spec, long mode, void (*func)(void))
{
    int i;

    for (i = 0; i < EVENT_COUNT; i++) {
        if (!s_events[i].open) {
            s_events[i] = (Event){(uint32_t)desc, spec, mode, func, true, false, false};
            return (long)(EVENT_HANDLE_BASE + (uint32_t)i);
        }
    }
    return -1;
}

long psx_CloseEvent(long handle)
{
    Event *e = event_from_handle(handle);

    if (e) {
        e->open = false;
    }
    return e != NULL;
}

long psx_EnableEvent(long handle)
{
    Event *e = event_from_handle(handle);

    if (e) {
        e->enabled = true;
    }
    return e != NULL;
}

long psx_DisableEvent(long handle)
{
    Event *e = event_from_handle(handle);

    if (e) {
        e->enabled = false;
    }
    return e != NULL;
}

long psx_TestEvent(long handle)
{
    Event *e = event_from_handle(handle);

    psx_poll();
    psx_card_poll();
    if (e && e->pending) {
        e->pending = false;
        return 1;
    }
    return 0;
}

/* Marks matching events as occurred, or calls their handler for events
 * opened in interrupt mode. */
void psx_deliver_event(uint32_t desc, int32_t spec)
{
    int i;

    for (i = 0; i < EVENT_COUNT; i++) {
        Event *e = &s_events[i];

        if (!e->open || !e->enabled || e->desc != desc || e->spec != spec) {
            continue;
        }
        if (e->mode == EV_MODE_INTERRUPT) {
            if (e->func) {
                e->func();
            }
        } else {
            e->pending = true;
        }
    }
}

void psx_UnDeliverEvent(unsigned long desc, unsigned long spec)
{
    int i;

    for (i = 0; i < EVENT_COUNT; i++) {
        if (s_events[i].open && s_events[i].desc == (uint32_t)desc && s_events[i].spec == (int32_t)spec) {
            s_events[i].pending = false;
        }
    }
}

int psx_EnterCriticalSection(void)
{
    return 1;
}

void psx_ExitCriticalSection(void)
{
}

/* --- root counters ------------------------------------------------------ */

long psx_SetRCnt(unsigned long counter, unsigned short target, long mode)
{
    RootCounter *c = &s_counters[counter & 3];

    c->target = target ? target : 0x10000;
    c->mode = mode;
    c->running = false;
    c->carry = 0;
    return 1;
}

long psx_StartRCnt(unsigned long counter)
{
    s_counters[counter & 3].running = true;
    return 1;
}

long psx_StopRCnt(unsigned long counter)
{
    s_counters[counter & 3].running = false;
    return 1;
}

long psx_GetRCnt(unsigned long counter)
{
    RootCounter *c = &s_counters[counter & 3];

    /* Callers only want a value that keeps changing. A counter that was
     * never set up counts through its full 16 bits. */
    c->value = (c->value + 97) % (c->target ? c->target : 0x10000);
    return (long)c->value;
}

/* One frame of sound, with the interrupts counter 2 would have produced
 * during it. That counter is the only one the game runs with interrupts: it
 * is the tick of the sound driver, which programs the sound chip. The sound
 * between two ticks is produced before the next tick changes anything, so
 * notes start and stop where the driver put them. */
static void run_sound_for_frame(void)
{
    static uint32_t until_tick;   /* system clock cycles before the next tick */
    static uint32_t sample_carry; /* cycles not yet turned into samples */
    RootCounter *c = &s_counters[2];
    uint32_t cycles = (uint32_t)(SAMPLE_RATE / FRAMES_PER_SECOND) * CLOCK_CYCLES_PER_SAMPLE;
    int left = SAMPLE_RATE / FRAMES_PER_SECOND;

    if (c->running && (c->mode & RCNT_MODE_INTERRUPT)) {
        uint32_t period = (c->target ? c->target : 0x10000u) * ((c->mode & RCNT_MODE_SYSTEM_CLOCK) ? 1u : 8u);

        if (until_tick == 0 || until_tick > period) {
            until_tick = period;
        }
        while (until_tick <= cycles) {
            int n;

            cycles -= until_tick;
            sample_carry += until_tick;
            n = (int)(sample_carry / CLOCK_CYCLES_PER_SAMPLE);
            sample_carry %= CLOCK_CYCLES_PER_SAMPLE;
            if (n > left) {
                n = left;
            }
            if (n > 0) {
                psx_spu_mix(n);
                left -= n;
            }
            psx_deliver_event(DESC_ROOT_COUNTER | 2, EV_SPEC_INTERRUPT);
            until_tick = period;
        }
        until_tick -= cycles;
        sample_carry += cycles;
        sample_carry %= CLOCK_CYCLES_PER_SAMPLE;
    } else {
        until_tick = 0;
    }
    /* The rest of the frame, so every frame is the same length of sound. */
    psx_spu_mix(left);
}

/* --- vertical blank ----------------------------------------------------- */

/* Never called from interrupt context: a frame runs handlers of its own. */
static void run_frame(void)
{
    uint16_t buttons;
    int i;

    psx_stage = "showing a frame";
    buttons = port_frame();
    psx_stage = "frame handlers (pad, disc, sound)";

    s_vsync_count++;
    s_idle_polls = 0;
    psx_in_interrupt = true;
    psx_pad_update(buttons);
    psx_cd_frame();
    psx_card_poll();
    run_sound_for_frame();
    if (s_vsync_callback) {
        s_vsync_callback();
    }
    psx_in_interrupt = false;
    for (i = 0; i < POLLS_PER_FRAME; i++) {
        psx_poll();
    }
    psx_stage = "game code after a frame";
}

int psx_frame_count(void)
{
    return s_vsync_count;
}

int psx_VSync(int mode)
{
    int target, start = s_vsync_count;

    if (mode < 0) {
        psx_stage = "game code polling the frame counter";
        psx_poll();
        if (!psx_in_interrupt && ++s_idle_polls >= IDLE_POLLS_PER_FRAME) {
            run_frame();
        }
        return s_vsync_count;
    }
    if (mode == 1 || psx_in_interrupt) {
        return HBLANKS_PER_FRAME;
    }
    /* Wait until `mode` blanks after the previous wait, and at least one. */
    target = s_last_wait_count + (mode ? mode : 1);
    if (target <= s_vsync_count) {
        target = s_vsync_count + 1;
    }
    while (s_vsync_count < target) {
        run_frame();
    }
    s_last_wait_count = s_vsync_count;
    return (s_vsync_count - start) * HBLANKS_PER_FRAME;
}

int psx_VSyncCallback(void (*func)(void))
{
    s_vsync_callback = func;
    return 0;
}

int psx_ResetCallback(void)
{
    return 0;
}

int psx_StopCallback(void)
{
    return 0;
}

long psx_SetVideoMode(long mode)
{
    (void)mode;
    return 0;
}

/* --- start-up calls with nothing to do on the port ---------------------- */

void psx___main(void)
{
}

void psx_SetMem(long size)
{
    (void)size;
}

long psx_SetConf(unsigned long events, unsigned long tcbs, unsigned long stack)
{
    (void)events;
    (void)tcbs;
    (void)stack;
    return 0;
}

void psx__96_remove(void)
{
}

void psx_ChangeClearPAD(long mode)
{
    (void)mode;
}
