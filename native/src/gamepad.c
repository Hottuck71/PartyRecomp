/* Gamepads (Xbox, PlayStation, Switch Pro and others, through SDL3) as Wii Remotes, up to four.

   Buttons: A (bottom face button) is A, B (right face button) and the right trigger are B, X (left) is 1, Y (top)
   is 2, Start is +, Back/Select is -, the Guide/PS/Home button is HOME, and the d-pad is the d-pad. The left
   stick also works the d-pad. The left bumper shakes the remote; holding the left trigger makes the left stick
   tilt it instead.

   Pointer: the right stick moves it, and a pad with a gyro (PlayStation, Switch Pro, many others) points by
   turning it, like a Wii Remote. Pressing the right stick re-centres it. A pad with an accelerometer also gives
   real motion: tilting and shaking it moves the stand-in remote the same way.

   Polled on the window's thread once a frame (SDL's events are pumped there); the CPU thread takes a copy with
   gamepad_get. Pads keep their slot while connected, so players don't swap when one is unplugged. */
#include <windows.h>
#include <math.h>
#include <string.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_sensor.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include "gamepad.h"

extern FILE* report;

#define WII_LEFT 0x0100u
#define WII_RIGHT 0x0200u
#define WII_DOWN 0x0400u
#define WII_UP 0x0800u
#define WII_PLUS 0x1000u
#define WII_TWO 0x0001u
#define WII_ONE 0x0002u
#define WII_B 0x0004u
#define WII_A 0x0008u
#define WII_MINUS 0x0010u
#define WII_HOME 0x0080u

#define STICK_DEAD 0.25f
#define STICK_DPAD 0.55f          /* the left stick presses a d-pad direction past this */
#define TRIGGER_DOWN 0.5f
#define STICK_POINTER_SPEED 1.6f  /* half-screens a second at full tilt */
#define GYRO_HALF_WIDTH 22.0f     /* degrees of turn from the centre to the window's side */
#define GYRO_HALF_HEIGHT 16.0f
#define GYRO_DEAD 0.6f            /* degrees a second: below this a pad is held still */
#define POINTER_LIMIT 1.25f       /* a little past the window's edge, so the pointer can leave the screen */
#define TILT_RADIANS 0.9f

typedef struct {
    SDL_Gamepad* pad;
    SDL_JoystickID id;
    int gyro, accel;
    float x, y;            /* the pointer */
    int rumble_wanted, rumble_sent, player, player_sent;
    PadInput in;
} Pad;

static CRITICAL_SECTION lock;
static Pad pads[GAMEPAD_SLOTS];
static int ready, failed;
static Uint64 last_ns;
static u32 seen, frames_with_input;

static float axis(SDL_Gamepad* g, SDL_GamepadAxis a) {
    float v = (float)SDL_GetGamepadAxis(g, a) / 32767.0f;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    if (fabsf(v) < STICK_DEAD) return 0.0f;
    return (v - (v > 0 ? STICK_DEAD : -STICK_DEAD)) / (1.0f - STICK_DEAD);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void open_new_pads(void) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) return;
    for (int i = 0; i < count; i++) {
        int known = 0;
        for (int s = 0; s < GAMEPAD_SLOTS; s++)
            if (pads[s].pad && pads[s].id == ids[i]) known = 1;
        if (known) continue;
        int slot = -1;
        for (int s = 0; s < GAMEPAD_SLOTS && slot < 0; s++)
            if (!pads[s].pad) slot = s;
        if (slot < 0) break;
        if (SDL_GetGamepadVendorForID(ids[i]) == 0x057E &&
            (SDL_GetGamepadProductForID(ids[i]) == 0x2006 || SDL_GetGamepadProductForID(ids[i]) == 0x2007))
            continue; /* a Joy-Con: joycon.c drives it as a Wii Remote */
        SDL_Gamepad* g = SDL_OpenGamepad(ids[i]);
        if (!g) continue;
        Pad* p = &pads[slot];
        memset(p, 0, sizeof *p);
        p->pad = g;
        p->id = ids[i];
        p->player = p->player_sent = -1;
        if (SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO)) p->gyro = SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true);
        if (SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL)) p->accel = SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, true);
        seen++;
        const char* name = SDL_GetGamepadName(g);
        fprintf(report, "Gamepad %d: %s connected, gyro %s, accelerometer %s\n", slot + 1, name ? name : "(no name)",
                p->gyro ? "yes" : "no", p->accel ? "yes" : "no");
        fflush(report);
    }
    SDL_free(ids);
}

static void read_pad(Pad* p, float dt) {
    SDL_Gamepad* g = p->pad;
    PadInput in;
    memset(&in, 0, sizeof in);
    in.present = 1;
    in.id = (u32)p->id;
    struct { SDL_GamepadButton b; u32 bit; } map[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, WII_A},       {SDL_GAMEPAD_BUTTON_EAST, WII_B},
        {SDL_GAMEPAD_BUTTON_WEST, WII_ONE},      {SDL_GAMEPAD_BUTTON_NORTH, WII_TWO},
        {SDL_GAMEPAD_BUTTON_START, WII_PLUS},    {SDL_GAMEPAD_BUTTON_BACK, WII_MINUS},
        {SDL_GAMEPAD_BUTTON_GUIDE, WII_HOME},    {SDL_GAMEPAD_BUTTON_DPAD_UP, WII_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, WII_DOWN}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, WII_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, WII_RIGHT},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (SDL_GetGamepadButton(g, map[i].b)) in.buttons |= map[i].bit;
    if (axis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_DOWN) in.buttons |= WII_B;
    int tilting = axis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_DOWN;
    float lx = axis(g, SDL_GAMEPAD_AXIS_LEFTX), ly = axis(g, SDL_GAMEPAD_AXIS_LEFTY);
    if (tilting) {
        in.roll = lx * TILT_RADIANS;
        in.pitch = -ly * TILT_RADIANS; /* stick up tilts the remote's tip up */
    } else {
        if (lx < -STICK_DPAD) in.buttons |= WII_LEFT;
        if (lx > STICK_DPAD) in.buttons |= WII_RIGHT;
        if (ly < -STICK_DPAD) in.buttons |= WII_UP;
        if (ly > STICK_DPAD) in.buttons |= WII_DOWN;
        in.nx = lx;
        in.ny = -ly;
    }
    in.swing = SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? 1.0f : 0.0f;

    /* The pointer: the right stick, and the gyro if there is one. */
    float rx = axis(g, SDL_GAMEPAD_AXIS_RIGHTX), ry = axis(g, SDL_GAMEPAD_AXIS_RIGHTY);
    p->x += rx * fabsf(rx) * STICK_POINTER_SPEED * dt;
    p->y += ry * fabsf(ry) * STICK_POINTER_SPEED * dt;
    float rate[3];
    if (p->gyro && SDL_GetGamepadSensorData(g, SDL_SENSOR_GYRO, rate, 3)) {
        /* Radians a second, counter-clockwise seen from the positive end of each axis: x pitch, y yaw. Tipping the
           far edge up turns about +x, and turning left about +y; the window's y grows downwards. */
        float pitch = rate[0] * 57.29578f, yaw = rate[1] * 57.29578f;
        if (fabsf(pitch) < GYRO_DEAD) pitch = 0;
        if (fabsf(yaw) < GYRO_DEAD) yaw = 0;
        p->x -= yaw * dt / GYRO_HALF_WIDTH;
        p->y -= pitch * dt / GYRO_HALF_HEIGHT;
    }
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) p->x = p->y = 0;
    p->x = clampf(p->x, -POINTER_LIMIT, POINTER_LIMIT);
    p->y = clampf(p->y, -POINTER_LIMIT, POINTER_LIMIT);
    in.x = p->x;
    in.y = p->y;
    in.pointer = fabsf(p->x) <= 1.0f && fabsf(p->y) <= 1.0f;

    /* Real motion: gravity and shakes as the remote's accelerometer would feel them. SDL's axes for a pad held
       in front of you: +x right, +y up, +z towards you. The stand-in's: +x for a tilt to the right, +y for the tip
       tipped up, +z up. */
    float a[3];
    if (p->accel && !tilting && SDL_GetGamepadSensorData(g, SDL_SENSOR_ACCEL, a, 3)) {
        in.motion = 1;
        in.accel[0] = -a[0] / SDL_STANDARD_GRAVITY;
        in.accel[1] = -a[2] / SDL_STANDARD_GRAVITY;
        in.accel[2] = a[1] / SDL_STANDARD_GRAVITY;
    }
    if (in.buttons || in.swing > 0 || rx || ry || lx || ly) frames_with_input++;
    p->in = in;
}

static void start(void) {
    InitializeCriticalSection(&lock);
    ready = 1;
    /* aurora has usually started SDL's gamepads already; this only makes sure. Wii Remotes are left to
       real_remote.c (main.c tells SDL not to drive them before anything starts). */
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        failed = 1;
        fprintf(report, "Gamepads: SDL couldn't start them: %s\n", SDL_GetError());
    }
    last_ns = SDL_GetTicksNS();
}

void gamepad_poll(void) {
    static u32 polls;
    if (!ready) start();
    if (failed) return;
    Uint64 now = SDL_GetTicksNS();
    float dt = (float)((double)(now - last_ns) / 1e9);
    last_ns = now;
    if (dt > 0.1f) dt = 0.1f;
    if (polls++ % 30 == 0) open_new_pads(); /* twice a second is often enough to notice a new pad */
    EnterCriticalSection(&lock);
    for (int s = 0; s < GAMEPAD_SLOTS; s++) {
        Pad* p = &pads[s];
        if (!p->pad) continue;
        if (!SDL_GamepadConnected(p->pad)) {
            fprintf(report, "Gamepad %d: disconnected\n", s + 1);
            fflush(report);
            SDL_CloseGamepad(p->pad);
            memset(p, 0, sizeof *p);
            continue;
        }
        read_pad(p, dt);
        if (p->rumble_wanted != p->rumble_sent || p->rumble_wanted) {
            /* A Wii Remote's motor is on or off: renewed each frame while on, so it stops if the game hangs. */
            SDL_RumbleGamepad(p->pad, p->rumble_wanted ? 0x9000 : 0, p->rumble_wanted ? 0x6000 : 0, 100);
            p->rumble_sent = p->rumble_wanted;
        }
        if (p->player != p->player_sent) {
            SDL_SetGamepadPlayerIndex(p->pad, p->player);
            p->player_sent = p->player;
        }
    }
    LeaveCriticalSection(&lock);
}

int gamepad_get(int slot, PadInput* out) {
    memset(out, 0, sizeof *out);
    if (!ready || slot < 0 || slot >= GAMEPAD_SLOTS) return 0;
    EnterCriticalSection(&lock);
    if (pads[slot].pad) *out = pads[slot].in;
    LeaveCriticalSection(&lock);
    return out->present;
}

void gamepad_output(int slot, int player, int rumble) {
    if (!ready || slot < 0 || slot >= GAMEPAD_SLOTS) return;
    EnterCriticalSection(&lock);
    pads[slot].rumble_wanted = rumble != 0;
    pads[slot].player = player;
    LeaveCriticalSection(&lock);
}

void gamepad_write_report(FILE* out) {
    if (!seen) return;
    fprintf(out, "gamepads: %u connected this session, %u frames with input\n", seen, frames_with_input);
}
