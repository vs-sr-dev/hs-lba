/* process.h — no such header in newlib. ADELINE.C uses spawnl() only to launch
 * the DOS driver-setup executables (MidiExec/WaveExec); on a console there is
 * nothing to spawn, so report failure and the engine skips the step. */
#ifndef HS_COMPAT_PROCESS_H
#define HS_COMPAT_PROCESS_H

#define P_WAIT 0
#define spawnl(...) (-1)

#endif
