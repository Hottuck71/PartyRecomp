/* Real Wii Remotes through Windows HID, up to four at once. A remote has to be paired with Windows first.
   Reports are passed through as they are. Threads keep Bluetooth waits away from the game: one scans for
   remotes, and each slot has one that reads and one that writes. A remote takes the first free slot when it
   turns up; the functions without _at are slot 0's (the first remote), as PairRemote and online play use. */
#include <windows.h>
#include <math.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "real_remote.h"

#define NINTENDO_VENDOR 0x057E
#define REMOTE_PRODUCT 0x0306
#define REMOTE_PLUS_PRODUCT 0x0330
#define SCAN_EVERY_MS 2000
#define READ_WAIT_MS 500
#define WRITE_WAIT_MS 500
#define QUEUE 256
#define MAX_REPORT 64
#define MAX_PATH_TEXT 512

extern FILE* report;

typedef struct {
    uint8_t data[REMOTE_REPORT_SIZE];
} Item;

typedef struct {
    Item items[QUEUE];
    uint32_t head, tail;
    uint32_t sizes[QUEUE];
    double times[QUEUE]; /* seconds, when the report was read */
} Ring;

typedef struct {
    Ring incoming, outgoing;
    HANDLE device, wake_writer;
    volatile LONG connected;
    uint32_t input_size, output_size;
    int use_set_output_report;
    char path[MAX_PATH_TEXT];
    uint64_t reports_read, reports_written, write_failures, reports_dropped;
} Slot;

static CRITICAL_SECTION lock;
static Slot slots[REAL_REMOTE_SLOTS];
static uint32_t devices_seen;
static int started;

static Slot* slot_at(int i) { return &slots[(unsigned)i % REAL_REMOTE_SLOTS]; }

static double seconds_now(void) {
    static LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)frequency.QuadPart;
}

static int ring_push(Ring* r, const uint8_t* data, uint32_t size) {
    int kept = 1;
    if (size > REMOTE_REPORT_SIZE) size = REMOTE_REPORT_SIZE;
    EnterCriticalSection(&lock);
    if (r->tail - r->head == QUEUE) {
        r->head++; /* the oldest report matters least */
        kept = 0;
    }
    memset(r->items[r->tail % QUEUE].data, 0, REMOTE_REPORT_SIZE);
    memcpy(r->items[r->tail % QUEUE].data, data, size);
    r->sizes[r->tail % QUEUE] = size;
    r->times[r->tail % QUEUE] = seconds_now();
    r->tail++;
    LeaveCriticalSection(&lock);
    return kept;
}

static int ring_pop(Ring* r, uint8_t* out, uint32_t* size, double* when) {
    int found = 0;
    EnterCriticalSection(&lock);
    if (r->head != r->tail) {
        memcpy(out, r->items[r->head % QUEUE].data, REMOTE_REPORT_SIZE);
        if (size) *size = r->sizes[r->head % QUEUE];
        if (when) *when = r->times[r->head % QUEUE];
        r->head++;
        found = 1;
    }
    LeaveCriticalSection(&lock);
    return found;
}

/* A remote already open in some slot. */
static int path_in_use(const char* path) {
    for (int i = 0; i < REAL_REMOTE_SLOTS; i++)
        if (slots[i].connected && !strcmp(slots[i].path, path)) return 1;
    return 0;
}

/* Looks through the HID devices for Wii Remotes. With open_into set, opens each one not yet in a slot into a
   free slot. Returns how many remotes it saw; *total is how many devices it looked at. */
static int find_remotes(uint32_t* total, int open_into) {
    GUID guid;
    int seen = 0;
    HidD_GetHidGuid(&guid);
    HDEVINFO list = SetupDiGetClassDevsA(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (list == INVALID_HANDLE_VALUE) return 0;
    *total = 0;
    for (DWORD i = 0;; i++) {
        SP_DEVICE_INTERFACE_DATA item;
        char buffer[1024];
        SP_DEVICE_INTERFACE_DETAIL_DATA_A* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*)buffer;
        item.cbSize = sizeof item;
        if (!SetupDiEnumDeviceInterfaces(list, NULL, &guid, i, &item)) break;
        detail->cbSize = sizeof *detail;
        if (!SetupDiGetDeviceInterfaceDetailA(list, &item, detail, sizeof buffer, NULL, NULL)) continue;
        (*total)++;
        /* Opening with no access is enough to read the IDs, and works for devices others hold. */
        HANDLE probe = CreateFileA(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, 0, NULL);
        if (probe == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attributes;
        attributes.Size = sizeof attributes;
        int is_remote = HidD_GetAttributes(probe, &attributes) && attributes.VendorID == NINTENDO_VENDOR &&
                        (attributes.ProductID == REMOTE_PRODUCT || attributes.ProductID == REMOTE_PLUS_PRODUCT);
        CloseHandle(probe);
        if (!is_remote) continue;
        seen++;
        if (!open_into || path_in_use(detail->DevicePath)) continue;
        int free_slot = -1;
        for (int s = 0; s < REAL_REMOTE_SLOTS && free_slot < 0; s++)
            if (!slots[s].connected) free_slot = s;
        if (free_slot < 0) continue;
        HANDLE found = CreateFileA(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        fprintf(report, "Wii Remote: found product %04X, open %s\n", attributes.ProductID,
                found == INVALID_HANDLE_VALUE ? "failed" : "worked");
        if (found == INVALID_HANDLE_VALUE) continue;
        Slot* sl = &slots[free_slot];
        sl->input_size = sl->output_size = REMOTE_REPORT_SIZE;
        PHIDP_PREPARSED_DATA parsed;
        HIDP_CAPS caps;
        if (HidD_GetPreparsedData(found, &parsed)) {
            if (HidP_GetCaps(parsed, &caps) == HIDP_STATUS_SUCCESS) {
                if (caps.InputReportByteLength) sl->input_size = caps.InputReportByteLength;
                if (caps.OutputReportByteLength) sl->output_size = caps.OutputReportByteLength;
            }
            HidD_FreePreparsedData(parsed);
        }
        EnterCriticalSection(&lock);
        sl->device = found;
        snprintf(sl->path, sizeof sl->path, "%s", detail->DevicePath);
        sl->incoming.head = sl->incoming.tail = sl->outgoing.head = sl->outgoing.tail = 0;
        sl->use_set_output_report = 0;
        LeaveCriticalSection(&lock);
        InterlockedExchange(&sl->connected, 1);
        fprintf(report, "Wii Remote %d: connected, reports %u in, %u out\n", free_slot + 1, sl->input_size,
                sl->output_size);
        fflush(report);
    }
    SetupDiDestroyDeviceInfoList(list);
    return seen;
}

static void lost_remote(Slot* sl) {
    InterlockedExchange(&sl->connected, 0);
    EnterCriticalSection(&lock);
    if (sl->device != INVALID_HANDLE_VALUE) CloseHandle(sl->device);
    sl->device = INVALID_HANDLE_VALUE;
    sl->path[0] = 0;
    sl->incoming.head = sl->incoming.tail = sl->outgoing.head = sl->outgoing.tail = 0;
    LeaveCriticalSection(&lock);
    fprintf(report, "Wii Remote %d: disconnected\n", (int)(sl - slots) + 1);
    fflush(report);
}

/* Looks for new remotes every couple of seconds while a slot is free. */
static DWORD WINAPI scanner(void* unused) {
    (void)unused;
    for (;;) {
        int any_free = 0;
        for (int s = 0; s < REAL_REMOTE_SLOTS; s++)
            if (!slots[s].connected) any_free = 1;
        if (any_free) {
            uint32_t total = 0;
            find_remotes(&total, 1);
            devices_seen = total;
        }
        Sleep(SCAN_EVERY_MS);
    }
    return 0;
}

/* One slot's reports, while it has a remote. */
static DWORD WINAPI reader(void* arg) {
    Slot* sl = (Slot*)arg;
    OVERLAPPED wait;
    uint8_t buffer[MAX_REPORT];
    memset(&wait, 0, sizeof wait);
    wait.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (;;) {
        if (!sl->connected) {
            Sleep(50);
            continue;
        }
        DWORD got = 0;
        ResetEvent(wait.hEvent);
        uint32_t want = sl->input_size < MAX_REPORT ? sl->input_size : MAX_REPORT;
        if (!ReadFile(sl->device, buffer, want, &got, &wait)) {
            if (GetLastError() != ERROR_IO_PENDING) {
                lost_remote(sl);
                continue;
            }
            if (WaitForSingleObject(wait.hEvent, READ_WAIT_MS) != WAIT_OBJECT_0) {
                CancelIo(sl->device); /* a quiet remote is fine: it only reports when asked to */
                GetOverlappedResult(sl->device, &wait, &got, TRUE);
                continue;
            }
            if (!GetOverlappedResult(sl->device, &wait, &got, FALSE)) {
                lost_remote(sl);
                continue;
            }
        }
        if (got) {
            sl->reports_read++;
            if (!ring_push(&sl->incoming, buffer, got)) sl->reports_dropped++;
        }
    }
    return 0;
}

/* WriteFile is right for most Windows Bluetooth setups, and a few only take SetOutputReport. */
static int send_report(Slot* sl, const uint8_t* data, uint32_t size) {
    uint8_t buffer[MAX_REPORT];
    OVERLAPPED wait;
    DWORD done = 0;
    uint32_t length = sl->output_size < MAX_REPORT ? sl->output_size : MAX_REPORT;
    memset(buffer, 0, sizeof buffer);
    memcpy(buffer, data, size < length ? size : length);
    if (sl->use_set_output_report) return HidD_SetOutputReport(sl->device, buffer, length);
    memset(&wait, 0, sizeof wait);
    wait.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    BOOL finished = WriteFile(sl->device, buffer, length, &done, &wait);
    if (!finished && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(wait.hEvent, WRITE_WAIT_MS) == WAIT_OBJECT_0) {
            finished = GetOverlappedResult(sl->device, &wait, &done, FALSE);
        } else {
            CancelIo(sl->device);
            GetOverlappedResult(sl->device, &wait, &done, TRUE);
        }
    }
    CloseHandle(wait.hEvent);
    if (finished) return 1;
    if (HidD_SetOutputReport(sl->device, buffer, length)) {
        sl->use_set_output_report = 1;
        fprintf(report, "Wii Remote %d: this setup uses SetOutputReport\n", (int)(sl - slots) + 1);
        return 1;
    }
    return 0;
}

static void sim_output(const uint8_t* r, uint32_t size);
static int simulated;

static DWORD WINAPI writer(void* arg) {
    Slot* sl = (Slot*)arg;
    uint8_t data[REMOTE_REPORT_SIZE];
    uint32_t size;
    for (;;) {
        WaitForSingleObject(sl->wake_writer, INFINITE);
        while (sl->connected && ring_pop(&sl->outgoing, data, &size, NULL)) {
            if (simulated && sl == slots) {
                sim_output(data, size);
                sl->reports_written++;
            } else if (send_report(sl, data, size)) {
                sl->reports_written++;
            } else {
                sl->write_failures++;
            }
        }
    }
    return 0;
}

static void start_slots(int with_readers) {
    InitializeCriticalSection(&lock);
    for (int s = 0; s < REAL_REMOTE_SLOTS; s++) {
        slots[s].device = INVALID_HANDLE_VALUE;
        slots[s].wake_writer = CreateEventA(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 0, writer, &slots[s], 0, NULL);
        if (with_readers) CreateThread(NULL, 0, reader, &slots[s], 0, NULL);
    }
    started = 1;
}

void real_remote_start(void) {
    if (started) return;
    start_slots(1);
    CreateThread(NULL, 0, scanner, NULL, 0, NULL);
}

int real_remote_connected_at(int slot) { return started && slot_at(slot)->connected != 0; }
int real_remote_connected(void) { return real_remote_connected_at(0); }

int real_remote_read_at(int slot, uint8_t* out, double* when) {
    Slot* sl = slot_at(slot);
    return started && sl->connected && ring_pop(&sl->incoming, out, NULL, when);
}
int real_remote_read(uint8_t* out, double* when) { return real_remote_read_at(0, out, when); }

static int rumble_on = 1;
void real_remote_set_rumble(int on) { rumble_on = on != 0; }

void real_remote_write_at(int slot, const uint8_t* data, uint32_t size) {
    Slot* sl = slot_at(slot);
    if (!started || !sl->connected) return;
    if (!rumble_on && size > 1 && (data[1] & 1u)) { /* rumble off: every output report's first byte carries the motor bit */
        uint8_t quiet[REMOTE_REPORT_SIZE];
        uint32_t n = size < sizeof quiet ? size : (uint32_t)sizeof quiet;
        memcpy(quiet, data, n);
        quiet[1] &= (uint8_t)~1u;
        ring_push(&sl->outgoing, quiet, n);
    } else {
        ring_push(&sl->outgoing, data, size);
    }
    SetEvent(sl->wake_writer);
}
void real_remote_write(const uint8_t* data, uint32_t size) { real_remote_write_at(0, data, size); }

/* Used by --scan-remote and the launcher to check for a remote without opening one. */
int real_remote_scan_once(uint32_t* devices) { return find_remotes(devices, 0); }

void real_remote_write_report(void) {
    for (int s = 0; s < REAL_REMOTE_SLOTS; s++) {
        Slot* sl = &slots[s];
        if (s && !sl->reports_read && !sl->reports_written) continue;
        fprintf(report, "real remote %d read %llu written %llu write_failures %llu dropped %llu hid_devices %u\n", s + 1,
                (unsigned long long)sl->reports_read, (unsigned long long)sl->reports_written,
                (unsigned long long)sl->write_failures, (unsigned long long)sl->reports_dropped, devices_seen);
    }
}

typedef uint8_t u8;
typedef uint32_t u32;

/* ---- A simulated Wii Remote Plus, for testing remote_driver.c and online play without one ----

   It answers what the driver sends as a real one would (status, memory reads and writes, MotionPlus switching on
   in front of a Nunchuk, data reports stopping after a status report until asked for again), with calibration
   values unlike the stand-in's, and streams mode 0x37 reports every 6 ms: a gentle swing on the accelerometer,
   yaw going back and forth on the gyro, and the Nunchuk's stick held right. */
static int sim_nunchuk, sim_mp_on, sim_mp_mode, sim_streaming, sim_turn;
static DWORD sim_mp_ready_at; /* a tick count: a MotionPlus just switched on answers reads from then */
#define SIM_MP_START_MS 400u
static u8 sim_leds, sim_mode;
static CRITICAL_SECTION sim_lock;
static u8 sim_eeprom[0x40], sim_mp_off[256], sim_mp_on_regs[256], sim_ext[256];

static void sim_push(const u8* r) { ring_push(&slots[0].incoming, r, REMOTE_REPORT_SIZE); }

static u32 sim_crc32(const u8* data, u32 size) {
    u32 crc = 0xFFFFFFFFu;
    for (u32 i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void sim_setup(int nunchuk) {
    static const u8 accel[9] = {0x82, 0x7E, 0x80, 0x00, 0x9C, 0x98, 0x9A, 0x00, 0x40};
    static const u8 nun[14] = {0x80, 0x80, 0x80, 0x00, 0xB3, 0xB3, 0xB3, 0x00, 0xE0, 0x20, 0x84, 0xE0, 0x20, 0x80};
    u32 sum = 0x55;
    memset(sim_eeprom, 0, sizeof sim_eeprom);
    memcpy(sim_eeprom + 0x16, accel, 9);
    for (int i = 0; i < 9; i++) sum += accel[i];
    sim_eeprom[0x16 + 9] = (u8)sum;
    memset(sim_mp_off, 0, sizeof sim_mp_off);
    const u8 mp_id[6] = {0x00, 0x00, 0xA6, 0x20, 0x00, 0x05};
    memcpy(sim_mp_off + 0xFA, mp_id, 6);
    /* MotionPlus calibration: zero 33000 and scale 9800 in each half, a reference of 45, CRC-32 of the halves. */
    u8* g = sim_mp_on_regs + 0x20;
    u8 halves[28];
    for (int half = 0; half < 2; half++)
        for (int axis = 0; axis < 3; axis++) {
            g[half * 16 + axis * 2] = (u8)(33000 >> 8);
            g[half * 16 + axis * 2 + 1] = (u8)(33000 & 0xFF);
            g[half * 16 + 6 + axis * 2] = (u8)(9800 >> 8);
            g[half * 16 + 6 + axis * 2 + 1] = (u8)(9800 & 0xFF);
            g[half * 16 + 12] = 45;
        }
    memcpy(halves, g, 14);
    memcpy(halves + 14, g + 16, 14);
    u32 crc = sim_crc32(halves, 28);
    g[14] = (u8)(crc >> 24);
    g[15] = (u8)(crc >> 16);
    g[30] = (u8)(crc >> 8);
    g[31] = (u8)crc;
    memset(sim_ext, 0xFF, sizeof sim_ext);
    u32 nsum = 0;
    for (int i = 0; i < 14; i++) nsum += nun[i];
    memcpy(sim_ext + 0x20, nun, 14);
    sim_ext[0x2E] = (u8)(nsum + 0x55);
    sim_ext[0x2F] = (u8)(nsum + 0xAA);
    const u8 nun_id[6] = {0x00, 0x00, 0xA4, 0x20, 0x00, 0x00};
    memcpy(sim_ext + 0xFA, nun_id, 6);
    sim_nunchuk = nunchuk;
}

static void sim_status(void) {
    u8 r[REMOTE_REPORT_SIZE] = {0x20};
    r[3] = (u8)((sim_leds << 4) | ((sim_mp_on || sim_nunchuk) ? 2u : 0u));
    r[6] = 0xC0;
    sim_push(r);
    sim_streaming = 0; /* until the mode is set again */
}

static void sim_ack(u8 id, u8 error) {
    u8 r[REMOTE_REPORT_SIZE] = {0x22};
    r[3] = id;
    r[4] = error;
    sim_push(r);
}

static const u8* sim_region(u32 space, u32 address) {
    u32 device = (address >> 16) & 0xFFu, offset = address & 0xFFu;
    if (space == 0) return (address & 0xFFFFu) < sizeof sim_eeprom ? sim_eeprom + (address & 0xFFFFu) : NULL;
    if (device == 0xA6 && !sim_mp_on) return sim_mp_off + offset;
    if (device == 0xA4 && sim_mp_on) {
        if ((LONG)(GetTickCount() - sim_mp_ready_at) < 0) return NULL; /* still coming up: an error, as a real one */
        u8 id[6] = {0x00, 0x00, 0xA4, 0x20, (u8)sim_mp_mode, 0x05};
        memcpy(sim_mp_on_regs + 0xFA, id, 6);
        return sim_mp_on_regs + offset;
    }
    if (device == 0xA4 && sim_nunchuk) return sim_ext + offset;
    return NULL;
}

static void sim_output(const u8* r, u32 size) {
    EnterCriticalSection(&sim_lock);
    switch (r[0]) {
    case 0x11:
        sim_leds = r[1] >> 4;
        break;
    case 0x12:
        sim_mode = r[2];
        sim_streaming = 1;
        break;
    case 0x15:
        sim_status();
        break;
    case 0x17: {
        u32 address = (u32)(r[2] << 16 | r[3] << 8 | r[4]), want = (u32)(r[5] << 8 | r[6]);
        const u8* from = sim_region(r[1] & 4u ? 4u : 0u, address);
        for (u32 done = 0; done < want || (!from && done == 0);) {
            u8 out[REMOTE_REPORT_SIZE] = {0x21};
            u32 step = want - done > 16 ? 16 : want - done;
            out[4] = (u8)((address + done) >> 8);
            out[5] = (u8)(address + done);
            if (!from) {
                out[3] = 7;
                sim_push(out);
                break;
            }
            out[3] = (u8)((step - 1) << 4);
            memcpy(out + 6, from + done, step);
            sim_push(out);
            done += step;
        }
        break;
    }
    case 0x16: {
        u32 address = (u32)(r[2] << 16 | r[3] << 8 | r[4]) & 0xFFFFFFu;
        u8 value = r[6];
        sim_ack(0x16, 0);
        if (address == 0xA600FEu && !sim_mp_on) {
            sim_mp_on = 1;
            sim_mp_mode = value;
            sim_mp_ready_at = GetTickCount() + SIM_MP_START_MS;
            sim_status();
        } else if (address == 0xA400F0u && value == 0x55 && sim_mp_on) {
            sim_mp_on = 0;
            sim_status();
        }
        break;
    }
    default:
        if (size > 1 && (r[1] & 2)) sim_ack(r[0], 0);
        break;
    }
    (void)size;
    LeaveCriticalSection(&sim_lock);
}

static DWORD sim_unplug_at; /* a tick count: the simulated Nunchuk comes out then */
static float sim_still = 45.0f; /* seconds before it starts to move */

void real_remote_sim_still_for(uint32_t seconds) { sim_still = (float)seconds; }

static DWORD WINAPI sim_stream(void* unused) {
    (void)unused;
    for (u32 n = 0;; n++) {
        Sleep(6);
        EnterCriticalSection(&sim_lock);
        if (sim_unplug_at && sim_nunchuk && (LONG)(GetTickCount() - sim_unplug_at) >= 0) {
            sim_nunchuk = 0; /* with MotionPlus on, only its data's extension bit says so */
            fprintf(report, "Wii Remote: simulated Nunchuk taken out\n");
        }
        if (sim_streaming && sim_mode == 0x37) {
            u8 r[REMOTE_REPORT_SIZE] = {0x37};
            float t = (float)n * 0.006f;
            u32 ax = 512, ay = (u32)(512 + (t < sim_still ? 0.0f : 40 * sinf(t * 3.0f))), az = 616;
            r[1] = (u8)((ax & 3) << 5);
            r[2] = (u8)(((ay >> 1) & 1) << 5 | ((az >> 1) & 1) << 6);
            r[3] = (u8)(ax >> 2);
            r[4] = (u8)(ay >> 2);
            r[5] = (u8)(az >> 2);
            memset(r + 6, 0xFF, 10);
            u8* e = r + 16;
            if (sim_mp_on && !(sim_mp_mode == 5 && sim_nunchuk && (sim_turn++ & 1))) {
                /* Still for the first 45 seconds, as a remote set down while the game checks its MotionPlus. */
                float sway = t < sim_still ? 0.0f : 1000.0f * sinf(t * 2.0f);
                u32 yaw = (u32)(8192 + sway), roll = 8100, pitch = 8250;
                e[0] = (u8)yaw;
                e[1] = (u8)roll;
                e[2] = (u8)pitch;
                e[3] = (u8)((yaw >> 8) << 2 | 3);
                e[4] = (u8)((roll >> 8) << 2 | 2 | (sim_nunchuk ? 1 : 0));
                e[5] = (u8)((pitch >> 8) << 2 | 2);
            } else if (sim_nunchuk) {
                u32 nx = 512, ny = 512, nz = 716;
                e[0] = 0xC0;
                e[1] = 0x80;
                e[2] = (u8)(nx >> 2);
                e[3] = (u8)(ny >> 2);
                if (sim_mp_on) {
                    e[4] = (u8)((nz >> 3) << 1 | 1);
                    e[5] = (u8)(((nz >> 1) & 3) << 6 | ((ny >> 1) & 1) << 5 | ((nx >> 1) & 1) << 4 | 8 | 4);
                } else {
                    e[4] = (u8)(nz >> 2);
                    e[5] = (u8)((nz & 3) << 6 | (ny & 3) << 4 | (nx & 3) << 2 | 2 | 1);
                }
            }
            sim_push(r);
        }
        LeaveCriticalSection(&sim_lock);
    }
    return 0;
}

void real_remote_unplug_simulated_after(u32 ms) { sim_unplug_at = GetTickCount() + ms; }

void real_remote_start_simulated(int nunchuk) {
    if (started) return;
    InitializeCriticalSection(&sim_lock);
    start_slots(0);
    sim_setup(nunchuk);
    simulated = 1;
    slots[0].connected = 1;
    CreateThread(NULL, 0, sim_stream, NULL, 0, NULL);
    fprintf(report, "Wii Remote: simulated, Nunchuk %s\n", nunchuk ? "in" : "out");
}
