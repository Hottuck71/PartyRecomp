/* Real Wii Remotes, driven by the runtime itself: one for online play, up to four in local play. See
   remote_driver.c. The functions without _at are the first remote's (slot 0). */
#ifndef REMOTE_DRIVER_H
#define REMOTE_DRIVER_H
#include <stdio.h>
#include "fake_remote.h"

#define REMOTE_DRIVER_WII_SLOTS 4 /* slots 0 to 3: real Wii Remotes */
#define REMOTE_DRIVER_SLOTS 8     /* slots 4 to 7: Joy-Cons (joycon.c) standing in for Wii Remotes */

/* Online: starts talking to the first remote (real_remote.c, or its simulator with --sim-real-remote) and waits up
   to wait_ms for one to answer and be set up: MotionPlus on, the Nunchuk if there is one, calibration read,
   reports flowing. Returns 1 if it's ready. */
int remote_driver_start(u32 wait_ms, int simulated, int sim_nunchuk);
/* Local play: drives every remote that turns up, up to four, without waiting for any. */
void remote_driver_start_all(int simulated); /* simulated: the software remote, for tests */
int remote_driver_ready(void);
int remote_driver_ready_at(int slot);
void remote_driver_calibration(RemoteCalibration* out);
void remote_driver_calibration_at(int slot, RemoteCalibration* out);
/* The remote's samples since the last call (offsets spread evenly over a field), and *plug set to 1 or 0 when
   the Nunchuk went in or came out, else -1. */
u32 remote_driver_take(RemoteSample* out, u32 max, s32* plug);
u32 remote_driver_take_at(int slot, RemoteSample* out, u32 max, s32* plug);
/* The game's player lights and rumble for a stand-in, passed on to the real remote behind it. */
void remote_driver_output(u32 leds, int rumble);
void remote_driver_output_at(int slot, u32 leds, int rumble);
void remote_driver_write_report(FILE* out);
int remote_driver_self_test(void); /* --test-remote-driver: against the simulated remote */

#endif
