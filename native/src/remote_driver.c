/* Real Wii Remotes driven by the runtime itself, up to four: one per slot of real_remote.c.

   The game only ever talks to stand-ins (fake_remote.c). Online, every PC's game talks to stand-ins, one per
   player, and they must all see the same reports. In local play the same way lets the runtime switch on each
   remote's MotionPlus itself, even in games like Wii Party that never use it, so the pointer works without a
   sensor bar. So this sets each remote up and streams what it measures: each report becomes a RemoteSample (raw
   buttons, accelerometer, MotionPlus gyro, Nunchuk, and sensor bar dots), which the stand-in for that player
   replays, reading it with this remote's own calibration.

   Set-up, from wiibrew.org/wiki/Wiimote: status (is an extension in?), accelerometer calibration from EEPROM
   0x16, MotionPlus at 0xA600FA; the Nunchuk first if there is one (0x55 to 0xA400F0 and 0x00 to 0xA400FB, the
   unencrypted way, then its ID and calibration); then 0x55 to 0xA600F0 and 4, or 5 with a Nunchuk, to
   0xA600FE, which switches MotionPlus on in front of the Nunchuk; its calibration from 0xA40020; the IR camera;
   and continuous reports in mode 0x37 (buttons, accelerometer, ten bytes of IR, six of extension).

   Each remote has its own thread, so Bluetooth waits never hold up the game or each other. A Nunchuk plugged in
   or out later is seen in the MotionPlus data's extension bit, and MotionPlus is switched over to match. With no
   sensor bar, the dots come from that remote's MotionPlus pointer (motion_pointer.c), or for the first remote
   from the mouse while it moves. The functions without _at are slot 0's.

   Slots 4 to 7 are Joy-Cons (joycon.c): each gives samples as a Wii Remote Plus would, with a fixed calibration,
   and gets the same MotionPlus pointer, so a seat takes a Joy-Con just as it takes a Wii Remote. Online, a Joy-Con
   plays when no Wii Remote is connected. */
#include <windows.h>
#include <math.h>
#include <string.h>
#include "remote_driver.h"
#include "motion_pointer.h"
#include "real_remote.h"
#include "joycon.h"

#define STEP_WAIT_MS 700u
#define QUEUE 128u
#define FIELD_MS 16u
#define PLUG_CONFIRM 12u /* MotionPlus samples in a row before an extension change counts */

extern FILE* report;

typedef struct {
    int slot;
    CRITICAL_SECTION lock;
    RemoteSample queue[QUEUE];
    u32 queue_head, queue_tail, dropped;
    RemoteCalibration cal;
    volatile LONG ready, plug_event, wanted_leds, wanted_rumble;
    int motion_plus, nunchuk_in, mode_set, a_held;
    u32 samples, inits, plugs, step_timeouts, status_reports;
    u8 sent_leds, sent_rumble;
} Driver;

static Driver drivers[REMOTE_DRIVER_SLOTS];
static int started;
static int online_slot; /* online: this PC's remote, slot 0, or the first Joy-Con's slot when no Wii Remote is there */

static int is_joycon(int slot) { return (int)((unsigned)slot % REMOTE_DRIVER_SLOTS) >= REMOTE_DRIVER_WII_SLOTS; }
static int joycon_of(int slot) { return (int)((unsigned)slot % REMOTE_DRIVER_SLOTS) - REMOTE_DRIVER_WII_SLOTS; }

static Driver* driver_at(int slot) { return &drivers[(unsigned)slot % REMOTE_DRIVER_SLOTS]; }

/* ---- Talking to the remote ---- */

static void send_out(Driver* d, const u8* bytes, u32 size) {
    u8 out[REMOTE_REPORT_SIZE];
    memset(out, 0, sizeof out);
    memcpy(out, bytes, size);
    if (size > 1) out[1] = (u8)((out[1] & ~1u) | (d->sent_rumble & 1u)); /* every output report carries rumble */
    real_remote_write_at(d->slot, out, size);
}

static void set_reporting(Driver* d) {
    const u8 mode[3] = {0x12, 0x04, 0x37};
    send_out(d, mode, sizeof mode);
    d->mode_set = 1;
}

/* Reads until a report with this id arrives, or the wait runs out. Other reports pass by, except that a status
   report means the remote stopped its data reports, so they're asked for again once set up. */
static int wait_report(Driver* d, u8 id, u8* out, u32 ms, int (*wanted)(const u8*, const void*), const void* arg) {
    DWORD until = GetTickCount() + ms;
    double when;
    while ((LONG)(until - GetTickCount()) > 0) {
        if (!real_remote_read_at(d->slot, out, &when)) {
            Sleep(1);
            continue;
        }
        if (out[0] == 0x20) d->status_reports++;
        if (out[0] == id && (!wanted || wanted(out, arg))) return 1;
    }
    d->step_timeouts++;
    return 0;
}

static int reply_for(const u8* r, const void* arg) { return (u32)((r[4] << 8) | r[5]) == *(const u32*)arg; }
static int ack_for(const u8* r, const void* arg) { (void)arg; return r[3] == 0x16; }

/* Reads memory: space 0 is EEPROM, 4 the registers. Replies come 16 bytes at a time. */
static int read_memory(Driver* d, u32 space, u32 address, u8* out, u32 size) {
    const u8 ask[7] = {0x17, (u8)space, (u8)(address >> 16), (u8)(address >> 8), (u8)address, (u8)(size >> 8), (u8)size};
    u8 r[REMOTE_REPORT_SIZE];
    send_out(d, ask, sizeof ask);
    for (u32 done = 0; done < size;) {
        u32 at = (address + done) & 0xFFFFu;
        if (!wait_report(d, 0x21, r, STEP_WAIT_MS, reply_for, &at)) return 0;
        if (r[3] & 0x0Fu) return 0; /* nothing there */
        u32 step = (r[3] >> 4) + 1u;
        if (step > size - done) step = size - done;
        memcpy(out + done, r + 6, step);
        done += step;
    }
    return 1;
}

static int write_memory(Driver* d, u32 address, const u8* data, u32 size) {
    u8 w[22] = {0x16, 0x04, (u8)(address >> 16), (u8)(address >> 8), (u8)address, (u8)size};
    u8 r[REMOTE_REPORT_SIZE];
    memcpy(w + 6, data, size);
    send_out(d, w, 22);
    return wait_report(d, 0x22, r, STEP_WAIT_MS, ack_for, NULL) && r[4] == 0;
}

static int write_byte(Driver* d, u32 address, u8 value) { return write_memory(d, address, &value, 1); }

static int status_flags(Driver* d, u8* flags) {
    const u8 ask[2] = {0x15, 0x00};
    u8 r[REMOTE_REPORT_SIZE];
    send_out(d, ask, sizeof ask);
    if (!wait_report(d, 0x20, r, STEP_WAIT_MS, NULL, NULL)) return 0;
    *flags = r[3];
    return 1;
}

/* Waits a moment and lets status reports from a MotionPlus switching over go by. */
static void settle(Driver* d, u32 ms) {
    DWORD until = GetTickCount() + ms;
    u8 r[REMOTE_REPORT_SIZE];
    double when;
    while ((LONG)(until - GetTickCount()) > 0)
        if (!real_remote_read_at(d->slot, r, &when)) Sleep(1);
        else if (r[0] == 0x20) d->status_reports++;
}

/* The Nunchuk on its own, MotionPlus off: unencrypted set-up, ID, calibration. */
static int start_nunchuk(Driver* d) {
    u8 id[6];
    if (!write_byte(d, 0x04A400F0u, 0x55) || !write_byte(d, 0x04A400FBu, 0x00)) return 0;
    if (!read_memory(d, 4, 0xA400FAu, id, 6)) return 0;
    if (!(id[2] == 0xA4 && id[3] == 0x20 && id[4] == 0x00 && id[5] == 0x00)) return 0; /* not a Nunchuk */
    if (!read_memory(d, 4, 0xA40020u, d->cal.nunchuk_cal, 16)) memset(d->cal.nunchuk_cal, 0, 16);
    return 1;
}

/* MotionPlus on, passing the Nunchuk through if there is one, and its calibration. */
static int start_motion_plus(Driver* d, int with_nunchuk) {
    if (!write_byte(d, 0x04A600F0u, 0x55) || !write_byte(d, 0x04A600FEu, with_nunchuk ? 0x05 : 0x04)) return 0;
    settle(d, 150);
    for (int tries = 0; tries < 3; tries++)
        if (read_memory(d, 4, 0xA40020u, d->cal.gyro, 32)) return 1;
    return 0;
}

static void stop_motion_plus(Driver* d) {
    write_byte(d, 0x04A400F0u, 0x55);
    settle(d, 150);
}

/* The IR camera, for a sensor bar if there is one: sensitivity from wiibrew's suggested settings, basic mode. */
static void start_camera(Driver* d) {
    static const u8 block1[9] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0x00, 0xC0};
    static const u8 block2[2] = {0x40, 0x00};
    const u8 on1[2] = {0x13, 0x04}, on2[2] = {0x1A, 0x04};
    send_out(d, on1, 2);
    send_out(d, on2, 2);
    write_byte(d, 0x04B00030u, 0x08);
    write_memory(d, 0x04B00000u, block1, 9);
    write_memory(d, 0x04B0001Au, block2, 2);
    write_byte(d, 0x04B00033u, 0x01);
    write_byte(d, 0x04B00030u, 0x08);
}

static int set_up(Driver* d) {
    u8 flags = 0, id[6];
    const u8 lights[2] = {0x11, (u8)((d->wanted_leds & 15) << 4)};
    send_out(d, lights, 2);
    if (!status_flags(d, &flags)) return 0;
    if (!read_memory(d, 0, 0x16u, d->cal.accel, 10)) return 0;
    /* A MotionPlus left on from before answers at 0xA4 and not 0xA6: switch it off first. */
    if (read_memory(d, 4, 0xA400FAu, id, 6) && id[2] == 0xA4 && id[3] == 0x20 && id[5] == 0x05) stop_motion_plus(d);
    d->motion_plus = read_memory(d, 4, 0xA600FAu, id, 6) && id[2] == 0xA6 && id[3] == 0x20 && id[5] == 0x05;
    status_flags(d, &flags);
    d->nunchuk_in = (flags & 2) ? start_nunchuk(d) : 0;
    if (d->motion_plus && !start_motion_plus(d, d->nunchuk_in)) d->motion_plus = 0;
    start_camera(d);
    set_reporting(d);
    d->cal.present = 1;
    d->cal.motion_plus = (u8)d->motion_plus;
    d->cal.nunchuk = (u8)d->nunchuk_in;
    d->inits++;
    fprintf(report, "Remote driver %d: set up, MotionPlus %s, Nunchuk %s\n", d->slot + 1, d->motion_plus ? "yes" : "no",
            d->nunchuk_in ? "in" : "out");
    fflush(report);
    return 1;
}

/* A Nunchuk went in or came out while MotionPlus was on: switch it over to match, as the game does. */
static void replug(Driver* d, int now_in) {
    stop_motion_plus(d);
    if (now_in) now_in = start_nunchuk(d);
    if (d->motion_plus) start_motion_plus(d, now_in);
    set_reporting(d);
    d->nunchuk_in = now_in;
    d->plugs++;
    InterlockedExchange(&d->plug_event, now_in);
    fprintf(report, "Remote driver %d: Nunchuk %s\n", d->slot + 1, now_in ? "plugged in" : "taken out");
    fflush(report);
}

/* ---- Reports to samples ---- */

static void decode_dots(const u8* ir, RemoteSample* s) {
    const u8* p = ir; /* the first two of the four dots */
    s->dots[0][0] = (u16)(p[0] | ((p[2] >> 4) & 3) << 8);
    s->dots[0][1] = (u16)(p[1] | ((p[2] >> 6) & 3) << 8);
    s->dots[1][0] = (u16)(p[3] | (p[2] & 3) << 8);
    s->dots[1][1] = (u16)(p[4] | ((p[2] >> 2) & 3) << 8);
    for (int k = 0; k < 2; k++)
        if (s->dots[k][0] >= 1023 || s->dots[k][1] >= 1023) s->dots[k][0] = s->dots[k][1] = NO_DOT;
}

/* The six extension bytes: MotionPlus data (byte 5 bits 1-0 are 2), a passed-through Nunchuk (0), or with
   MotionPlus off the Nunchuk's own format. Returns the MotionPlus extension bit, or -1. */
static int decode_extension(Driver* d, const u8* e, RemoteSample* s) {
    if (d->motion_plus && (e[5] & 3u) == 2u) {
        s->gyro[0] = (u16)(e[0] | (e[3] >> 2) << 8);
        s->gyro[1] = (u16)(e[1] | (e[4] >> 2) << 8);
        s->gyro[2] = (u16)(e[2] | (e[5] >> 2) << 8);
        s->gyro_flags = (u8)(SAMPLE_HAS_MOTION_PLUS | ((e[3] >> 1) & 1) | (((e[4] >> 1) & 1) << 1) | ((e[3] & 1) << 2));
        return e[4] & 1;
    }
    if (d->motion_plus && (e[5] & 3u) == 0u && d->nunchuk_in) {
        s->stick[0] = e[0];
        s->stick[1] = e[1];
        s->naccel[0] = (u16)(e[2] << 2 | ((e[5] >> 4) & 1) << 1);
        s->naccel[1] = (u16)(e[3] << 2 | ((e[5] >> 5) & 1) << 1);
        s->naccel[2] = (u16)((e[4] >> 1) << 3 | ((e[5] >> 6) & 3) << 1);
        s->nunchuk_flags = (u8)(SAMPLE_HAS_NUNCHUK | ((e[5] & 8) ? 0 : 1) | ((e[5] & 4) ? 0 : 2));
    } else if (!d->motion_plus && d->nunchuk_in) {
        s->stick[0] = e[0];
        s->stick[1] = e[1];
        s->naccel[0] = (u16)(e[2] << 2 | ((e[5] >> 2) & 3));
        s->naccel[1] = (u16)(e[3] << 2 | ((e[5] >> 4) & 3));
        s->naccel[2] = (u16)(e[4] << 2 | ((e[5] >> 6) & 3));
        s->nunchuk_flags = (u8)(SAMPLE_HAS_NUNCHUK | ((e[5] & 2) ? 0 : 1) | ((e[5] & 1) ? 0 : 2));
    }
    return -1;
}

/* Where the game should see the sensor bar: the remote's own dots if it sees a bar, else (first remote only) the
   mouse while it moves, else the MotionPlus pointer; and pressing A with nothing on the screen centres that
   pointer, so "point at the screen and press A" always works. */
static void choose_dots(Driver* d, const u8* r, double when, RemoteSample* s) {
    float x, y;
    int a_pressed = (r[2] & 0x08) && !d->a_held;
    d->a_held = (r[2] & 0x08) != 0;
    if (d->motion_plus) motion_pointer_feed_at(d->slot, r, when);
    if (s->dots[0][0] != NO_DOT) return;
    float ox = 0, oy = 0; /* the remote's own pointer: MotionPlus, or tilt with no gyro or while MotionPlus is off */
    int own = d->motion_plus ? motion_pointer_for_report_at(d->slot, r, &ox, &oy)
                             : motion_pointer_tilt_at(d->slot, s->accel, d->cal.accel, &ox, &oy);
    if (d->slot == 0 && fake_remote_mouse_aims(own, ox, oy) && fake_remote_mouse_pointer(&x, &y)) {
        if (d->motion_plus && fake_remote_mouse_moving()) motion_pointer_place_at(d->slot, x, y);
        fake_remote_dots_for(x, y, s->dots);
    } else if (own) {
        fake_remote_dots_for(ox, oy, s->dots);
    } else if (d->motion_plus && a_pressed) {
        motion_pointer_calibrate_at(d->slot);
        fake_remote_dots_for(0, 0, s->dots);
    }
}

static void keep(Driver* d, const RemoteSample* s) {
    EnterCriticalSection(&d->lock);
    if (d->queue_tail - d->queue_head == QUEUE) {
        d->queue_head++;
        d->dropped++;
    }
    d->queue[d->queue_tail++ % QUEUE] = *s;
    LeaveCriticalSection(&d->lock);
}

static DWORD WINAPI run(void* arg) {
    Driver* d = (Driver*)arg;
    u8 r[REMOTE_REPORT_SIZE];
    double when;
    u32 changed = 0;
    for (;;) {
        if (!real_remote_connected_at(d->slot)) {
            if (d->ready) {
                fprintf(report, "Remote driver %d: remote gone\n", d->slot + 1);
                fflush(report);
            }
            InterlockedExchange(&d->ready, 0);
            Sleep(100);
            continue;
        }
        if (!d->ready) {
            if (set_up(d)) {
                EnterCriticalSection(&d->lock);
                d->queue_head = d->queue_tail; /* nothing from before it was ready */
                LeaveCriticalSection(&d->lock);
                InterlockedExchange(&d->ready, 1);
            } else {
                Sleep(500);
            }
            continue;
        }
        if ((u8)d->wanted_leds != d->sent_leds || (u8)d->wanted_rumble != d->sent_rumble) {
            d->sent_rumble = (u8)d->wanted_rumble;
            d->sent_leds = (u8)d->wanted_leds;
            const u8 lights[2] = {0x11, (u8)(d->sent_leds << 4)};
            send_out(d, lights, 2);
        }
        if (!real_remote_read_at(d->slot, r, &when)) {
            Sleep(1);
            continue;
        }
        if (r[0] == 0x20) { /* data reports stop after a status report until asked for again */
            d->status_reports++;
            if (!d->motion_plus && ((r[3] & 2) != 0) != (d->nunchuk_in != 0)) replug(d, (r[3] & 2) != 0);
            else set_reporting(d);
            continue;
        }
        if (r[0] != 0x37) continue;
        RemoteSample s;
        memset(&s, 0, sizeof s);
        s.buttons = (u16)(((r[1] << 8) | r[2]) & 0x9F1Fu);
        s.accel[0] = (u16)(r[3] << 2 | ((r[1] >> 5) & 3));
        s.accel[1] = (u16)(r[4] << 2 | ((r[2] >> 5) & 1) << 1);
        s.accel[2] = (u16)(r[5] << 2 | ((r[2] >> 6) & 1) << 1);
        decode_dots(r + 6, &s);
        int extension = decode_extension(d, r + 16, &s);
        choose_dots(d, r, when, &s);
        d->samples++;
        keep(d, &s);
        /* MotionPlus says whether something is plugged into it: follow a lasting change. */
        if (extension >= 0) {
            changed = (extension != d->nunchuk_in) ? changed + 1 : 0;
            if (changed >= PLUG_CONFIRM) {
                changed = 0;
                replug(d, extension);
            }
        }
    }
    return 0;
}

/* A Joy-Con's samples, with sensor bar dots from its MotionPlus pointer as for a real remote with no sensor bar. */
static u32 take_joycon(Driver* d, RemoteSample* out, u32 max, s32* plug) {
    double when[64];
    int recentre = 0;
    if (max > 64) max = 64;
    u32 n = joycon_take_at(joycon_of(d->slot), out, when, max, plug, &recentre);
    if (recentre) motion_pointer_recentre();
    for (u32 i = 0; i < n; i++) {
        u8 r[REMOTE_REPORT_SIZE];
        joycon_pointer_report(joycon_of(d->slot), i, &out[i], r);
        choose_dots(d, r, when[i], &out[i]);
        d->samples++;
    }
    return n;
}

static void start_joycons(void) {
    for (int s = REMOTE_DRIVER_WII_SLOTS; s < REMOTE_DRIVER_SLOTS; s++) {
        Driver* d = &drivers[s];
        d->slot = s;
        d->motion_plus = 1; /* a Joy-Con's gyro is its MotionPlus */
        d->plug_event = -1;
    }
    joycon_start(0); /* every Joy-Con a remote of its own */
}

static void start_driver(int slot) {
    Driver* d = &drivers[slot];
    d->slot = slot;
    d->plug_event = -1;
    d->wanted_leds = 1u << (slot & 3);
    d->sent_leds = 0xFF;
    InitializeCriticalSection(&d->lock);
    CreateThread(NULL, 0, run, d, 0, NULL);
}

int remote_driver_start(u32 wait_ms, int simulated, int sim_nunchuk) {
    if (simulated) real_remote_start_simulated(sim_nunchuk);
    else real_remote_start();
    if (!started) start_driver(0);
    if (!started && !simulated) start_joycons();
    started = 1;
    DWORD until = GetTickCount() + wait_ms;
    /* A Joy-Con plays only if no Wii Remote is connected: a connected one is waited for. */
    while (!drivers[0].ready && (LONG)(until - GetTickCount()) > 0) {
        if (!simulated && joycon_ready_at(0) && !real_remote_connected()) break;
        Sleep(20);
    }
    if (!drivers[0].ready && !simulated && joycon_ready_at(0)) {
        online_slot = REMOTE_DRIVER_WII_SLOTS;
        fprintf(report, "Remote driver: no Wii Remote; a Joy-Con plays\n");
        return 1;
    }
    if (!drivers[0].ready)
        fprintf(report, "Remote driver: no Wii Remote or Joy-Con ready within %u ms; the keyboard plays\n", wait_ms);
    return drivers[0].ready != 0;
}

void remote_driver_start_all(int simulated) {
    if (started) return;
    if (simulated) real_remote_start_simulated(0);
    else real_remote_start();
    for (int s = 0; s < REMOTE_DRIVER_WII_SLOTS; s++) start_driver(s);
    if (!simulated) start_joycons();
    started = 1;
}

int remote_driver_ready_at(int slot) {
    if (!started) return 0;
    if (is_joycon(slot)) return joycon_ready_at(joycon_of(slot));
    return driver_at(slot)->ready != 0;
}
int remote_driver_ready(void) { return remote_driver_ready_at(online_slot); }

void remote_driver_calibration_at(int slot, RemoteCalibration* out) {
    Driver* d = driver_at(slot);
    if (started && is_joycon(slot)) {
        joycon_calibration_at(joycon_of(slot), out);
        return;
    }
    if (started && d->ready) *out = d->cal;
    else memset(out, 0, sizeof *out);
}
void remote_driver_calibration(RemoteCalibration* out) { remote_driver_calibration_at(online_slot, out); }

u32 remote_driver_take_at(int slot, RemoteSample* out, u32 max, s32* plug) {
    Driver* d = driver_at(slot);
    u32 n = 0;
    *plug = -1;
    if (!started) return 0;
    if (is_joycon(slot)) return take_joycon(d, out, max, plug);
    *plug = InterlockedExchange(&d->plug_event, -1);
    if (!d->ready) return 0;
    EnterCriticalSection(&d->lock);
    /* Far behind, as when samples piled up while players joined: keep only the newest, so play isn't late. */
    if (d->queue_tail - d->queue_head > 3u * max) {
        d->dropped += d->queue_tail - d->queue_head - max;
        d->queue_head = d->queue_tail - max;
    }
    while (d->queue_head != d->queue_tail && n < max) out[n++] = d->queue[d->queue_head++ % QUEUE];
    LeaveCriticalSection(&d->lock);
    for (u32 i = 0; i < n; i++) out[i].offset = (u8)(i * FIELD_MS / n);
    return n;
}
u32 remote_driver_take(RemoteSample* out, u32 max, s32* plug) { return remote_driver_take_at(online_slot, out, max, plug); }

void remote_driver_output_at(int slot, u32 leds, int rumble) {
    Driver* d = driver_at(slot);
    if (is_joycon(slot)) {
        joycon_output_at(joycon_of(slot), leds, rumble);
        return;
    }
    InterlockedExchange(&d->wanted_leds, (LONG)(leds & 15u));
    InterlockedExchange(&d->wanted_rumble, rumble ? 1 : 0);
}
void remote_driver_output(u32 leds, int rumble) { remote_driver_output_at(online_slot, leds, rumble); }

void remote_driver_write_report(FILE* out) {
    for (int s = 0; s < REMOTE_DRIVER_SLOTS; s++) {
        Driver* d = &drivers[s];
        if (!d->inits) continue;
        fprintf(out, "remote driver %d: %s, %u set-ups, %u samples, %u dropped, %u Nunchuk changes, %u status reports, "
                     "%u timed-out steps\n",
                s + 1, d->ready ? "ready" : "not ready", d->inits, d->samples, d->dropped, d->plugs, d->status_reports,
                d->step_timeouts);
    }
    joycon_write_report(out);
}

/* --test-remote-driver: sets up the simulated remote and checks what comes back. */
int remote_driver_self_test(void) {
    int failures = 0;
    if (!report) report = stdout;
    real_remote_sim_still_for(0);
    if (!remote_driver_start(5000, 1, 1)) {
        printf("not ready\n");
        return 1;
    }
    RemoteCalibration cal = drivers[0].cal;
    printf("calibration: accel %02X %02X %02X / %02X %02X %02X, MotionPlus %d, Nunchuk %d, gyro zero %02X%02X, "
           "Nunchuk stick centre %02X\n",
           cal.accel[0], cal.accel[1], cal.accel[2], cal.accel[4], cal.accel[5], cal.accel[6], cal.motion_plus,
           cal.nunchuk, cal.gyro[0], cal.gyro[1], cal.nunchuk_cal[10]);
    if (!cal.motion_plus || !cal.nunchuk || cal.accel[0] != 0x82 || cal.nunchuk_cal[10] != 0x84) failures++;
    Sleep(500);
    RemoteSample got[MAX_SAMPLES];
    s32 plug;
    remote_driver_take(got, MAX_SAMPLES, &plug); /* what piled up meanwhile is trimmed, as when players join */
    u32 total = 0, with_gyro = 0, with_nunchuk = 0;
    u16 yaw_low = 0xFFFF, yaw_high = 0, stick_x = 0;
    for (int field = 0; field < 60; field++) {
        Sleep(16);
        u32 n = remote_driver_take(got, MAX_SAMPLES, &plug);
        for (u32 i = 0; i < n; i++) {
            total++;
            if (got[i].gyro_flags & SAMPLE_HAS_MOTION_PLUS) {
                with_gyro++;
                if (got[i].gyro[0] < yaw_low) yaw_low = got[i].gyro[0];
                if (got[i].gyro[0] > yaw_high) yaw_high = got[i].gyro[0];
            }
            if (got[i].nunchuk_flags & SAMPLE_HAS_NUNCHUK) {
                with_nunchuk++;
                stick_x = got[i].stick[0];
            }
        }
    }
    printf("one second: %u samples, %u with MotionPlus (yaw %u to %u), %u with the Nunchuk (stick x %u)\n", total,
           with_gyro, yaw_low, yaw_high, with_nunchuk, stick_x);
    if (total < 90 || with_gyro < 50 || with_nunchuk < 50 || yaw_high - yaw_low < 200 || stick_x != 0xC0) failures++;
    remote_driver_write_report(stdout);
    printf("remote driver self-test: %d failures\n", failures);
    return failures;
}
