#ifndef HS_INPUT_H
#define HS_INPUT_H

/*
 * hs_input — HyperScan controller, read over I2C.
 *
 * The pad has no D-pad: four coloured face buttons, Start/Select, two
 * shoulders, two triggers, and one analog stick. `dpad` below is synthesised
 * from the stick so the engine can be driven with a digital direction.
 */

/* byte 0 of the controller report */
#define HS_BTN_BLUE     0x0001
#define HS_BTN_START    0x0002
#define HS_BTN_SELECT   0x0004
#define HS_BTN_LS       0x0008
#define HS_BTN_RS       0x0010
#define HS_BTN_LT       0x0020
#define HS_BTN_RT       0x0040

/* byte 1, shifted up by 8 */
#define HS_BTN_YELLOW   0x2000
#define HS_BTN_RED      0x4000
#define HS_BTN_GREEN    0x8000

/* synthesised from the stick */
#define HS_DPAD_UP      0x01
#define HS_DPAD_DOWN    0x02
#define HS_DPAD_LEFT    0x04
#define HS_DPAD_RIGHT   0x08

typedef struct {
    unsigned int   buttons;     /* HS_BTN_*, current state          */
    unsigned int   pressed;     /* buttons that went down this poll */
    unsigned int   released;    /* ...and up                        */
    signed   int   ax;          /* stick, -128..127, 0 = centred    */
    signed   int   ay;
    unsigned int   dpad;        /* HS_DPAD_*, from ax/ay            */
    unsigned char  raw[5];      /* as read off the bus              */
    unsigned int   ok;          /* 0 if the last poll timed out     */
} hs_pad;

/* Bring up the I2C master and the controller select lines. Returns 0 if the
 * first read of pad 0 completed, -1 if the bus never answered. Never blocks
 * indefinitely — every transfer is bounded by a spin count. */
int  hs_input_init(void);

/* Read pad 0 and update its state. Returns 0 on success, -1 on bus timeout
 * (in which case the previous button state is held). */
int  hs_input_poll(void);

/* Current state of pad 0. Valid after hs_input_init(). */
const hs_pad *hs_input_pad(void);

/* One raw 8-bit read off the bus, for diagnostics and for timing the bus
 * against the rest of a frame. Returns 0 on success, -1 on timeout. */
int  hs_input_read_raw(unsigned int reg, unsigned int *out);

/* Diagnostics: total transfers issued and how many timed out. */
unsigned int hs_input_transfers(void);
unsigned int hs_input_timeouts(void);

#endif
