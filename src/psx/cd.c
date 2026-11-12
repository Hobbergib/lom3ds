/* The CD-ROM library, served from the disc image.
 *
 * The game drives the drive with non-blocking commands and two callbacks:
 * "sync" when a command completes and "ready" for each sector read. Both are
 * delivered from psx_cd_poll(), the way the drive's interrupts arrive on the
 * console: at most one completion and one sector per poll, so the game's
 * buffering sees them one at a time. Ordinary reads are paced only by how
 * often the game polls; real-time streams run at the drive's double speed
 * because the game times video and audio by them. */
#include "psx.h"

#include <string.h>

enum {
    CMD_NOP = 0x01,
    CMD_SETLOC = 0x02,
    CMD_READ_N = 0x06,
    CMD_STANDBY = 0x07,
    CMD_STOP = 0x08,
    CMD_PAUSE = 0x09,
    CMD_MUTE = 0x0B,
    CMD_DEMUTE = 0x0C,
    CMD_SETFILTER = 0x0D,
    CMD_SETMODE = 0x0E,
    CMD_GETPARAM = 0x0F,
    CMD_GETLOC_L = 0x10,
    CMD_GETLOC_P = 0x11,
    CMD_GET_TN = 0x13,
    CMD_GET_TD = 0x14,
    CMD_SEEK_L = 0x15,
    CMD_SEEK_P = 0x16,
    CMD_READ_S = 0x1B
};

enum { INTR_DATA_READY = 1, INTR_COMPLETE = 2, INTR_DISK_ERROR = 5 };
enum { STAT_ERROR = 0x01, STAT_STANDBY = 0x02, STAT_READ = 0x20 };
enum { MODE_SPEED = 0x80, MODE_REALTIME = 0x40, MODE_WHOLE_SECTOR = 0x20, MODE_FILTER = 0x08 };
enum { SUBMODE_AUDIO = 0x04 };

#define LEAD_IN_FRAMES 150
#define DATA_OFFSET_IN_WHOLE 12 /* header and subheader precede the data */
/* Double speed is 150 sectors a second: five sectors every two frames. */
#define REALTIME_SECTORS_PER_2_FRAMES 5

typedef void (*CdCallback)(uint8_t intr, uint8_t *result);

typedef struct {
    uint8_t minute, second, sector, track;
} CdLoc;

static Disc *s_disc;
static CdCallback s_sync_callback, s_ready_callback;
static uint8_t s_result[8];
static uint8_t s_mode;
static uint8_t s_filter_file, s_filter_channel;
static uint32_t s_setloc_lba, s_read_lba;
static bool s_setloc_pending; /* a Setloc the drive has not acted on yet */
static bool s_reading;
static bool s_sync_pending;
static uint8_t s_sync_intr;
static int s_realtime_budget; /* in half sectors */
static uint8_t s_sector[DISC_SECTOR_WHOLE];
static int s_sector_pos, s_sector_len;

static uint8_t from_bcd(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 15));
}

static uint8_t to_bcd(int v)
{
    return (uint8_t)((v / 10) << 4 | v % 10);
}

static uint8_t status(void)
{
    return (uint8_t)(STAT_STANDBY | (s_reading ? STAT_READ : 0));
}

void psx_cd_attach(Disc *disc)
{
    s_disc = disc;
}

CdLoc *psx_CdIntToPos(int sector, CdLoc *pos)
{
    sector += LEAD_IN_FRAMES;
    pos->sector = to_bcd(sector % 75);
    pos->second = to_bcd(sector / 75 % 60);
    pos->minute = to_bcd(sector / 75 / 60);
    return pos;
}

int psx_CdPosToInt(const CdLoc *pos)
{
    return (from_bcd(pos->minute) * 60 + from_bcd(pos->second)) * 75 + from_bcd(pos->sector) - LEAD_IN_FRAMES;
}

/* Carries out a command; returns how many result bytes it produced. */
static int run_command(uint8_t command, const uint8_t *param)
{
    int result_len = 1;

    s_sync_intr = INTR_COMPLETE;
    switch (command) {
    case CMD_SETLOC:
    case CMD_READ_N:
    case CMD_READ_S:
    case CMD_SEEK_L:
    case CMD_SEEK_P:
        if (param) {
            s_setloc_lba = (uint32_t)psx_CdPosToInt((const CdLoc *)param);
            s_setloc_pending = true;
        }
        if (command == CMD_SETLOC) {
            break;
        }
        /* Without a new target, a read carries on from where the head is. */
        if (s_setloc_pending) {
            s_read_lba = s_setloc_lba;
            s_setloc_pending = false;
        }
        s_reading = command == CMD_READ_N || command == CMD_READ_S;
        break;
    case CMD_SETMODE:
        s_mode = param ? param[0] : 0;
        break;
    case CMD_SETFILTER:
        if (param) {
            s_filter_file = param[0];
            s_filter_channel = param[1];
        }
        break;
    case CMD_PAUSE:
    case CMD_STOP:
    case CMD_STANDBY:
        s_reading = false;
        break;
    case CMD_GET_TN:
        s_result[1] = s_result[2] = 1; /* one track, the data track */
        result_len = 3;
        break;
    case CMD_NOP:
    case CMD_MUTE:
    case CMD_DEMUTE:
        break;
    default:
        psx_log("CD command 0x%02X not handled", command);
        break;
    }
    s_result[0] = status();
    s_sync_pending = true;
    return result_len;
}

static void deliver_sync(void)
{
    s_sync_pending = false;
    if (s_sync_callback) {
        s_sync_callback(s_sync_intr, s_result);
    }
}

/* Reads on to the next sector the game should see and hands it to the
 * ready callback. */
static void deliver_sector(void)
{
    bool realtime = (s_mode & MODE_REALTIME) != 0;

    for (;;) {
        if (!s_reading || !s_ready_callback || !s_disc) {
            return;
        }
        if (realtime) {
            if (s_realtime_budget < 2) {
                return;
            }
            s_realtime_budget -= 2;
        }
        if (disc_read_whole(s_disc, s_read_lba, s_sector)) {
            s_reading = false;
            s_result[0] = (uint8_t)(status() | STAT_ERROR);
            s_ready_callback(INTR_DISK_ERROR, s_result);
            return;
        }
        s_read_lba++;
        /* In real-time mode the drive plays audio sectors itself instead of
         * delivering them. (Playback arrives with the sound core.) */
        if (!(realtime && (s_sector[6] & SUBMODE_AUDIO))) {
            break;
        }
    }
    if (s_mode & MODE_WHOLE_SECTOR) {
        s_sector_pos = 0;
        s_sector_len = DISC_SECTOR_WHOLE;
    } else {
        s_sector_pos = DATA_OFFSET_IN_WHOLE;
        s_sector_len = DATA_OFFSET_IN_WHOLE + DISC_SECTOR_DATA;
    }
    s_result[0] = status();
    s_ready_callback(INTR_DATA_READY, s_result);
}

void psx_cd_poll(void)
{
    psx_stage = "disc events";
    if (s_sync_pending) {
        deliver_sync();
    }
    /* A completion handler may have issued the next command; its own
     * completion comes first, at the next poll. */
    if (!s_sync_pending) {
        deliver_sector();
    }
}

void psx_cd_frame(void)
{
    /* 5 sectors per 2 frames, in half-sector units: 5 per frame. */
    s_realtime_budget += REALTIME_SECTORS_PER_2_FRAMES;
    if (s_realtime_budget > 4 * REALTIME_SECTORS_PER_2_FRAMES) {
        s_realtime_budget = 4 * REALTIME_SECTORS_PER_2_FRAMES;
    }
}

int psx_CdInit(void)
{
    s_sync_callback = s_ready_callback = NULL;
    s_reading = s_sync_pending = false;
    s_mode = 0;
    return 1;
}

int psx_CdSetDebug(int level)
{
    (void)level;
    return 0;
}

CdCallback psx_CdSyncCallback(CdCallback func)
{
    CdCallback old = s_sync_callback;

    s_sync_callback = func;
    return old;
}

CdCallback psx_CdReadyCallback(CdCallback func)
{
    CdCallback old = s_ready_callback;

    s_ready_callback = func;
    return old;
}

int psx_CdControlF(uint8_t command, uint8_t *param)
{
    run_command(command, param);
    return 1;
}

int psx_CdControlB(uint8_t command, uint8_t *param, uint8_t *result)
{
    int len = run_command(command, param);

    if (result) {
        memcpy(result, s_result, (size_t)len);
    }
    /* A blocking command completes before it returns, wherever it is
     * called from, and its completion handler runs as on the console. */
    {
        bool nested = psx_in_interrupt;

        psx_in_interrupt = true;
        deliver_sync();
        psx_in_interrupt = nested;
    }
    return 1;
}

int psx_CdSync(int mode, uint8_t *result)
{
    if (mode == 0) {
        while (s_sync_pending && !psx_in_interrupt) {
            psx_poll();
        }
    }
    if (result) {
        result[0] = status();
    }
    return s_sync_pending ? 0 : INTR_COMPLETE;
}

int psx_CdGetSector(void *dst, int words)
{
    int bytes = words * 4;

    if (bytes > s_sector_len - s_sector_pos) {
        bytes = s_sector_len - s_sector_pos;
    }
    if (bytes > 0) {
        memcpy(dst, s_sector + s_sector_pos, (size_t)bytes);
        s_sector_pos += bytes;
    }
    return 1;
}

void psx_CdFlush(void)
{
    s_sync_pending = false;
    s_reading = false;
}

int psx_CdDiskReady(int mode)
{
    (void)mode;
    return INTR_COMPLETE;
}

int psx_CdGetDiskType(void)
{
    return 2; /* CdlCdromFormat */
}

int psx_CdMix(const void *volume)
{
    (void)volume;
    return 1;
}
