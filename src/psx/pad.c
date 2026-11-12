/* The controller library. The game registers two receive buffers and reads
 * the raw packets a pad would send; the port fills port 1 as a standard
 * digital pad and leaves port 2 empty. */
#include "psx.h"

#define PAD_STATE_DISCONNECTED 0
#define PAD_STATE_STABLE 6
#define PACKET_OK 0x00
#define PACKET_NONE 0xFF
#define PACKET_DIGITAL_PAD 0x41 /* type 4, one data word */

static uint8_t *s_buffer[2];

void psx_pad_update(uint16_t buttons)
{
    if (s_buffer[0]) {
        s_buffer[0][0] = PACKET_OK;
        s_buffer[0][1] = PACKET_DIGITAL_PAD;
        /* The pad reports pressed buttons as 0 bits. */
        s_buffer[0][2] = (uint8_t)~buttons;
        s_buffer[0][3] = (uint8_t)(~buttons >> 8);
    }
    if (s_buffer[1]) {
        s_buffer[1][0] = PACKET_NONE;
    }
}

void psx_PadInitDirect(uint8_t *port1, uint8_t *port2)
{
    s_buffer[0] = port1;
    s_buffer[1] = port2;
    psx_pad_update(0);
}

int psx_PadGetState(int port)
{
    return port == 0 ? PAD_STATE_STABLE : PAD_STATE_DISCONNECTED;
}

/* A digital pad has no alternative modes and no vibration motors. */
int psx_PadInfoMode(int port, int term, int offs)
{
    (void)port;
    (void)term;
    (void)offs;
    return 0;
}

int psx_PadInfoAct(int port, int actuator, int term)
{
    (void)port;
    (void)actuator;
    (void)term;
    return 0;
}

int psx_PadSetActAlign(int port, const uint8_t *alignment)
{
    (void)port;
    (void)alignment;
    return 0;
}

int psx_PadSetMainMode(int port, int mode, int lock)
{
    (void)port;
    (void)mode;
    (void)lock;
    return 0;
}

void psx_PadSetAct(int port, uint8_t *data, int len)
{
    (void)port;
    (void)data;
    (void)len;
}

int psx_PadChkVsync(void)
{
    return 1;
}

void psx_PadStartCom(void)
{
}

void psx_PadStopCom(void)
{
}
