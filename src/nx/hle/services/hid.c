/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* hid. Games read the shared memory directly, so we fill it in the real layout (17 entry LIFOs). */
#include "nx/hle/services/hid.h"
#include "nx/hle/services/hid_motion.h"
#include "nx/hle/services/services.h"
#include "nx/hle/kernel/ipc.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/platform/plat.h"
#include <string.h>
#include "nx/hle/services/hid_layout.h"
#include "nx/hle/services/services_int.h"
#include "hle/services/hid.swipc.inc"

PadState g_pad;
PadState g_pad_extra[7];
PMutex g_pad_input_lock = PMUTEX_INIT;
TouchState g_touch;
static struct {
    KShmem* shm;
    u64 samp;
    u64 last_motion_ns;
    HidMotionState motion_state[8][2];
    u64 motion_source[8][2];
    HidFingers fingers;
    u32 style_supported;
    u32 id_supported; /* No1..No8 and Handheld, one bit per shared-memory index */
    u32 visible_style[10];
    u64 visible_hold_type;
    u8 joy_assignment[8];              /* 0 = physical style, 1 = left only, 2 = right only */
    i8 split_parent[8], split_dest[8]; /* virtual single Joy-Con and its source; -1 means none */
    u32 split_source[8];               /* physical instance that was split, indexed by parent */
    i8 paired[8];                      /* a paired left/right source, -2 on the hidden secondary, -1 when unpaired */
    u32 paired_source[8];              /* physical instance at pairing time, for each occupied slot */
    KEvent* style_ev[10];              /* AcquireNpadStyleSetUpdateEventHandle, per npad index */
    /* the legacy lifos (GetJoyXpadLifoHandle, GetSixAxisSensorLifoHandle, GetJoySixAxisSensorLifoHandle): one shared
       memory each, as the console gives the same one on every call. Nothing writes them */
    KShmem* xpad_shm;
    KShmem* six_shm;
    KShmem* joysix_shm;
    u64 hold_type, comm_mode, handheld_mode; /* set and read by game threads: atomics */
} hid = {.style_supported = 0x7F, .id_supported = 0x1FF};

static PMutex style_lock = PMUTEX_INIT;
static void input_style_lock(void) {
    pm_lock(&g_pad_input_lock);
    pm_lock(&style_lock);
}
static void input_style_unlock(void) {
    pm_unlock(&style_lock);
    pm_unlock(&g_pad_input_lock);
}

static PadState* player_pad(int idx) { return idx ? &g_pad_extra[idx - 1] : &g_pad; }

static u32 npad_style(int idx) {
    const PadState* pad = player_pad(idx);
    u32 supported = __atomic_load_n(&hid.style_supported, __ATOMIC_RELAXED);
    u32 ids = __atomic_load_n(&hid.id_supported, __ATOMIC_RELAXED);
    u32 style = 0;
    if (!(ids & (1u << idx))) return 0;
    if (hid.split_parent[idx] >= 0) {
        int parent = hid.split_parent[idx];
        const PadState* source = player_pad(parent);
        u32 other_style = hid.joy_assignment[parent] == 1 ? 1u << 4 : 1u << 3;
        if (!__atomic_load_n(&source->connected, __ATOMIC_RELAXED) ||
            __atomic_load_n(&pad->connected, __ATOMIC_RELAXED))
            return 0;
        return supported & other_style ? other_style : 0;
    }
    if (!__atomic_load_n(&pad->connected, __ATOMIC_RELAXED) || hid.paired[idx] == -2) return 0;
    if (hid.paired[idx] >= 0) {
        const PadState* other = player_pad(hid.paired[idx]);
        if (__atomic_load_n(&other->connected, __ATOMIC_RELAXED)) style = supported & (1u << 2);
        if (style) return style;
    }
    switch (__atomic_load_n(&pad->kind, __ATOMIC_RELAXED)) {
        case SNX_PAD_JOY_LEFT: style = supported & (1u << 3); break;
        case SNX_PAD_JOY_RIGHT: style = supported & (1u << 4); break;
        case SNX_PAD_JOY_DUAL:
            if (hid.joy_assignment[idx] == 1)
                style = supported & (1u << 3);
            else if (hid.joy_assignment[idx] == 2)
                style = supported & (1u << 4);
            else
                style = supported & (1u << 2);
            break;
        default:
            style = supported & 1u ? 1u : supported & (1u << 2); /* legacy full-pad fallback */
            break;
    }
    /* Keyboard and scripted input still drive No1 when its physical Joy-Con style is unsupported. */
    return style ? style : idx == 0 ? supported & 1u : 0;
}

static void lifo_push(u8* lifo, u32 esize, const void* state, u32 ssize, u64 samp) {
    /* lifo header: unused, buffer_count, tail, count. The entry's atomic sampling number is a
       sequence lock: odd while writing, even when the state is ready. The state has its own sample number. */
    u64 cnt = 17, tail, count;
    memcpy(&tail, lifo + 0x10, 8);
    memcpy(&count, lifo + 0x18, 8);
    tail = (tail + 1) % cnt;
    u8* e = lifo + 0x20 + tail * esize;
    __atomic_store_n((u64*)e, (samp << 1) | 1, __ATOMIC_SEQ_CST);
    memcpy(e + 8, state, ssize);
    __atomic_store_n((u64*)e, samp << 1, __ATOMIC_RELEASE);
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
    u32 jam = style == (1u << 3) || style == (1u << 4) ? 1 : 0;
    memcpy(e + HO_HidNpadInternalState_joy_assignment_mode, &jam, 4);
    /* color attribute 0 = Ok (1 is ReadError) */
    u32 col[3] = {0, 0xFF323232u, 0xFF0A0A0A};
    memcpy(e + HO_HidNpadInternalState_full_key_color, col, 12);
    u32 jc[5] = {0, 0xFF3C3C3Cu, 0xFF0A0A0A, 0xFF3C3C3Cu, 0xFF0A0A0A};
    memcpy(e + HO_HidNpadInternalState_joy_color, jc, 20);
    memcpy(e + HO_HidNpadInternalState_device_type, &devtype, 4);
    /* Orientation and available system buttons follow the one style this Npad reports. */
    u64 props = style ? (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 3) : 0;
    if (style == (1u << 3) || style == (1u << 4))
        props |= 1ull << (__atomic_load_n(&hid.hold_type, __ATOMIC_RELAXED) ? 12 : 11);
    else if (style)
        props |= 1ull << 11;
    if (style && style != (1u << 3)) props |= 1ull << 13; /* Plus */
    if (style && style != (1u << 4)) props |= 1ull << 14; /* Minus */
    memcpy(e + HO_HidNpadInternalState_system_properties, &props, 8);
    u32 bat[3] = {4, 4, 4};
    memcpy(e + HO_HidNpadInternalState_battery_level, bat, 12);
    u32 fattr = 0;
    memcpy(e + HO_HidNpadInternalState_applet_footer_ui_attribute, &fattr, 4);
    e[HO_HidNpadInternalState_applet_footer_ui_type] = footer;
}

static void npad_refresh_locked(void) {
    u8* base = kshmem_ptr(hid.shm);
    u64 hold = __atomic_load_n(&hid.hold_type, __ATOMIC_RELAXED);
    u32 ids = __atomic_load_n(&hid.id_supported, __ATOMIC_RELAXED);
    int hold_changed = hold != hid.visible_hold_type;
    int i;
    /* A hot-plugged physical pad takes precedence over a virtual split destination. */
    for (i = 0; i < 8; i++)
        if (hid.split_parent[i] >= 0 &&
            (!(ids & (1u << i)) || !(ids & (1u << hid.split_parent[i])) ||
             player_pad(hid.split_parent[i])->source_id != hid.split_source[hid.split_parent[i]] ||
             __atomic_load_n(&player_pad(i)->connected, __ATOMIC_RELAXED) ||
             !__atomic_load_n(&player_pad(hid.split_parent[i])->connected, __ATOMIC_RELAXED) ||
             __atomic_load_n(&player_pad(hid.split_parent[i])->kind, __ATOMIC_RELAXED) != SNX_PAD_JOY_DUAL)) {
            int parent = hid.split_parent[i];
            hid.split_parent[i] = -1;
            hid.split_dest[parent] = -1;
            hid.joy_assignment[parent] = 0;
        }
    for (i = 0; i < 8; i++)
        if (hid.paired[i] >= 0) {
            int other = hid.paired[i];
            PadKind a = __atomic_load_n(&player_pad(i)->kind, __ATOMIC_RELAXED);
            PadKind b = __atomic_load_n(&player_pad(other)->kind, __ATOMIC_RELAXED);
            if (!__atomic_load_n(&player_pad(i)->connected, __ATOMIC_RELAXED) ||
                !__atomic_load_n(&player_pad(other)->connected, __ATOMIC_RELAXED) ||
                player_pad(i)->source_id != hid.paired_source[i] ||
                player_pad(other)->source_id != hid.paired_source[other] ||
                !((a == SNX_PAD_JOY_LEFT && b == SNX_PAD_JOY_RIGHT) ||
                  (a == SNX_PAD_JOY_RIGHT && b == SNX_PAD_JOY_LEFT)))
                hid.paired[i] = hid.paired[other] = -1;
        }
    for (i = 0; i < 8; i++) {
        u32 style = npad_style(i), devtype = 0;
        u8 footer = 0;
        switch (style) {
            case 1u << 0:
                devtype = 1u << 0;
                footer = 12;
                break;
            case 1u << 2:
                devtype = (1u << 4) | (1u << 5);
                footer = 5;
                break;
            case 1u << 3:
                devtype = 1u << 4;
                footer = __atomic_load_n(&hid.hold_type, __ATOMIC_RELAXED) ? 8 : 9;
                break;
            case 1u << 4:
                devtype = 1u << 5;
                footer = __atomic_load_n(&hid.hold_type, __ATOMIC_RELAXED) ? 10 : 11;
                break;
        }
        if (hid.visible_style[i] != style || (hold_changed && (style == (1u << 3) || style == (1u << 4))))
            npad_setup(base, i, style, devtype, footer);
        if (hid.visible_style[i] != style) {
            hid.visible_style[i] = style;
            if (hid.style_ev[i]) kevent_signal(hid.style_ev[i]);
        }
    }
    {
        u32 supported = __atomic_load_n(&hid.style_supported, __ATOMIC_RELAXED);
        u32 ids = __atomic_load_n(&hid.id_supported, __ATOMIC_RELAXED);
        u32 style = !g_opt_docked && (supported & (1u << 1)) && (ids & (1u << 8)) ? 1u << 1 : 0;
        if (hid.visible_style[8] != style) npad_setup(base, 8, style, style ? (1u << 2) | (1u << 3) : 0, style ? 4 : 0);
        if (hid.visible_style[8] != style) {
            hid.visible_style[8] = style;
            if (hid.style_ev[8]) kevent_signal(hid.style_ev[8]);
        }
    }
    hid.visible_hold_type = hold;
}

static void npad_input(int idx, PadState* out) {
    static const u64 left_bits =
        (1ull << 4) | (1ull << 6) | (1ull << 8) | (1ull << 11) | (15ull << 12) | (15ull << 16) | (3ull << 24);
    static const u64 right_bits =
        15ull | (1ull << 5) | (1ull << 7) | (1ull << 9) | (1ull << 10) | (15ull << 20) | (3ull << 26);
    int parent = hid.split_parent[idx];
    int horizontal = __atomic_load_n(&hid.hold_type, __ATOMIC_RELAXED) != 0;
    const PadState* source = player_pad(parent >= 0 ? parent : idx);
    out->buttons = source->buttons;
    out->lx = source->lx;
    out->ly = source->ly;
    out->rx = source->rx;
    out->ry = source->ry;
    out->connected = __atomic_load_n(&source->connected, __ATOMIC_RELAXED);
    out->kind = __atomic_load_n(&source->kind, __ATOMIC_RELAXED);
    out->source_id = source->source_id;
    memcpy(out->motion, source->motion, sizeof out->motion);
    out->rumble[0] = out->rumble[1] = 0;
    if (parent >= 0 || hid.joy_assignment[idx]) {
        u32 style = hid.visible_style[idx];
        if (style == (1u << 3)) {
            out->buttons &= left_bits;
            out->rx = out->ry = 0;
            out->motion[1].valid = 0;
        } else if (style == (1u << 4)) {
            out->buttons &= right_bits;
            out->lx = out->ly = 0;
            out->motion[0] = out->motion[1].valid ? out->motion[1] : out->motion[0];
            out->motion[1].valid = 0;
        }
    }
    if (hid.visible_style[idx] == (1u << 3) || hid.visible_style[idx] == (1u << 4)) {
        /* SDL exposes a separate Joy-Con as a horizontal mini-gamepad. A combined pair's sticks are vertical. */
        if (out->kind == SNX_PAD_JOY_LEFT && !horizontal) {
            i32 x = out->lx, y = out->ly;
            out->lx = y;
            out->ly = -x;
        } else if (out->kind == SNX_PAD_JOY_RIGHT && !horizontal) {
            i32 x = out->rx, y = out->ry;
            out->rx = -y;
            out->ry = x;
        } else if (out->kind == SNX_PAD_JOY_DUAL && horizontal && hid.visible_style[idx] == (1u << 3)) {
            i32 x = out->lx, y = out->ly;
            out->lx = -y;
            out->ly = x;
        } else if (out->kind == SNX_PAD_JOY_DUAL && horizontal && hid.visible_style[idx] == (1u << 4)) {
            i32 x = out->rx, y = out->ry;
            out->rx = y;
            out->ry = -x;
        }
    }
    if (hid.paired[idx] >= 0) {
        const PadState* a = player_pad(idx);
        const PadState* b = player_pad(hid.paired[idx]);
        const PadState* left = __atomic_load_n(&a->kind, __ATOMIC_RELAXED) == SNX_PAD_JOY_LEFT ? a : b;
        const PadState* right = __atomic_load_n(&a->kind, __ATOMIC_RELAXED) == SNX_PAD_JOY_RIGHT ? a : b;
        out->buttons = (left->buttons & left_bits) | (right->buttons & right_bits);
        /* Each separate Joy-Con is an SDL horizontal mini-gamepad, even when the guest pairs them. */
        out->lx = left->ly;
        out->ly = -left->lx;
        out->rx = -right->ry;
        out->ry = right->rx;
        out->motion[0] = left->motion[0];
        out->motion[1] = right->motion[1].valid ? right->motion[1] : right->motion[0];
    }
}

/* The key follows the physical sensor through split and pair assignments. */
static u64 motion_source_key(int idx, int sensor) {
    int slot = hid.split_parent[idx] >= 0 ? hid.split_parent[idx] : idx;
    if (hid.paired[idx] >= 0) {
        int other = hid.paired[idx];
        PadKind kind = __atomic_load_n(&player_pad(slot)->kind, __ATOMIC_RELAXED);
        if ((sensor == 0 && kind != SNX_PAD_JOY_LEFT) || (sensor == 1 && kind != SNX_PAD_JOY_RIGHT)) slot = other;
    }
    const PadState* pad = player_pad(slot);
    int physical_sensor = sensor;
    if (__atomic_load_n(&pad->kind, __ATOMIC_RELAXED) == SNX_PAD_JOY_DUAL && sensor == 0 &&
        hid.visible_style[idx] == (1u << 4))
        physical_sensor = 1;
    if (hid.paired[idx] >= 0 || __atomic_load_n(&pad->kind, __ATOMIC_RELAXED) != SNX_PAD_JOY_DUAL) physical_sensor = 0;
    return ((u64)pad->source_id << 8) | ((u64)(slot + 1) << 2) | (u64)(physical_sensor + 1);
}

int hid_connected_players(u32* first_id) {
    int count = 0;
    input_style_lock();
    if (hid.shm) npad_refresh_locked();
    for (int i = 0; i < 8; i++) {
        if (!hid.visible_style[i]) continue;
        if (!count && first_id) *first_id = (u32)i;
        count++;
    }
    input_style_unlock();
    return count;
}

void hid_update(void) {
    if (!hid.shm) return;
    u8* b = kshmem_ptr(hid.shm);
    hid.samp++;
    /* A separate LIFO per player and style is what the SDK reads. Disconnected styles still advance their sampling
       numbers so callers never mistake an old pressed button for a current one. */
    u64 now_ns = plat_ns();
    u64 dt_ns = hid.last_motion_ns ? now_ns - hid.last_motion_ns : 0;
    u8 six_handheld[HS_HidSixAxisSensorState];
    hid.last_motion_ns = now_ns;
    input_style_lock();
    npad_refresh_locked();
    for (int i = 0; i < 8; i++) {
        PadState input;
        const PadState* pad = &input;
        npad_input(i, &input);
        u32 style = hid.visible_style[i];
        u8* e = b + HO_HidSharedMemory_npad + i * HS_HidNpadSharedMemoryEntry;
        u8 st[HS_HidNpadCommonState], z[HS_HidNpadCommonState];
        u8 six_left[HS_HidSixAxisSensorState], six_right[HS_HidSixAxisSensorState];
        u8 six_zero[HS_HidSixAxisSensorState];
        const PadMotion no_motion = {{0}, {0}, 0};
        const PadMotion* left = &pad->motion[0];
        const PadMotion* right = pad->motion[1].valid ? &pad->motion[1] : left;
        u64 btn = pad->buttons;
        u32 attr = 1;
        i32 sticks[4] = {pad->lx, pad->ly, pad->rx, pad->ry};
        memset(st, 0, sizeof st);
        memset(z, 0, sizeof z);
        memset(six_zero, 0, sizeof six_zero);
        memcpy(st, &hid.samp, 8);
        memcpy(z, &hid.samp, 8);
        memcpy(six_zero + 8, &hid.samp, 8);
        if (pad->lx < -16000) btn |= 1ull << 16;
        if (pad->ly > 16000) btn |= 1ull << 17;
        if (pad->lx > 16000) btn |= 1ull << 18;
        if (pad->ly < -16000) btn |= 1ull << 19;
        if (pad->rx < -16000) btn |= 1ull << 20;
        if (pad->ry > 16000) btn |= 1ull << 21;
        if (pad->rx > 16000) btn |= 1ull << 22;
        if (pad->ry < -16000) btn |= 1ull << 23;
        if (style == (1u << 2)) attr |= (1u << 2) | (1u << 4); /* both Joy-Cons */
        if (style == (1u << 3)) attr |= 1u << 2;
        if (style == (1u << 4)) attr |= 1u << 4;
        memcpy(st + HO_HidNpadCommonState_buttons, &btn, 8);
        memcpy(st + HO_HidNpadCommonState_analog_stick_l, sticks, 16);
        memcpy(st + HO_HidNpadCommonState_attributes, &attr, 4);
        lifo_push(e + HO_HidNpadInternalState_full_key_lifo, HS_HidNpadCommonStateAtomicStorage, style == 1u ? st : z,
                  sizeof st, hid.samp);
        lifo_push(e + HO_HidNpadInternalState_joy_dual_lifo, HS_HidNpadCommonStateAtomicStorage,
                  style == (1u << 2) ? st : z, sizeof st, hid.samp);
        lifo_push(e + HO_HidNpadInternalState_joy_left_lifo, HS_HidNpadCommonStateAtomicStorage,
                  style == (1u << 3) ? st : z, sizeof st, hid.samp);
        lifo_push(e + HO_HidNpadInternalState_joy_right_lifo, HS_HidNpadCommonStateAtomicStorage,
                  style == (1u << 4) ? st : z, sizeof st, hid.samp);
        for (int sensor = 0; sensor < 2; sensor++) {
            u64 key = motion_source_key(i, sensor);
            if (hid.motion_source[i][sensor] != key) {
                memset(&hid.motion_state[i][sensor], 0, sizeof hid.motion_state[i][sensor]);
                hid.motion_source[i][sensor] = key;
            }
        }
        hid_motion_sample(&hid.motion_state[i][0], style || (!i && hid.visible_style[8]) ? left : &no_motion, dt_ns,
                          hid.samp, six_left);
        hid_motion_sample(&hid.motion_state[i][1], style || (!i && hid.visible_style[8]) ? right : &no_motion, dt_ns,
                          hid.samp, six_right);
        if (!i) memcpy(six_handheld, left->valid ? six_left : six_right, sizeof six_handheld);
        lifo_push(e + HO_HidNpadInternalState_full_key_six_axis_sensor_lifo, HS_HidSixAxisSensorStateAtomicStorage,
                  style == 1u ? (left->valid ? six_left : six_right) : six_zero, sizeof six_left, hid.samp);
        lifo_push(e + HO_HidNpadInternalState_joy_dual_left_six_axis_sensor_lifo, HS_HidSixAxisSensorStateAtomicStorage,
                  style == (1u << 2) ? six_left : six_zero, sizeof six_left, hid.samp);
        lifo_push(e + HO_HidNpadInternalState_joy_dual_right_six_axis_sensor_lifo,
                  HS_HidSixAxisSensorStateAtomicStorage, style == (1u << 2) ? six_right : six_zero, sizeof six_right,
                  hid.samp);
        lifo_push(e + HO_HidNpadInternalState_joy_left_six_axis_sensor_lifo, HS_HidSixAxisSensorStateAtomicStorage,
                  style == (1u << 3) ? six_left : six_zero, sizeof six_left, hid.samp);
        lifo_push(e + HO_HidNpadInternalState_joy_right_six_axis_sensor_lifo, HS_HidSixAxisSensorStateAtomicStorage,
                  style == (1u << 4) ? six_right : six_zero, sizeof six_right, hid.samp);
    }
    /* The virtual handheld shares No1's keyboard and touch controls. */
    u8* eh = b + HO_HidSharedMemory_npad + 8 * HS_HidNpadSharedMemoryEntry;
    u8 handheld[HS_HidNpadCommonState];
    memset(handheld, 0, sizeof handheld);
    memcpy(handheld, &hid.samp, 8);
    if (hid.visible_style[8]) {
        u32 attr = 1u | (1u << 2) | (1u << 4);
        i32 sticks[4] = {g_pad.lx, g_pad.ly, g_pad.rx, g_pad.ry};
        memcpy(handheld + HO_HidNpadCommonState_buttons, &g_pad.buttons, 8);
        memcpy(handheld + HO_HidNpadCommonState_analog_stick_l, sticks, 16);
        memcpy(handheld + HO_HidNpadCommonState_attributes, &attr, 4);
    }
    lifo_push(eh + HO_HidNpadInternalState_handheld_lifo, HS_HidNpadCommonStateAtomicStorage, handheld, sizeof handheld,
              hid.samp);
    lifo_push(eh + HO_HidNpadInternalState_full_key_lifo, HS_HidNpadCommonStateAtomicStorage, handheld, sizeof handheld,
              hid.samp);
    if (!hid.visible_style[8]) {
        memset(six_handheld, 0, sizeof six_handheld);
        memcpy(six_handheld + 8, &hid.samp, 8);
    }
    lifo_push(eh + HO_HidNpadInternalState_handheld_six_axis_sensor_lifo, HS_HidSixAxisSensorStateAtomicStorage,
              six_handheld, sizeof six_handheld, hid.samp);
    input_style_unlock();
    /* touch screen: the fingers the frontend saw */
    u8 ts[HS_HidTouchScreenState];
    hid_touch_sample(&hid.fingers, &g_touch, hid.samp, ts);
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
    u32 id = (handle >> 8) & 0xFF;
    u32 device = (handle >> 16) & 0xFF;
    PadState input;
    const PadState* pad;
    const PadMotion* sensor;
    u32 style;
    input_style_lock();
    if (id < 8) {
        npad_input((int)id, &input); /* includes a virtual split or a paired physical Joy-Con */
        pad = &input;
        style = hid.visible_style[id];
    } else {
        pad = &g_pad;
        style = hid.visible_style[8];
    }
    sensor = &pad->motion[0];
    if ((style == (1u << 1) || style == (1u << 2)) && device == 1)
        sensor = pad->motion[1].valid ? &pad->motion[1] : sensor;
    else if (style == (1u << 4))
        sensor = pad->motion[1].valid ? &pad->motion[1] : sensor;
    else if (style == 1u && !sensor->valid)
        sensor = &pad->motion[1];
    *at_rest = (u8)hid_motion_at_rest(sensor);
    input_style_unlock();
    return 0;
}
static int nn_hid_IHidServer_SetSupportedNpadStyleSet(SvcObj* self, nn_hid_NpadStyleSet style_set, u64 aruid) {
    input_style_lock();
    __atomic_store_n(&hid.style_supported, style_set, __ATOMIC_RELAXED);
    npad_refresh_locked();
    input_style_unlock();
    return 0;
}
static int nn_hid_IHidServer_GetSupportedNpadStyleSet(SvcObj* self, u64 aruid, nn_hid_NpadStyleSet* style_set) {
    *style_set = __atomic_load_n(&hid.style_supported, __ATOMIC_RELAXED);
    return 0;
}
static int nn_hid_IHidServer_SetSupportedNpadIdType(SvcObj* self, u64 aruid, IpcBuf ids) {
    u32 mask = 0;
    u64 off;
    for (off = 0; off + 4 <= ids.size; off += 4) {
        u32 id;
        memcpy(&id, GP(ids.addr + off), 4);
        if (id < 8) mask |= 1u << id;
        if (id == 0x20) mask |= 1u << 8;
    }
    input_style_lock();
    __atomic_store_n(&hid.id_supported, mask, __ATOMIC_RELAXED);
    npad_refresh_locked();
    input_style_unlock();
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
    input_style_lock();
    __atomic_store_n(&hid.hold_type, (u64)hold_type, __ATOMIC_RELAXED);
    npad_refresh_locked();
    input_style_unlock();
    return 0;
}
static int nn_hid_IHidServer_GetNpadJoyHoldType(SvcObj* self, u64 aruid, nn_hid_NpadJoyHoldType* hold_type) {
    *hold_type = (i64)__atomic_load_n(&hid.hold_type, __ATOMIC_RELAXED);
    return 0;
}
/* Return a paired Joy-Con's other Npad, including when id is the hidden secondary. style_lock held. */
static int pair_release_locked(int id) {
    int primary = id;
    int other;
    if (hid.paired[id] == -2) {
        for (primary = 0; primary < 8 && hid.paired[primary] != id; primary++) {}
        if (primary == 8) return -1;
    }
    other = hid.paired[primary];
    if (other < 0) return -1;
    hid.paired[primary] = hid.paired[other] = -1;
    hid.paired_source[primary] = hid.paired_source[other] = 0;
    return id == primary ? other : primary;
}
static int joy_assignment_set(u32 id, u8 assignment) {
    int dest = -1;
    if (id >= 8) return -1;
    input_style_lock();
    {
        int other = pair_release_locked((int)id);
        if (assignment && other >= 0) dest = other;
    }
    if (!assignment && hid.split_dest[id] >= 0) {
        hid.split_parent[hid.split_dest[id]] = -1;
        hid.split_dest[id] = -1;
    }
    if (assignment && hid.split_dest[id] < 0 &&
        __atomic_load_n(&player_pad((int)id)->kind, __ATOMIC_RELAXED) == SNX_PAD_JOY_DUAL) {
        u32 ids = __atomic_load_n(&hid.id_supported, __ATOMIC_RELAXED);
        for (int i = 0; i < 8; i++)
            if (i != (int)id && (ids & (1u << i)) && hid.split_parent[i] < 0 && hid.paired[i] == -1 &&
                !__atomic_load_n(&player_pad(i)->connected, __ATOMIC_RELAXED)) {
                hid.split_parent[i] = (i8)id;
                hid.split_dest[id] = (i8)i;
                hid.split_source[id] = player_pad((int)id)->source_id;
                break;
            }
    }
    if (assignment && __atomic_load_n(&player_pad((int)id)->kind, __ATOMIC_RELAXED) == SNX_PAD_JOY_DUAL &&
        hid.split_dest[id] < 0)
        assignment = 0; /* keep the pair connected if no guest player slot can take its other half */
    hid.joy_assignment[id] = assignment;
    if (hid.split_dest[id] >= 0) dest = hid.split_dest[id];
    npad_refresh_locked();
    input_style_unlock();
    return dest;
}
static int nn_hid_IHidServer_SetNpadJoyAssignmentModeSingleByDefault(SvcObj* self, nn_hid_NpadIdType id, u64 aruid) {
    if (id < 8) {
        PadKind kind = __atomic_load_n(&player_pad((int)id)->kind, __ATOMIC_RELAXED);
        joy_assignment_set(id, kind == SNX_PAD_JOY_RIGHT ? 2 : 1);
    }
    return 0;
}
static int nn_hid_IHidServer_SetNpadJoyAssignmentModeSingle(SvcObj* self, nn_hid_NpadIdType id, u64 aruid,
                                                            nn_hid_NpadJoyDeviceType type) {
    joy_assignment_set(id, type == 1 ? 2 : 1);
    return 0;
}
static int nn_hid_IHidServer_SetNpadJoyAssignmentModeDual(SvcObj* self, nn_hid_NpadIdType id, u64 aruid) {
    joy_assignment_set(id, 0);
    return 0;
}
static int nn_hid_IHidServer_SetNpadJoyAssignmentModeSingleWithDestination(SvcObj* self, nn_hid_NpadIdType id,
                                                                           u64 aruid, nn_hid_NpadJoyDeviceType type,
                                                                           u8* is_reassigned,
                                                                           nn_hid_NpadIdType* new_id) {
    int dest = joy_assignment_set(id, type == 1 ? 2 : 1);
    *is_reassigned = dest >= 0;
    *new_id = dest >= 0 ? (u32)dest : id;
    return 0;
}
static int nn_hid_IHidServer_MergeSingleJoyAsDualJoy(SvcObj* self, nn_hid_NpadIdType id1, nn_hid_NpadIdType id2,
                                                     u64 aruid) {
    if (id1 >= 8 || id2 >= 8 || id1 == id2) return 0;
    input_style_lock();
    PadKind a = __atomic_load_n(&player_pad((int)id1)->kind, __ATOMIC_RELAXED);
    PadKind b = __atomic_load_n(&player_pad((int)id2)->kind, __ATOMIC_RELAXED);
    if (__atomic_load_n(&player_pad((int)id1)->connected, __ATOMIC_RELAXED) &&
        __atomic_load_n(&player_pad((int)id2)->connected, __ATOMIC_RELAXED) &&
        ((a == SNX_PAD_JOY_LEFT && b == SNX_PAD_JOY_RIGHT) || (a == SNX_PAD_JOY_RIGHT && b == SNX_PAD_JOY_LEFT))) {
        /* Either Joy-Con may already belong to another pair. Release both old partners first. */
        pair_release_locked((int)id1);
        pair_release_locked((int)id2);
        hid.paired[id1] = (i8)id2;
        hid.paired[id2] = -2;
        hid.paired_source[id1] = player_pad((int)id1)->source_id;
        hid.paired_source[id2] = player_pad((int)id2)->source_id;
        hid.joy_assignment[id1] = 0;
    }
    npad_refresh_locked();
    input_style_unlock();
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
/* an amplitude, 0 to 1, as 0 to 65535 (a NaN is none) */
static u32 amplitude(float a) { return a > 0 ? a < 1 ? (u32)(a * 65535.0f) : 65535 : 0; }
/* The handle's second byte is the Npad ID; its third byte selects the motor on that player. */
static void vibrate(nn_hid_VibrationDeviceHandle handle, const nn_hid_VibrationValue* v) {
    u32 id = (handle >> 8) & 0xFF;
    u32 device = (handle >> 16) & 1;
    PadState* pad = id < 8 ? player_pad((int)id) : id == 0x20 ? &g_pad : NULL;
    if (!pad) return;
    input_style_lock();
    if (id < 8 && hid.split_parent[id] >= 0) {
        pad = player_pad(hid.split_parent[id]);
        device = hid.visible_style[id] == (1u << 4) ? 1 : 0;
    } else if (id < 8 && hid.paired[id] >= 0) {
        int other = hid.paired[id];
        PadKind kind = __atomic_load_n(&pad->kind, __ATOMIC_RELAXED);
        if ((device == 0 && kind != SNX_PAD_JOY_LEFT) || (device == 1 && kind != SNX_PAD_JOY_RIGHT))
            pad = player_pad(other);
    }
    __atomic_store_n(&pad->rumble[device], amplitude(v->amp_low) | amplitude(v->amp_high) << 16, __ATOMIC_RELAXED);
    input_style_unlock();
}
static int nn_hid_IHidServer_SendVibrationValue(SvcObj* self, nn_hid_VibrationDeviceHandle handle,
                                                const nn_hid_VibrationValue* value, u64 aruid) {
    vibrate(handle, value);
    return 0;
}
static int nn_hid_IHidServer_SendVibrationValues(SvcObj* self, nn_applet_AppletResourceUserId aruid, IpcBuf handles,
                                                 IpcBuf values) {
    u64 i, n = handles.size / 4 < values.size / 16 ? handles.size / 4 : values.size / 16;
    for (i = 0; i < n; i++) {
        nn_hid_VibrationDeviceHandle h;
        nn_hid_VibrationValue v;
        memcpy(&h, GP(handles.addr + i * 4), 4);
        memcpy(&v, GP(values.addr + i * 16), 16);
        vibrate(h, &v);
    }
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
    u32 id = (handle >> 8) & 0xFF;
    input_style_lock();
    *mounted = id < 8 ? hid.visible_style[id] != 0 : id == 0x20 && hid.visible_style[8] != 0;
    input_style_unlock();
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
    /* No1 also represents the keyboard, on-screen controls, and scripted input. */
    g_pad.connected = 1;
    g_pad.kind = SNX_PAD_FULL_KEY;
    for (int i = 0; i < 8; i++) {
        hid.split_parent[i] = -1;
        hid.split_dest[i] = -1;
        hid.paired[i] = -1;
    }
    hid.shm = owned_shm(0x40000, "hid");
    hid.xpad_shm = owned_shm(0x1000, "hid xpad lifo");
    hid.six_shm = owned_shm(0x1000, "hid six-axis lifo");
    hid.joysix_shm = owned_shm(0x1000, "hid joy six-axis lifo");
    /* the style events start signaled, so the game reads the styles the first time it waits */
    for (int i = 0; i < 10; i++) {
        hid.style_ev[i] = owned_event("npad style");
        kevent_signal(hid.style_ev[i]);
    }
    npad_refresh_locked();
    hid_update();
    hid_update();
    nn_hid_IHidServer_register(new_hid);
    nn_hid_IHidSystemServer_register(new_hidsys);
}
