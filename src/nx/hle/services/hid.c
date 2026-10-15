/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* hid. Games read the shared memory directly, so we fill it in the real layout (17 entry LIFOs). */
#include "hle/services/hid.h"
#include "hle/services/services.h"
#include "hle/kernel/ipc.h"
#include "hle/kernel/kernel.h"
#include <string.h>
#include "hle/services/hid_layout.h"
#include "hle/services/services_int.h"
#include "hle/services/hid.swipc.inc"

PadState g_pad;
static struct {
    KShmem* shm;
    u64 samp;
    u32 style_supported;
    KEvent* style_ev[10]; /* AcquireNpadStyleSetUpdateEventHandle, per npad index */
    /* the legacy lifos (GetJoyXpadLifoHandle, GetSixAxisSensorLifoHandle, GetJoySixAxisSensorLifoHandle): one shared
       memory each, as the console gives the same one on every call. Nothing writes them */
    KShmem* xpad_shm;
    KShmem* six_shm;
    KShmem* joysix_shm;
    u64 hold_type, comm_mode, handheld_mode; /* set and read by game threads: atomics */
} hid = {.style_supported = 0x7F};

static void lifo_push(u8* lifo, u32 esize, const void* state, u32 ssize, u64 samp) {
    /* lifo header: unused, buffer_count, tail, count. entries: {u64 sampling_number; state} */
    u64 cnt = 17, tail, count;
    memcpy(&tail, lifo + 0x10, 8);
    memcpy(&count, lifo + 0x18, 8);
    tail = (tail + 1) % cnt;
    u8* e = lifo + 0x20 + tail * esize;
    memcpy(e, &samp, 8);
    memcpy(e + 8, state, ssize);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy(lifo + 8, &cnt, 8);
    memcpy(lifo + 0x10, &tail, 8);
    if (count < 16) count++;
    memcpy(lifo + 0x18, &count, 8);
}

/* npad index: 0..7 = No1..No8, 8 = Handheld (0x20), 9 = Other (0x10). footer: AppletFooterUiType */
static void npad_setup(u8* base, int idx, u32 style, u32 devtype, u8 footer) {
    u8* e = base + HO_HidSharedMemory_npad + idx * HS_HidNpadSharedMemoryEntry;
    memcpy(e + HO_HidNpadInternalState_style_set, &style, 4);
    u32 jam = 0;
    memcpy(e + HO_HidNpadInternalState_joy_assignment_mode, &jam, 4);
    /* color attribute 0 = Ok (1 is ReadError) */
    u32 col[3] = {0, 0xFF323232u, 0xFF0A0A0A};
    memcpy(e + HO_HidNpadInternalState_full_key_color, col, 12);
    u32 jc[5] = {0, 0xFF3C3C3Cu, 0xFF0A0A0A, 0xFF3C3C3Cu, 0xFF0A0A0A};
    memcpy(e + HO_HidNpadInternalState_joy_color, jc, 20);
    memcpy(e + HO_HidNpadInternalState_device_type, &devtype, 4);
    /* IsCharging JoyDual/Left/Right, IsPowered JoyDual, IsAbxyButtonOriented, IsSlSrButtonOriented,
       IsPlusAvailable, IsMinusAvailable */
    u64 props = (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 3) | (1ull << 11) | (1ull << 12) | (1ull << 13) |
                (1ull << 14);
    memcpy(e + HO_HidNpadInternalState_system_properties, &props, 8);
    u32 bat[3] = {4, 4, 4};
    memcpy(e + HO_HidNpadInternalState_battery_level, bat, 12);
    u32 fattr = 0;
    memcpy(e + HO_HidNpadInternalState_applet_footer_ui_attribute, &fattr, 4);
    e[HO_HidNpadInternalState_applet_footer_ui_type] = footer;
}

void hid_update(void) {
    if (!hid.shm) return;
    u8* b = kshmem_ptr(hid.shm);
    hid.samp++;
    u8 st[0x28];
    memset(st, 0, sizeof st);
    u64 btn = g_pad.buttons;
    /* sticks also set the stick-direction button bits, like hw */
    if (g_pad.lx < -16000) btn |= 1ull << 16;
    if (g_pad.ly > 16000) btn |= 1ull << 17;
    if (g_pad.lx > 16000) btn |= 1ull << 18;
    if (g_pad.ly < -16000) btn |= 1ull << 19;
    if (g_pad.rx < -16000) btn |= 1ull << 20;
    if (g_pad.ry > 16000) btn |= 1ull << 21;
    if (g_pad.rx > 16000) btn |= 1ull << 22;
    if (g_pad.ry < -16000) btn |= 1ull << 23;
    memcpy(st + HO_HidNpadCommonState_sampling_number, &hid.samp, 8);
    memcpy(st + HO_HidNpadCommonState_buttons, &btn, 8);
    i32 s4[4] = {g_pad.lx, g_pad.ly, g_pad.rx, g_pad.ry};
    memcpy(st + HO_HidNpadCommonState_analog_stick_l, s4, 16);
    u32 attr = 1;
    memcpy(st + HO_HidNpadCommonState_attributes, &attr, 4); /* IsConnected */
    /* No1 = pro / dual joycon, Handheld = attached */
    u8* e0 = b + HO_HidSharedMemory_npad;
    lifo_push(e0 + HO_HidNpadInternalState_full_key_lifo, HS_HidNpadCommonStateAtomicStorage, st, 0x28, hid.samp);
    lifo_push(e0 + HO_HidNpadInternalState_joy_dual_lifo, HS_HidNpadCommonStateAtomicStorage, st, 0x28, hid.samp);
    lifo_push(e0 + HO_HidNpadInternalState_handheld_lifo, HS_HidNpadCommonStateAtomicStorage, st, 0x28, hid.samp);
    u8* eh = b + HO_HidSharedMemory_npad + 8 * HS_HidNpadSharedMemoryEntry;
    lifo_push(eh + HO_HidNpadInternalState_handheld_lifo, HS_HidNpadCommonStateAtomicStorage, st, 0x28, hid.samp);
    lifo_push(eh + HO_HidNpadInternalState_full_key_lifo, HS_HidNpadCommonStateAtomicStorage, st, 0x28, hid.samp);
    /* unused npads still need a sampling number, the SDK looks at it */
    u8 z[0x28];
    memset(z, 0, sizeof z);
    memcpy(z, &hid.samp, 8);
    for (int i = 1; i < 10; i++) {
        if (i == 8) continue;
        u8* e = b + HO_HidSharedMemory_npad + i * HS_HidNpadSharedMemoryEntry;
        lifo_push(e + HO_HidNpadInternalState_full_key_lifo, HS_HidNpadCommonStateAtomicStorage, z, 0x28, hid.samp);
    }
    /* six-axis: at rest, gravity down */
    u8 six[HS_HidSixAxisSensorState]; /* {u64 delta_time; u64 sampling_number; ...}: no motion, so no delta */
    memset(six, 0, sizeof six);
    memcpy(six + 8, &hid.samp, 8);
    float acc[3] = {0, 0, -1};
    memcpy(six + HO_HidSixAxisSensorState_acceleration, acc, 12);
    float dir[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    memcpy(six + HO_HidSixAxisSensorState_direction, dir, 36);
    u32 sattr = 1;
    memcpy(six + HO_HidSixAxisSensorState_attributes, &sattr, 4);
    lifo_push(e0 + HO_HidNpadInternalState_full_key_six_axis_sensor_lifo, HS_HidSixAxisSensorStateAtomicStorage, six,
              sizeof six, hid.samp);
    lifo_push(e0 + HO_HidNpadInternalState_joy_dual_left_six_axis_sensor_lifo, HS_HidSixAxisSensorStateAtomicStorage,
              six, sizeof six, hid.samp);
    lifo_push(e0 + HO_HidNpadInternalState_joy_dual_right_six_axis_sensor_lifo, HS_HidSixAxisSensorStateAtomicStorage,
              six, sizeof six, hid.samp);
    lifo_push(eh + HO_HidNpadInternalState_handheld_six_axis_sensor_lifo, HS_HidSixAxisSensorStateAtomicStorage, six,
              sizeof six, hid.samp);
    /* touch screen: no touches, only the counters */
    u8 ts[HS_HidTouchScreenState];
    memset(ts, 0, sizeof ts);
    memcpy(ts, &hid.samp, 8);
    lifo_push(b + HO_HidSharedMemory_touchscreen + HO_HidTouchScreenSharedMemoryFormat_lifo,
              HS_HidTouchScreenStateAtomicStorage, ts, sizeof ts, hid.samp);
    /* keyboard/mouse/debug pad: only bump the counters */
    u8 kb[HS_HidKeyboardState];
    memset(kb, 0, sizeof kb);
    memcpy(kb, &hid.samp, 8);
    lifo_push(b + HO_HidSharedMemory_keyboard, 8 + HS_HidKeyboardState, kb, sizeof kb, hid.samp);
    u8 ms[HS_HidMouseState];
    memset(ms, 0, sizeof ms);
    memcpy(ms, &hid.samp, 8);
    lifo_push(b + HO_HidSharedMemory_mouse, 8 + HS_HidMouseState, ms, sizeof ms, hid.samp);
    u8 dp[HS_HidDebugPadState];
    memset(dp, 0, sizeof dp);
    memcpy(dp, &hid.samp, 8);
    lifo_push(b + HO_HidSharedMemory_debug_pad, 8 + HS_HidDebugPadState, dp, sizeof dp, hid.samp);
}

static int nn_hid_IAppletResource_GetSharedMemoryHandle(SvcObj* self, u32* shared_memory) {
    *shared_memory = h_new(&hid.shm->o);
    return 0;
}
/* No1 reports ONE style like real hardware (the SDK asserts on multi-style sets, e.g. in GetVibrationDeviceHandles):
   Pro Controller if the game supports it, else dual Joy-Con, else nothing (a game that takes only the handheld
   style finds No1 disconnected). Input goes to every lifo, so either one reads the pad */
static void npad1_setup(void) {
    u8* b = kshmem_ptr(hid.shm);
    u32 styles = __atomic_load_n(&hid.style_supported, __ATOMIC_RELAXED);
    if (styles & 1u)
        npad_setup(b, 0, 1u << 0, 1u << 0, 12); /* FullKey, footer SwitchProController */
    else if (styles & (1u << 2))
        npad_setup(b, 0, 1u << 2, (1u << 4) | (1u << 5), 5); /* JoyDual: JoyLeft + JoyRight, footer JoyDual */
    else
        npad_setup(b, 0, 0, 0, 0);
}
static int nn_hid_IHidServer_CreateAppletResource(SvcObj* self, u64 aruid, SvcObj** resource) {
    *resource = nn_hid_IAppletResource_new(0);
    return 0;
}
static int nn_hid_IHidServer_AcquireXpadIdEventHandle(SvcObj* self, u64 unknown, u32* event) {
    return new_event(event, "xpad id", 0);
}
static int nn_hid_IHidServer_GetJoyXpadLifoHandle(SvcObj* self, nn_hid_JoyXpadId id, u32* lifo) {
    *lifo = h_new(&hid.xpad_shm->o);
    return 0;
}
static int nn_hid_IHidServer_GetSixAxisSensorLifoHandle(SvcObj* self, nn_hid_BasicXpadId id, u32* lifo) {
    *lifo = h_new(&hid.six_shm->o);
    return 0;
}
static int nn_hid_IHidServer_GetJoySixAxisSensorLifoHandle(SvcObj* self, nn_hid_JoyXpadId id, u32* lifo) {
    *lifo = h_new(&hid.joysix_shm->o);
    return 0;
}
static int nn_hid_IHidServer_IsSixAxisSensorFusionEnabled(SvcObj* self, nn_hid_SixAxisSensorHandle handle, u64 aruid,
                                                          u8* enabled) {
    *enabled = 1;
    return 0;
}
static int nn_hid_IHidServer_IsSixAxisSensorAtRest(SvcObj* self, nn_hid_SixAxisSensorHandle handle, u64 aruid,
                                                   u8* at_rest) {
    *at_rest = 1;
    return 0;
}
/* No1's style can change, which its style event tells. The set and No1's shared memory change together: two game
   threads setting styles at once leave both on the same one */
static PMutex style_lock = PMUTEX_INIT;
static int nn_hid_IHidServer_SetSupportedNpadStyleSet(SvcObj* self, nn_hid_NpadStyleSet style_set, u64 aruid) {
    pm_lock(&style_lock);
    __atomic_store_n(&hid.style_supported, style_set, __ATOMIC_RELAXED);
    npad1_setup();
    pm_unlock(&style_lock);
    kevent_signal(hid.style_ev[0]);
    return 0;
}
static int nn_hid_IHidServer_GetSupportedNpadStyleSet(SvcObj* self, u64 aruid, nn_hid_NpadStyleSet* style_set) {
    *style_set = __atomic_load_n(&hid.style_supported, __ATOMIC_RELAXED);
    return 0;
}
static int nn_hid_IHidServer_AcquireNpadStyleSetUpdateEventHandle(SvcObj* self, nn_hid_NpadIdType id, u64 aruid,
                                                                  u64 event_ptr, u32* event) {
    int i = id == 0x20 ? 8 : id == 0x10 ? 9 : (int)(id & 7);
    *event = h_new(&hid.style_ev[i]->o);
    return 0;
}
/* the LEDs of players 1 to 8 */
static int nn_hid_IHidServer_GetPlayerLedPattern(SvcObj* self, nn_hid_NpadIdType id, u64* pattern) {
    static const u8 led[8] = {0x1, 0x3, 0x7, 0xF, 0x9, 0x5, 0xD, 0x6};
    *pattern = id < 8 ? led[id] : 0;
    return 0;
}
static int nn_hid_IHidServer_SetNpadJoyHoldType(SvcObj* self, u64 aruid, nn_hid_NpadJoyHoldType hold_type) {
    __atomic_store_n(&hid.hold_type, (u64)hold_type, __ATOMIC_RELAXED);
    return 0;
}
static int nn_hid_IHidServer_GetNpadJoyHoldType(SvcObj* self, u64 aruid, nn_hid_NpadJoyHoldType* hold_type) {
    *hold_type = (i64)__atomic_load_n(&hid.hold_type, __ATOMIC_RELAXED);
    return 0;
}
static int nn_hid_IHidServer_SetNpadHandheldActivationMode(SvcObj* self, u64 aruid,
                                                           nn_hid_NpadHandheldActivationMode mode) {
    __atomic_store_n(&hid.handheld_mode, (u64)mode, __ATOMIC_RELAXED);
    return 0;
}
static int nn_hid_IHidServer_GetNpadHandheldActivationMode(SvcObj* self, u64 aruid,
                                                           nn_hid_NpadHandheldActivationMode* mode) {
    *mode = (i64)__atomic_load_n(&hid.handheld_mode, __ATOMIC_RELAXED);
    return 0;
}
/* an LRA, on the left or the right */
static int nn_hid_IHidServer_GetVibrationDeviceInfo(SvcObj* self, nn_hid_VibrationDeviceHandle handle,
                                                    nn_hid_VibrationDeviceInfo* info) {
    info->type = 1;
    info->position = ((handle >> 16) & 0xFF) == 0 ? 1 : 2;
    return 0;
}
static int nn_hid_IHidServer_GetActualVibrationValue(SvcObj* self, nn_hid_VibrationDeviceHandle handle, u64 aruid,
                                                     nn_hid_VibrationValue* value) {
    value->amp_low = 0.f;
    value->freq_low = 160.f;
    value->amp_high = 0.f;
    value->freq_high = 320.f;
    return 0;
}
static int nn_hid_IHidServer_CreateActiveVibrationDeviceList(SvcObj* self, SvcObj** list) {
    *list = nn_hid_IActiveVibrationDeviceList_new(0);
    return 0;
}
static int nn_hid_IHidServer_IsVibrationPermitted(SvcObj* self, u8* permitted) {
    *permitted = 1;
    return 0;
}
static int nn_hid_IHidServer_IsVibrationDeviceMounted(SvcObj* self, nn_hid_VibrationDeviceHandle handle, u64 aruid,
                                                      u8* mounted) {
    *mounted = 1;
    return 0;
}
static int nn_hid_IHidServer_SetNpadCommunicationMode(SvcObj* self, u64 aruid, nn_hid_NpadCommunicationMode mode) {
    __atomic_store_n(&hid.comm_mode, (u64)mode, __ATOMIC_RELAXED);
    return 0;
}
static int nn_hid_IHidServer_GetNpadCommunicationMode(SvcObj* self, nn_hid_NpadCommunicationMode* mode) {
    *mode = (i64)__atomic_load_n(&hid.comm_mode, __ATOMIC_RELAXED);
    return 0;
}
static SvcObj* new_hid(void) { return nn_hid_IHidServer_new(0); }
static SvcObj* new_hidsys(void) { return nn_hid_IHidSystemServer_new(0); }

/* the service keeps these for the whole run: its reference keeps them alive when the game closes its handles */
static KShmem* owned_shm(u64 size, const char* name) {
    KShmem* s = kshmem_new(size, name);
    kobj_ref(&s->o);
    return s;
}
void svc_hid_init(void) {
    hid.shm = owned_shm(0x40000, "hid");
    hid.xpad_shm = owned_shm(0x1000, "hid xpad lifo");
    hid.six_shm = owned_shm(0x1000, "hid six-axis lifo");
    hid.joysix_shm = owned_shm(0x1000, "hid joy six-axis lifo");
    /* the style events start signaled, so the game reads the styles the first time it waits */
    for (int i = 0; i < 10; i++) {
        hid.style_ev[i] = owned_event("npad style");
        kevent_signal(hid.style_ev[i]);
    }
    u8* b = kshmem_ptr(hid.shm);
    npad1_setup();
    npad_setup(b, 8, 1u << 1, (1u << 2) | (1u << 3), 4); /* Handheld, footer HandheldJoyConLeftJoyConRight */
    hid_update();
    hid_update();
    nn_hid_IHidServer_register(new_hid);
    nn_hid_IHidSystemServer_register(new_hidsys);
}
