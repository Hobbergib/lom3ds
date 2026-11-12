/* The sound chip (SPU): 24 voices playing ADPCM samples from 512 KiB of
 * sound memory, each with a pitch, a volume pair and an ADSR envelope.
 *
 * The game's sound driver programs the chip by writing its registers, which
 * the port keeps in the RAM image (see PSX_ADDR in game/prelude.h), and by a
 * few library calls. After every tick of the driver the kernel calls
 * psx_spu_mix(), which takes a copy of the registers, picks up and clears
 * the key-on and key-off strobes, and has the sound up to the next tick
 * produced from that copy.
 *
 * Producing the sound is the expensive part, so where the platform has a
 * processor core to spare it runs there: psx_spu_mix() only queues the copy,
 * and a worker turns queued copies into sound. Without one (the host build)
 * the same code runs in place. The worker owns the voices; the game's side
 * only reads a voice's envelope level, and learns of a playback-address
 * interrupt at its next call.
 *
 * Not here yet: reverb, volume sweeps, and CD audio. */
#include "psx.h"

#include "../core/config.h"
#include "../plat/plat.h"

#include <math.h>
#include <string.h>

#define SPU_RAM_SIZE 0x80000
#define VOICES 24
#define BLOCK_BYTES 16
#define BLOCK_SAMPLES 28
#define ENV_MAX 0x7FFF
#define PITCH_MAX 0x3FFF
#define MIX_CHUNK 256

/* Register addresses. */
#define REG_VOICES 0x1F801C00u
#define REG_MAIN_VOLUME 0x1F801D80u
#define REG_KEY_ON 0x1F801D88u
#define REG_KEY_OFF 0x1F801D8Cu
#define REG_PITCH_MOD 0x1F801D90u
#define REG_NOISE 0x1F801D94u
#define REG_END 0x1F801D9Cu

/* Offsets inside a voice's 16-byte register block. */
enum { V_VOL_L = 0, V_VOL_R = 2, V_PITCH = 4, V_START = 6, V_ADSR1 = 8, V_ADSR2 = 10, V_ENVX = 12, V_REPEAT = 14 };

enum { PHASE_OFF, PHASE_ATTACK, PHASE_DECAY, PHASE_SUSTAIN, PHASE_RELEASE };

/* Block flags. */
enum { FLAG_END = 1, FLAG_REPEAT = 2, FLAG_START = 4 };

typedef struct {
    uint32_t addr;   /* next block to decode */
    uint32_t repeat; /* where a looping sample goes back to */
    int16_t block[BLOCK_SAMPLES];
    int pos;         /* next sample of the block; BLOCK_SAMPLES when used up */
    bool last_block; /* the block being played ends a one-shot sample */
    int32_t s1, s2;  /* decoder history */
    int16_t hist[4]; /* the last four samples, oldest first */
    uint32_t counter; /* pitch counter, 12 fraction bits */
    int phase;
    int32_t level;
    int env_wait;
    int16_t out;     /* last output, for pitch modulation of the next voice */
} Voice;

/* The registers 0x1F801C00 to 0x1F801DFF as they were at one tick, with the
 * strobes taken from them and how much sound to produce before the next. */
#define REG_BASE 0x1F801C00u
#define REG_WORDS 0x100
#define QUEUE_SIZE 32 /* a power of two; about eight frames of ticks */

typedef struct {
    uint16_t regs[REG_WORDS];
    uint32_t key_on, key_off;
    int frames;
} Command;

static Command s_queue[QUEUE_SIZE];
static uint32_t s_queue_head, s_queue_tail; /* written by the game side and the worker respectively */
static bool s_threaded;
static uint16_t s_regs[REG_WORDS]; /* the copy the sound is being produced from */
static uint32_t s_ended;           /* voices that have reached the end of their sample */

static uint8_t s_ram[SPU_RAM_SIZE];
static Voice s_voice[VOICES];
static uint32_t s_transfer_addr;
static void (*s_transfer_callback)(void);
static void (*s_irq_callback)(void);
static uint32_t s_irq_addr;
static bool s_irq_on, s_irq_hit;
static int s_noise_clock;
static int32_t s_noise_timer;
static uint16_t s_noise_level = 1;
/* Weights of a smooth four-point curve for each of 256 positions between
 * two samples: the port's stand-in for the chip's Gaussian filter. */
static int16_t s_curve[256][4];

/* A register in the game's memory. */
static uint16_t *game_reg(uint32_t addr)
{
    return (uint16_t *)(psx_ram + PSX_IMAGE_OFFSET(addr));
}

/* A register in the copy the sound is produced from. */
static uint16_t *reg(uint32_t addr)
{
    return &s_regs[(addr - REG_BASE) / 2];
}

static uint16_t *voice_reg(int v, int field)
{
    return reg(REG_VOICES + (uint32_t)v * 16 + (uint32_t)field);
}

static uint32_t reg_pair(uint32_t addr)
{
    return (uint32_t)*reg(addr) | (uint32_t)*reg(addr + 2) << 16;
}

static int clamp16(int v)
{
    return v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v;
}

/* A volume register as a signed level. Sweep mode is not modelled: such a
 * register plays at a moderate fixed level. */
static int volume(uint16_t r)
{
    return (r & 0x8000) ? 0x3FFF : (int16_t)(r << 1);
}

/* --- samples ------------------------------------------------------------ */

static void decode_block(Voice *v, int index)
{
    static const int filter_a[5] = {0, 60, 115, 98, 122}, filter_b[5] = {0, 0, -52, -55, -60};
    const uint8_t *p = &s_ram[v->addr & (SPU_RAM_SIZE - BLOCK_BYTES)];
    int shift = p[0] & 0xF, filter = (p[0] >> 4) & 7, flags = p[1], i;

    if (filter > 4) {
        filter = 4;
    }
    if (shift > 12) {
        shift = 9;
    }
    /* The sound driver asks to be told when playback reaches an address. */
    if (s_irq_on && s_irq_addr - (v->addr & (SPU_RAM_SIZE - BLOCK_BYTES)) < BLOCK_BYTES) {
        s_irq_hit = true;
    }
    if (flags & FLAG_START) {
        v->repeat = v->addr;
    }
    for (i = 0; i < BLOCK_SAMPLES; i++) {
        int nibble = (p[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
        int s = ((int16_t)(nibble << 12) >> shift) + ((v->s1 * filter_a[filter] + v->s2 * filter_b[filter] + 32) >> 6);

        s = clamp16(s);
        v->block[i] = (int16_t)s;
        v->s2 = v->s1;
        v->s1 = s;
    }
    v->pos = 0;
    v->last_block = false;
    if (flags & FLAG_END) {
        __atomic_or_fetch(&s_ended, 1u << index, __ATOMIC_RELAXED);
        v->addr = v->repeat;
        v->last_block = !(flags & FLAG_REPEAT);
    } else {
        v->addr += BLOCK_BYTES;
    }
}

static void next_sample(Voice *v, int index)
{
    if (v->pos >= BLOCK_SAMPLES) {
        if (v->last_block) {
            v->phase = PHASE_OFF;
            v->level = 0;
        }
        decode_block(v, index);
    }
    v->hist[0] = v->hist[1];
    v->hist[1] = v->hist[2];
    v->hist[2] = v->hist[3];
    v->hist[3] = v->block[v->pos++];
}

/* --- envelope ----------------------------------------------------------- */

static void envelope_step(Voice *v, uint16_t adsr1, uint16_t adsr2)
{
    int rate, shift, step, cycles;
    bool exponential, falling;

    switch (v->phase) {
    case PHASE_ATTACK:
        rate = (adsr1 >> 8) & 0x7F;
        exponential = (adsr1 & 0x8000) != 0;
        falling = false;
        break;
    case PHASE_DECAY:
        rate = ((adsr1 >> 4) & 0xF) * 4;
        exponential = true;
        falling = true;
        break;
    case PHASE_SUSTAIN:
        rate = (adsr2 >> 6) & 0x7F;
        exponential = (adsr2 & 0x8000) != 0;
        falling = (adsr2 & 0x4000) != 0;
        break;
    case PHASE_RELEASE:
        rate = (adsr2 & 0x1F) * 4;
        exponential = (adsr2 & 0x20) != 0;
        falling = true;
        break;
    default:
        return;
    }
    shift = rate >> 2;
    step = falling ? -8 + (rate & 3) : 7 - (rate & 3);
    cycles = 1 << (shift > 11 ? shift - 11 : 0);
    step <<= shift < 11 ? 11 - shift : 0;
    if (exponential && !falling && v->level > 0x6000) {
        cycles *= 4;
    }
    if (exponential && falling) {
        step = step * v->level >> 15;
    }
    if (++v->env_wait < cycles) {
        return;
    }
    v->env_wait = 0;
    v->level += step;
    if (v->level < 0) {
        v->level = 0;
    }
    if (v->level > ENV_MAX) {
        v->level = ENV_MAX;
    }

    if (v->phase == PHASE_ATTACK && v->level >= ENV_MAX) {
        v->phase = PHASE_DECAY;
    } else if (v->phase == PHASE_DECAY && v->level <= ((adsr1 & 0xF) + 1) * 0x800) {
        v->phase = PHASE_SUSTAIN;
    } else if (v->phase == PHASE_RELEASE && v->level == 0) {
        v->phase = PHASE_OFF;
    }
}

/* --- mixing ------------------------------------------------------------- */

/* Starts and releases voices. */
static void apply_keys(uint32_t on, uint32_t off)
{
    int i;

    for (i = 0; i < VOICES; i++) {
        Voice *v = &s_voice[i];

        if (off & (1u << i) && v->phase != PHASE_OFF) {
            v->phase = PHASE_RELEASE;
            v->env_wait = 0;
        }
        if (on & (1u << i)) {
            memset(v, 0, sizeof *v);
            v->addr = v->repeat = (uint32_t)*voice_reg(i, V_START) * 8;
            v->pos = BLOCK_SAMPLES;
            v->phase = PHASE_ATTACK;
            __atomic_and_fetch(&s_ended, ~(1u << i), __ATOMIC_RELAXED);
        }
    }
}

static int noise_sample(void)
{
    int shift = (s_noise_clock >> 2) & 0xF, step = s_noise_clock & 3;

    s_noise_timer -= 4 + step;
    if (s_noise_timer < 0) {
        int bit = ((s_noise_level >> 15) ^ (s_noise_level >> 12) ^ (s_noise_level >> 11) ^ (s_noise_level >> 10) ^ 1) & 1;

        s_noise_level = (uint16_t)(s_noise_level << 1 | bit);
        s_noise_timer += 0x20000 >> shift;
        if (s_noise_timer < 0) {
            s_noise_timer = 0x20000 >> shift;
        }
    }
    return (int16_t)s_noise_level;
}

static int interpolate(const Voice *v)
{
    const int16_t *w;

    switch (g_opt[OPT_INTERPOLATION]) {
    case INTERP_NEAREST:
        return v->hist[3];
    case INTERP_LINEAR:
        return v->hist[2] + (((v->hist[3] - v->hist[2]) * (int)(v->counter & 0xFFF)) >> 12);
    default:
        w = s_curve[(v->counter >> 4) & 0xFF];
        return (v->hist[0] * w[0] + v->hist[1] * w[1] + v->hist[2] * w[2] + v->hist[3] * w[3]) >> 15;
    }
}

/* Produces the sound of one command. Runs on the worker when there is one. */
static void run_command(const Command *c)
{
    int16_t out[MIX_CHUNK * 2];
    int user_volume = g_opt[OPT_VOLUME];
    int main_l, main_r, frames = c->frames;
    uint32_t pitch_mod, noise;

    memcpy(s_regs, c->regs, sizeof s_regs);
    apply_keys(c->key_on, c->key_off);
    main_l = volume(*reg(REG_MAIN_VOLUME));
    main_r = volume(*reg(REG_MAIN_VOLUME + 2));
    pitch_mod = reg_pair(REG_PITCH_MOD);
    noise = reg_pair(REG_NOISE);

    while (frames > 0) {
        int n = frames < MIX_CHUNK ? frames : MIX_CHUNK;
        int i, k;

        for (k = 0; k < n; k++) {
            int left = 0, right = 0, previous = 0;
            int noise_now = noise ? noise_sample() : 0;

            for (i = 0; i < VOICES; i++) {
                Voice *v = &s_voice[i];
                int step, s;

                if (v->phase == PHASE_OFF) {
                    previous = 0;
                    continue;
                }
                step = *voice_reg(i, V_PITCH);
                if (i > 0 && (pitch_mod & (1u << i))) {
                    step = (step * (0x8000 + previous)) >> 15;
                }
                if (step > PITCH_MAX) {
                    step = PITCH_MAX;
                }
                v->counter += (uint32_t)step;
                while (v->counter >= 0x1000) {
                    v->counter -= 0x1000;
                    next_sample(v, i);
                }
                s = (noise & (1u << i)) ? noise_now : interpolate(v);
                envelope_step(v, *voice_reg(i, V_ADSR1), *voice_reg(i, V_ADSR2));
                s = (s * v->level) >> 15;
                v->out = (int16_t)s;
                previous = s;
                left += (s * volume(*voice_reg(i, V_VOL_L))) >> 15;
                right += (s * volume(*voice_reg(i, V_VOL_R))) >> 15;
            }
            left = (clamp16(left) * main_l) >> 15;
            right = (clamp16(right) * main_r) >> 15;
            out[k * 2] = (int16_t)(clamp16(left) * user_volume / 10);
            out[k * 2 + 1] = (int16_t)(clamp16(right) * user_volume / 10);
        }
        plat_audio_submit(out, n);
        frames -= n;
    }
}

/* The worker: turns queued commands into sound, sleeping when there are none. */
static void worker(void)
{
    for (;;) {
        uint32_t tail = s_queue_tail;

        if (tail == __atomic_load_n(&s_queue_head, __ATOMIC_ACQUIRE)) {
            plat_worker_sleep();
            psx_watchdog();
            continue;
        }
        run_command(&s_queue[tail % QUEUE_SIZE]);
        __atomic_store_n(&s_queue_tail, tail + 1, __ATOMIC_RELEASE);
    }
}

void psx_spu_mix(int frames)
{
    Command local, *c = &local;
    uint32_t head = s_queue_head, ended;
    int i;

    /* A playback-address interrupt the sound driver asked for. */
    if (__atomic_exchange_n(&s_irq_hit, false, __ATOMIC_RELAXED) && s_irq_callback) {
        bool nested = psx_in_interrupt;

        psx_in_interrupt = true;
        s_irq_callback();
        psx_in_interrupt = nested;
    }
    psx_stage = "queueing sound";
    if (s_threaded) {
        /* The worker has fallen a whole queue behind: let it catch up. */
        int waited = 0;

        while (head - __atomic_load_n(&s_queue_tail, __ATOMIC_ACQUIRE) >= QUEUE_SIZE) {
            plat_worker_wake();
            plat_sleep_us(200);
            if (++waited == 10000) {
                psx_log("sound worker is not keeping up: %u commands queued, %u done", (unsigned)head,
                        (unsigned)s_queue_tail);
            }
        }
        c = &s_queue[head % QUEUE_SIZE];
    }
    memcpy(c->regs, game_reg(REG_BASE), sizeof c->regs);
    c->key_on = (uint32_t)*game_reg(REG_KEY_ON) | (uint32_t)*game_reg(REG_KEY_ON + 2) << 16;
    c->key_off = (uint32_t)*game_reg(REG_KEY_OFF) | (uint32_t)*game_reg(REG_KEY_OFF + 2) << 16;
    c->frames = frames;
    *game_reg(REG_KEY_ON) = *game_reg(REG_KEY_ON + 2) = 0;
    *game_reg(REG_KEY_OFF) = *game_reg(REG_KEY_OFF + 2) = 0;

    /* What the chip reports back: envelope levels and finished voices. */
    for (i = 0; i < VOICES; i++) {
        *game_reg(REG_VOICES + (uint32_t)i * 16 + V_ENVX) = (uint16_t)s_voice[i].level;
    }
    /* A voice keyed on in a command the worker has not reached yet has not
     * ended, whatever its state still says. */
    {
        /* The queue position first: a command finished after this point has
         * already cleared its voices' bits. */
        uint32_t at = s_threaded ? __atomic_load_n(&s_queue_tail, __ATOMIC_ACQUIRE) : head;

        ended = __atomic_load_n(&s_ended, __ATOMIC_RELAXED) & ~c->key_on;
        for (; at != head; at++) {
            ended &= ~s_queue[at % QUEUE_SIZE].key_on;
        }
    }
    *game_reg(REG_END) = (uint16_t)ended;
    *game_reg(REG_END + 2) = (uint16_t)(ended >> 16);

    if (s_threaded) {
        __atomic_store_n(&s_queue_head, head + 1, __ATOMIC_RELEASE);
        plat_worker_wake();
    } else {
        run_command(c);
    }
}

/* --- libspu ------------------------------------------------------------- */

void psx_SpuInit(void)
{
    int i;

    if (!s_threaded) {
        memset(s_voice, 0, sizeof s_voice);
    }
    s_irq_on = s_irq_hit = false;
    for (i = 0; i < 256; i++) {
        /* A cubic B-spline: smooth, slightly soft, like the chip's own filter. */
        double f = i / 256.0;

        s_curve[i][0] = (int16_t)lround((1 - f) * (1 - f) * (1 - f) / 6 * 32768);
        s_curve[i][1] = (int16_t)lround((3 * f * f * f - 6 * f * f + 4) / 6 * 32768);
        s_curve[i][2] = (int16_t)lround((-3 * f * f * f + 3 * f * f + 3 * f + 1) / 6 * 32768);
        s_curve[i][3] = (int16_t)lround(f * f * f / 6 * 32768);
    }
    if (!s_threaded) {
        s_threaded = plat_worker_start(worker);
        psx_log("sound is produced %s", s_threaded ? "on a second processor core" : "on the main thread");
    }
}

void psx_SpuStart(void)
{
}

void psx_SpuQuit(void)
{
}

long psx_SpuInitMalloc(long count, char *table)
{
    (void)table;
    return count;
}

/* The game spins on a flag its completion callback clears, without calling
 * anything that could act as a poll point, so the callback runs before the
 * transfer call returns. */
static void transfer_done(void)
{
    void (*callback)(void) = s_transfer_callback;
    bool nested = psx_in_interrupt;

    if (callback) {
        psx_in_interrupt = true;
        callback();
        psx_in_interrupt = nested;
    }
}

static uint32_t clip_transfer(uint32_t size)
{
    return size > SPU_RAM_SIZE - s_transfer_addr ? SPU_RAM_SIZE - s_transfer_addr : size;
}

uint32_t psx_SpuSetTransferStartAddr(uint32_t addr)
{
    s_transfer_addr = addr & (SPU_RAM_SIZE - 1) & ~7u;
    return s_transfer_addr;
}

long psx_SpuSetTransferMode(long mode)
{
    return mode;
}

void *psx_SpuSetTransferCallback(void (*func)(void))
{
    void (*old)(void) = s_transfer_callback;

    s_transfer_callback = func;
    return (void *)(uintptr_t)old;
}

uint32_t psx_SpuWrite(const uint8_t *src, uint32_t size)
{
    psx_stage = "sound memory transfer";
    size = clip_transfer(size);
    memcpy(s_ram + s_transfer_addr, src, size);
    s_transfer_addr += size;
    transfer_done();
    return size;
}

uint32_t psx_SpuRead(uint8_t *dst, uint32_t size)
{
    size = clip_transfer(size);
    memcpy(dst, s_ram + s_transfer_addr, size);
    s_transfer_addr += size;
    transfer_done();
    return size;
}

long psx_SpuSetIRQ(long on_off)
{
    s_irq_on = on_off == 1;
    return on_off;
}

uint32_t psx_SpuSetIRQAddr(uint32_t addr)
{
    s_irq_addr = addr & (SPU_RAM_SIZE - 1) & ~7u;
    return s_irq_addr;
}

void *psx_SpuSetIRQCallback(void (*func)(void))
{
    void (*old)(void) = s_irq_callback;

    s_irq_callback = func;
    return (void *)(uintptr_t)old;
}

long psx_SpuSetNoiseClock(long clock)
{
    s_noise_clock = (int)(clock & 0x3F);
    return s_noise_clock;
}

void psx_SpuGetVoiceEnvelope(int voice, int16_t *level)
{
    *level = voice >= 0 && voice < VOICES ? (int16_t)s_voice[voice].level : 0;
}

/* Reverb is not implemented: the calls are accepted and report it off. */
long psx_SpuSetReverb(long on_off)
{
    (void)on_off;
    return 0;
}

long psx_SpuSetReverbModeType(long mode)
{
    (void)mode;
    return 0;
}

void psx_SpuSetReverbModeDepth(int16_t left, int16_t right)
{
    (void)left;
    (void)right;
}

void psx_SpuGetReverbModeType(long *mode)
{
    *mode = 0;
}
