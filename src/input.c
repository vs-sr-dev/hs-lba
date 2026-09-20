/*
 * input.c — HyperScan controller driver (I2C master, SPG290 block at 0x88130000).
 *
 * Why not the SDK's HS_Controller.c: it deadlocks. Two separate reasons, both
 * visible by lining its code up against the register model MAME implements
 * (spg290_i2c.cpp, derived from retail HyperScan titles):
 *
 *   1. I2C_Init() never sets C_I2C_INT_EN (bit 1 of P_I2C_INT_STATUS). The
 *      completion flag, bit 0, is only ever raised when bit 1 is set — see
 *      spg290_i2c_device::i2c_update(), `if (m_irq_control & 0x02)`. So
 *      I2C_Read8()'s `while (a == 0) a = *P_I2C_INT_STATUS;` spins forever on
 *      the very first byte. The constant exists in SPG290_Constants.h; the
 *      init function just never uses it.
 *
 *   2. Even with the enable set, I2C_Read8() acknowledges with
 *      `*P_I2C_INT_STATUS = C_I2C_INT_FLAG;` — a plain store of 0x01, which
 *      clears the enable bit along with the flag. The first read would then
 *      succeed and the second hang. Acking has to write ENABLE|FLAG.
 *
 * So: keep the enable bit set at all times, and never spin unbounded. Two
 * independent completion signals are polled — the interrupt flag and the
 * 8-bit ACK bit in the mode register, which the controller raises separately —
 * so a quirk in either path on real hardware still lets a transfer retire, and
 * a spin budget bounds the wait if neither ever does.
 *
 * On the interrupt. This is polled I/O — through m5 it stayed polled because
 * PSR.IE was clear, so the line the block asserts on every completed transfer
 * was simply never taken. Once m6 turned interrupts on, that stopped being
 * free: every byte read raised vector 39 with nothing behind it, and it showed
 * up immediately as ~1700 spurious interrupts in a ten-second run.
 *
 * The handler installed below is not an attempt to convert this driver to
 * interrupt-driven I/O. It exists because leaving the flag set on real hardware
 * would re-assert a level-triggered line the moment the handler returned and
 * livelock the machine, so *something* has to acknowledge it. Since it has to
 * run anyway, it also raises a software completion flag, which becomes a third
 * way for a transfer to retire alongside the status flag and the ACK bit.
 *
 * Controller selection. Real hardware strobes a TFT GPIO line per pad and
 * addresses registers 0..4 on both; MAME instead decodes the pad from bits 4-7
 * of the I2C data address (spg29x_mame.cpp, `port = (offset >> 4) & 0x0f`).
 * Pad 0 is register 0x00..0x04 under both conventions, which is what this
 * driver uses. LBA is single-player, so the ambiguity never has to be
 * resolved; HS_INPUT_PAD2 documents where it would go.
 */

#include "input.h"
#include "irq.h"

typedef volatile unsigned int vu32;

#define I2C_BASE        0x88130000u
#define I2C_MODE_CTRL   (*(vu32 *)(I2C_BASE + 0x20))
#define I2C_INT_STATUS  (*(vu32 *)(I2C_BASE + 0x24))
#define I2C_RATE_SETUP  (*(vu32 *)(I2C_BASE + 0x28))
#define I2C_SLAVE_ADDR  (*(vu32 *)(I2C_BASE + 0x2c))
#define I2C_DATA_ADDR   (*(vu32 *)(I2C_BASE + 0x30))
#define I2C_RX_DATA     (*(vu32 *)(I2C_BASE + 0x38))

#define I2C_CLK_CONF    (*(vu32 *)0x88210094u)
#define I2C_IFACE_SEL   (*(vu32 *)0x88200004u)

#define TFT_GPIO_DATA   (*(vu32 *)0x88200014u)
#define TFT_GPIO_OUTEN  (*(vu32 *)0x88200018u)
#define TFT_GPIO_PULLUP (*(vu32 *)0x8820001cu)
#define TFT_GPIO_PULLDN (*(vu32 *)0x88200020u)

#define CLK_EN          0x00000001u
#define RST_DIS         0x00000002u
#define PORT_SEL        0x00000001u

#define M_START8        0x00000001u
#define M_ACK8          0x00000008u
#define M_RX            0x00000040u

#define INT_FLAG        0x00000001u
#define INT_EN          0x00000002u

/* Build with -DHS_I2C_NO_INT_EN to hold the interrupt enable low and prove the
 * ACK path alone retires a transfer. That is the case that matters if real
 * hardware turns out not to raise the completion flag the way MAME does. */
#ifdef HS_I2C_NO_INT_EN
#define INT_KEEP        0u
#else
#define INT_KEEP        INT_EN
#endif

#define PAD_ADDR        0x54u       /* controller MCU, SPC11122A */

/*
 * Bus rate. MAME derives the SCL period as clock/4/((rate & 0x3ff) + 1) off the
 * 27 MHz crystal, and charges 2 + 9*4 clocks for an 8-bit read. The SDK's own
 * value, 0x258, works out to 11 kHz and 3.4 ms per byte — 17 ms to read one
 * pad, an entire frame of latency for four bytes of buttons. 66 gives 100 kHz,
 * I2C standard mode, which any slave on the bus supports, and 0.38 ms/byte.
 *
 * If a real console ever disagrees, HS_I2C_RATE_SDK is the value the retail
 * firmware shipped with, and is the thing to fall back to.
 */
#define HS_I2C_RATE      66
#define HS_I2C_RATE_SDK  0x258

/* Spin budget per byte. MAME runs the CPU at ~18M instructions/s, so this is
 * on the order of 100 ms — far past a 0.38 ms transfer, and short enough that
 * a dead bus degrades to "no input" instead of a lockup. */
#define HS_I2C_SPINS     200000u

/* Stick deadzone, in units of the raw 8-bit axis. */
#define HS_STICK_DEAD    40

static hs_pad pad0;
static unsigned int n_transfers;
static unsigned int n_timeouts;
static volatile unsigned int i2c_done;

/* Runs only if irq_init() has been called; the polling paths below stand on
 * their own if it has not. */
static void i2c_isr(void)
{
    I2C_INT_STATUS = INT_KEEP | INT_FLAG;
    i2c_done = 1;
}

static void pad_select(unsigned int pad, int on)
{
    /* Real hardware gates each controller with a TFT GPIO line. MAME does not
     * model this region, so the writes are inert there. */
    if (on)
        TFT_GPIO_DATA |= (1u << pad);
    else
        TFT_GPIO_DATA &= ~(1u << pad);
}

static int i2c_read8(unsigned int reg, unsigned int *out)
{
    unsigned int spins = HS_I2C_SPINS;

    n_transfers++;

    /* Clear a stale completion flag while holding the enable bit — the store
     * that the SDK gets wrong. */
    i2c_done = 0;
    I2C_INT_STATUS = INT_KEEP | INT_FLAG;

    I2C_DATA_ADDR = reg;
    I2C_MODE_CTRL = M_RX | M_START8;    /* also clears the ACK bits */

    for (;;) {
        if (i2c_done)
            break;
        if (I2C_INT_STATUS & INT_FLAG)
            break;
        if (I2C_MODE_CTRL & M_ACK8)
            break;
        if (--spins == 0) {
            n_timeouts++;
            return -1;
        }
    }

    I2C_INT_STATUS = INT_KEEP | INT_FLAG;
    *out = I2C_RX_DATA & 0xffu;
    return 0;
}

/* Read the five report bytes of one pad. Byte 4 is not part of the button
 * state — MAME returns a checksum there, and the SDK's own comment says the
 * controller stalls if the host stops short of reading every port — so it is
 * read and kept, but nothing is gated on it. */
static int pad_read(unsigned int pad, unsigned char *raw)
{
    unsigned int i, v;
    int rc = 0;

    I2C_SLAVE_ADDR = PAD_ADDR;
    pad_select(pad, 1);

    for (i = 0; i < 5; i++) {
        if (i2c_read8(i, &v) < 0) {
            rc = -1;
            break;
        }
        raw[i] = (unsigned char)v;
    }

    pad_select(pad, 0);
    return rc;
}

static void decode(hs_pad *p, const unsigned char *raw)
{
    unsigned int b = raw[0] | ((unsigned int)raw[1] << 8);
    int x, y;

    p->pressed  = b & ~p->buttons;
    p->released = ~b & p->buttons;
    p->buttons  = b;

    /*
     * Axes come back as unsigned with 0x7f at rest, and both count the *other*
     * way from the obvious reading: MAME declares them
     * `IPT_AD_STICK_X/Y ... PORT_REVERSE` (hyperscan_ctrl.cpp) and passes the
     * port value straight into the report, so up and right are the values
     * *above* centre. Taking them at face value put up on down, which is what
     * showed in-game. Left/right is inverted by exactly the same declaration.
     *
     * m5's axis test compared against the injected port value, so it proved
     * the bytes crossed the bus intact and could not have caught this.
     */
    x = 0x7f - (int)raw[3];
    y = 0x7f - (int)raw[2];
    p->ax = x;
    p->ay = y;

    p->dpad = 0;
    if (x < -HS_STICK_DEAD) p->dpad |= HS_DPAD_LEFT;
    if (x >  HS_STICK_DEAD) p->dpad |= HS_DPAD_RIGHT;
    if (y < -HS_STICK_DEAD) p->dpad |= HS_DPAD_UP;
    if (y >  HS_STICK_DEAD) p->dpad |= HS_DPAD_DOWN;
}

int hs_input_init(void)
{
    unsigned int i;

    I2C_CLK_CONF   = CLK_EN | RST_DIS;
    I2C_IFACE_SEL |= PORT_SEL;
    I2C_RATE_SETUP = HS_I2C_RATE;
    I2C_SLAVE_ADDR = PAD_ADDR;

    /* The bit the SDK forgets. Without it the completion flag never rises. */
    I2C_INT_STATUS = INT_KEEP;

    irq_set_handler(IRQ_I2C, i2c_isr);

    TFT_GPIO_OUTEN  = 0x000fffffu;
    TFT_GPIO_PULLUP = 0x000fffffu;
    TFT_GPIO_PULLDN = 0x00000000u;
    TFT_GPIO_DATA   = 0;

    for (i = 0; i < 5; i++)
        pad0.raw[i] = (i >= 2 && i <= 3) ? 0x7f : 0;
    pad0.buttons = 0;

    return hs_input_poll();
}

int hs_input_poll(void)
{
    unsigned char raw[5];
    int i;

    if (pad_read(0, raw) < 0) {
        pad0.pressed = pad0.released = 0;
        pad0.ok = 0;
        return -1;
    }

    for (i = 0; i < 5; i++)
        pad0.raw[i] = raw[i];
    decode(&pad0, raw);
    pad0.ok = 1;
    return 0;
}

const hs_pad *hs_input_pad(void)
{
    return &pad0;
}

int hs_input_read_raw(unsigned int reg, unsigned int *out)
{
    return i2c_read8(reg, out);
}

unsigned int hs_input_transfers(void) { return n_transfers; }
unsigned int hs_input_timeouts(void)  { return n_timeouts; }
