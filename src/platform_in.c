/*
 * platform_in.c — LIB_SYS's keyboard/joystick surface, on the HyperScan pad.
 *
 * The engine reads three globals maintained by the DOS IRQ 9 handler: `Key` (a
 * raw scan code), `Joy` (a direction bitmask) and `Fire` (action buttons). The
 * platform's whole job is to keep them current, and the word that matters is
 * *asynchronously*: game code spins on them without calling back into the
 * platform at all. GAMEMENU.C's DoGameMenu() is the case that proves it — its
 * Vsync() is commented out, and after every selection it sits in
 * `while (Joy OR Fire OR Key)` waiting for a release. Driving the poll from
 * Vsync()/Flip() froze all three the moment the main menu appeared: the intro
 * responded, the menu could not be operated at all.
 *
 * So the read runs from the vblank interrupt, as it does on the DS port. It
 * costs 1.9 ms of spinning on the I2C bus inside an ISR — 5.7% of a 33 ms
 * frame, and it delays the 50 Hz timer interrupt by well under its own period,
 * so no tick is lost. It also makes this the only caller, which is what keeps
 * the non-reentrant bus driver safe.
 *
 * The pad has no D-pad, so `Joy` comes from the analog stick through the
 * deadzone in src/input.c. The button mapping is the DOS keyboard laid onto
 * the pad one key at a time — see the table at PORT_PollInput().
 *
 * There is no mouse. LBA uses it only in the menus, which are fully navigable
 * from the pad, so the mouse globals exist and never move — the same state the
 * DOS build reports when no mouse driver is loaded.
 */

#include "input.h"
#include "video.h"
#include "cd.h"

typedef unsigned char  UBYTE;
typedef unsigned short UWORD;
typedef signed short   WORD;
typedef unsigned long  ULONG;
typedef signed long    LONG;

/* Joy bits, as INPUT.ASM defined them. */
#define J_UP     0x0001
#define J_DOWN   0x0002
#define J_LEFT   0x0004
#define J_RIGHT  0x0008

/* Fire bits (LIB_SYS.H). The names are the DOS keys; what each one does in
 * game is what matters here, and is written against the engine:
 *   F_SPACE  action — talk, pick up, jump, attack or hide depending on the
 *            current behaviour            (OBJECT.C, the switch on Comportement)
 *   F_ALT    throw the magic ball / use the weapon          (OBJECT.C:2263)
 *   F_CTRL   behaviour menu                                 (PERSO.C:737)
 *   F_SHIFT  inventory                                      (PERSO.C:564)
 *   F_RETURN recentre the camera on Twinsen                 (PERSO.C:837)
 */
#define F_SPACE  0x0001
#define F_RETURN 0x0002
#define F_CTRL   0x0004
#define F_ALT    0x0008
#define F_SHIFT  0x0020

/* Scan codes the engine tests by name (LIB_SYS.H). */
#define K_ESC    1
#define K_1      2
#define K_2      3
#define K_H      35
#define K_RETURN 28
#define K_SPACE  57

/* Behaviours, in the order the behaviour menu cycles them (COMMON.H). Repeated
 * here rather than pulled in: this file is the platform, and c_extern.h drags
 * the whole engine's declarations with it. */
#define C_NORMAL    0
#define C_SPORTIF   1
#define C_AGRESSIF  2
#define C_DISCRET   3

/*
 * What the shoulders and triggers do.
 *
 *   0  L1/R1 step the behaviour one place left/right, L2/R2 keep the two
 *      weapon keys (1 = magic ball, 2 = magic sword).
 *   1  one behaviour per back button — L1 Normal, L2 Sporty, R1 Aggressive,
 *      R2 Stealth. Costs the weapon keys, which then live only in the
 *      inventory.
 *
 * Either way the back buttons never open the behaviour menu — they call
 * SetComportement() directly, see PERSO.C. Red still opens it deliberately,
 * because that menu is also the status screen.
 */
#ifndef HS_SHOULDER_DIRECT
#define HS_SHOULDER_DIRECT 0
#endif

volatile UWORD Key = 0;
volatile UWORD FuncKey = 0;
volatile UWORD Joy = 0;
volatile UWORD Fire = 0;
UWORD AsciiMode = 0;

/*
 * Behaviour mailboxes, posted here from the vblank interrupt and consumed once
 * per main-loop iteration in PERSO.C — the same shape as the DS port's touch-UI
 * mailboxes, and for the same reason: SetComportement() reloads Twinsen's body
 * and cannot be called from an interrupt.
 *
 * Step is -1/+1/0, Set is 0..3 or -1. Only one of the two is ever posted; which
 * one depends on HS_SHOULDER_DIRECT above.
 */
volatile WORD PORT_ComportementStep = 0;
volatile WORD PORT_ComportementSet = -1;

/* MOUSE.C globals: present, never moving. */
volatile LONG Click = 0;
volatile LONG Mouse_X = 0;
volatile LONG Mouse_Y = 0;
LONG Mouse_X_Dep = 0;
LONG Mouse_Y_Dep = 0;
UBYTE *GphMouse = 0;

void PORT_PollInput(void);

void InitKeyboard(void)
{
    Key = Joy = Fire = FuncKey = 0;
    PORT_ComportementStep = 0;
    PORT_ComportementSet = -1;
    hs_input_init();
    video_set_vblank_hook(PORT_PollInput);
}

void ClearKeyboard(void)
{
    video_set_vblank_hook(0);
    Key = Joy = Fire = FuncKey = 0;
}

/*
 * Runs in the vblank interrupt — once per frame, and from nowhere else.
 *
 * Except while the disc is streaming. One pad read is five I2C transfers and
 * 1.9 ms of spinning, and cd_read()'s servo hands over a frame every 166 us
 * into a one-frame ring: stalling it loses sectors, and the corruption only
 * surfaces much later as an HQR header that decodes to nonsense and a pointer
 * built from it. Nothing needs the pad during a load, so this simply stands
 * aside — the loads are the only thing on screen at the time.
 */
void PORT_PollInput(void)
{
    const hs_pad *p;
    UWORD joy = 0, fire = 0, key = 0;

    if (cd_streaming)
        return;

    hs_input_poll();
    p = hs_input_pad();

    if (p->dpad & HS_DPAD_UP)    joy |= J_UP;
    if (p->dpad & HS_DPAD_DOWN)  joy |= J_DOWN;
    if (p->dpad & HS_DPAD_LEFT)  joy |= J_LEFT;
    if (p->dpad & HS_DPAD_RIGHT) joy |= J_RIGHT;

    /*
     * The face buttons and Start/Select are the DOS keyboard, key for key:
     *
     *   Green   Space    action — talk, pick up, jump, attack, hide
     *   Yellow  Enter    throw the magic ball / recentre, and "validate"
     *   Blue    Alt      use the weapon
     *   Red     Ctrl     status: hold for life, magic, money and keys
     *   Select  Shift    inventory
     *   Start   Escape   the quit / save menu
     *
     * F_CTRL is worth a note, because it does two things at once and the
     * second is the one that matters here. It opens the behaviour menu — a
     * modifier held down while the player steers with the stick — but that
     * menu is also the game's only status screen: DrawMenuComportement() ends
     * in DrawInfoMenu(), which is the life bar, the magic bar, the gold and
     * the keys (GAMEMENU.C). There is nowhere else to read any of that, so
     * the button has to exist even though the shoulders below already cover
     * changing behaviour without it.
     */
    if (p->buttons & HS_BTN_GREEN)  fire |= F_SPACE;   /* action            */
    if (p->buttons & HS_BTN_YELLOW) fire |= F_RETURN;  /* recentre / valid. */
    if (p->buttons & HS_BTN_BLUE)   fire |= F_ALT;     /* throw / weapon    */
    if (p->buttons & HS_BTN_RED)    fire |= F_CTRL;    /* status / behav.   */
    if (p->buttons & HS_BTN_SELECT) fire |= F_SHIFT;   /* inventory         */

    if (p->buttons & HS_BTN_START)  key = K_ESC;

    /*
     * K_H, the holomap, has no button of its own: there are six keys worth
     * having and six buttons that are not back buttons. It is the first entry
     * in the inventory (PERSO.C's InventoryAction switch, case 0), so Select
     * then Green reaches it — which is one press more than the keyboard, and
     * not a function that has been lost.
     *
     * The engine reads one scan code at a time, so the two below are
     * exclusive with each other and with Start; they are all modal, so that
     * is not a constraint in practice.
     */
#if !HS_SHOULDER_DIRECT
    if (p->buttons & HS_BTN_LT)     key = K_1;         /* magic ball        */
    if (p->buttons & HS_BTN_RT)     key = K_2;         /* magic sword       */
#endif

    /*
     * Behaviour, on the back buttons. These post to a mailbox on the *edge*
     * rather than adding a Fire bit: everything above is level-triggered
     * state the engine re-reads for as long as it is held, but a behaviour
     * change is a one-shot, and holding a shoulder must not keep spinning
     * through the ring at 50 Hz.
     *
     * A press that lands while the previous one is still unconsumed
     * overwrites it. The main loop consumes these every iteration, so that
     * only happens if the frame took longer than the gap between two presses,
     * and the last thing pressed is the right thing to honour anyway.
     */
#if HS_SHOULDER_DIRECT
    if (p->pressed & HS_BTN_LS)     PORT_ComportementSet = C_NORMAL;
    if (p->pressed & HS_BTN_LT)     PORT_ComportementSet = C_SPORTIF;
    if (p->pressed & HS_BTN_RS)     PORT_ComportementSet = C_AGRESSIF;
    if (p->pressed & HS_BTN_RT)     PORT_ComportementSet = C_DISCRET;
#else
    if (p->pressed & HS_BTN_LS)     PORT_ComportementStep = -1;
    if (p->pressed & HS_BTN_RS)     PORT_ComportementStep = 1;
#endif

    Joy = joy;
    Fire = fire;
    Key = key;
}

UWORD GetAscii(void)
{
    return 0;
}

/* MOUSE.C */
void GetMouseDep(void) { Mouse_X_Dep = 0; Mouse_Y_Dep = 0; }
void ShowMouse(long on) { (void)on; }
void AffMouse(void) { }

/* SYSTEM.C */
void InitSystem(void)  { }
void ClearSystem(void) { }
