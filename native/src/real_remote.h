/* See real_remote.c. */
#ifndef REAL_REMOTE_H
#define REAL_REMOTE_H
#include <stdint.h>

#define REMOTE_REPORT_SIZE 22u
#define REAL_REMOTE_SLOTS 4 /* remotes open at once; the functions without _at are slot 0's */

int real_remote_connected_at(int slot);
int real_remote_is_plus_at(int slot); /* a Wii Remote Plus: MotionPlus built in */
int real_remote_read_at(int slot, uint8_t* out, double* when);
void real_remote_write_at(int slot, const uint8_t* data, uint32_t size);

void real_remote_start(void);
void real_remote_start_simulated(int nunchuk); /* a software Wii Remote Plus, for tests */
void real_remote_unplug_simulated_after(uint32_t ms); /* its Nunchuk comes out then */
void real_remote_sim_still_for(uint32_t seconds); /* it starts to move then; 45 by default */
int real_remote_connected(void);
int real_remote_read(uint8_t* out, double* when); /* 22 bytes, and when it was read, in seconds. 0 if none waits */
void real_remote_write(const uint8_t* data, uint32_t size);
void real_remote_set_rumble(int on); /* --rumble 0: the remote's motor never starts */
int real_remote_scan_once(uint32_t* devices); /* how many remotes are connected, without opening them */
void real_remote_write_report(void);

#endif
