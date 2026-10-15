/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* am: the applet manager (appletOE, appletAE) and its storages */
#include "hle/services/services_int.h"
#include "hle/services/am.swipc.inc"
/* ================================================================ am */
static struct {
    KEvent* msg_ev;
    u32 msgq[16];
    int nmsg;
    KEvent* launch_ev;
    KEvent* res_ev;
    KEvent* sleep_ev; /* GetAcquiredSleepLockEvent: signaled when a RequestToAcquireSleepLock is granted */
    u64 layer;        /* the next managed display layer id, CreateManagedDisplayLayer's and SeparableLayer's */
    int user_given;   /* PreselectedUser was popped: it is there once */
} am = {.layer = 1};
u64 g_user_id[2] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
static void am_push_msg(u32 m) {
    pm_lock(&g_klock);
    int queued = am.nmsg < 16;
    if (queued) am.msgq[am.nmsg++] = m;
    pm_unlock(&g_klock);
    /* a full queue is already signaled, and the message is lost: say so rather than signal for nothing */
    if (queued)
        kevent_signal(am.msg_ev);
    else
        WARN("am: applet message queue full, message %u dropped\n", m);
}
void am_init(void) {
    am.msg_ev = owned_event("applet msg");
    am.launch_ev = owned_event("launchable");
    am.res_ev = owned_event("res change");
    am.sleep_ev = owned_event("sleep lock");
    am_push_msg(0xF);  /* FocusStateChanged */
    am_push_msg(0x1F); /* PerformanceModeChanged */
    kevent_signal(am.launch_ev);
}

/* IStorage / IStorageAccessor */
typedef struct {
    u8* d;
    u64 size;
} StorageData;
static void storage_destroy(SvcObj* o) { free(((StorageData*)o->data)->d); }
/* an accessor holds a reference on its storage (u[0]), dropped when the game closes the accessor */
static void storage_acc_destroy(SvcObj* o) { svcobj_unref((SvcObj*)(size_t)o->u[0]); }
static StorageData* accessor_storage(SvcObj* accessor) { return ((SvcObj*)(size_t)accessor->u[0])->data; }
static int nn_am_service_IStorageAccessor_GetSize(SvcObj* self, i64* size) {
    *size = (i64)accessor_storage(self)->size;
    return 0;
}
static int nn_am_service_IStorageAccessor_Write(SvcObj* self, i64 offset, IpcBuf data) {
    StorageData* s = accessor_storage(self);
    u64 off = (u64)offset, n = data.size;
    /* off + n can wrap: compare against what is left instead */
    if (off > s->size || n > s->size - off) return RC(128, 503);
    memcpy(s->d + off, GP(data.addr), n);
    return 0;
}
static int nn_am_service_IStorageAccessor_Read(SvcObj* self, i64 offset, IpcBuf data) {
    StorageData* s = accessor_storage(self);
    u64 off = (u64)offset, n = data.size;
    if (off > s->size) return RC(128, 503);
    if (off + n > s->size) n = s->size - off;
    memcpy(GP(data.addr), s->d + off, n);
    return 0;
}
static int nn_am_service_IStorage_Open(SvcObj* self, SvcObj** accessor) {
    SvcObj* a = nn_am_service_IStorageAccessor_new(0);
    a->u[0] = (u64)(size_t)self;
    svcobj_ref(self);
    a->destroy = storage_acc_destroy;
    *accessor = a;
    return 0;
}
/* OpenTransferStorage gives an ITransferStorageAccessor, which only a storage made over transfer memory has. These
   storages are plain memory (CreateTransferMemoryStorage copies the transfer memory into one) */
static int nn_am_service_IStorage_OpenTransferStorage(SvcObj* self, SvcObj** accessor) { return RC(128, 503); }
/* NULL when the guest asks for more than the host can allocate */
static SvcObj* am_storage_new(u64 size, const void* init) {
    u8* d = calloc(1, size ? size : 1);
    if (!d) {
        WARN("am: no memory for a %llu byte storage\n", (unsigned long long)size);
        return NULL;
    }
    SvcObj* o = nn_am_service_IStorage_new(sizeof(StorageData));
    StorageData* s = o->data;
    s->d = d;
    s->size = size;
    if (init) memcpy(s->d, init, size);
    o->destroy = storage_destroy;
    return o;
}
/* the result of a command that makes a storage */
static int out_storage(SvcObj** storage, u64 size, const void* init) {
    *storage = am_storage_new(size, init);
    return *storage ? 0 : RC(128, 503);
}
static int is_storage(const SvcObj* o) { return o && o->dispatch == nn_am_service_IStorage_dispatch; }

static int nn_am_service_ICommonStateGetter_GetEventHandle(SvcObj* self, u32* event) {
    *event = ev_handle(am.msg_ev);
    return 0;
}
static int nn_am_service_ICommonStateGetter_ReceiveMessage(SvcObj* self, nn_am_AppletMessage* message) {
    pm_lock(&g_klock);
    if (!am.nmsg) {
        am.msg_ev->signaled = 0;
        pm_unlock(&g_klock);
        return RC(128, 3);
    }
    *message = am.msgq[0];
    memmove(am.msgq, am.msgq + 1, (--am.nmsg) * 4);
    if (!am.nmsg) am.msg_ev->signaled = 0;
    pm_unlock(&g_klock);
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetOperationMode(SvcObj* self, u8* mode) {
    *mode = (u8)g_opt_docked;
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetPerformanceMode(SvcObj* self, u32* mode) {
    *mode = (u32)g_opt_docked;
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetCurrentFocusState(SvcObj* self, u8* state) {
    *state = 1; /* InFocus */
    return 0;
}
/* granted at once, there is no sleep here */
static int nn_am_service_ICommonStateGetter_RequestToAcquireSleepLock(SvcObj* self) {
    kevent_signal(am.sleep_ev);
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetAcquiredSleepLockEvent(SvcObj* self, u32* event) {
    *event = ev_handle(am.sleep_ev);
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetDefaultDisplayResolution(SvcObj* self, i32* width, i32* height) {
    *width = g_opt_docked ? 1920 : 1280;
    *height = g_opt_docked ? 1080 : 720;
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetDefaultDisplayResolutionChangeEvent(SvcObj* self, u32* event) {
    *event = ev_handle(am.res_ev);
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetHdcpAuthenticationStateChangeEvent(SvcObj* self, u32* event) {
    *event = ev_handle(am.res_ev);
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetCurrentPerformanceConfiguration(SvcObj* self, u32* configuration) {
    *configuration = 0x00020003; /* the one apm reports */
    return 0;
}
/* the main loop saves what it keeps and ends the process */
static int nn_am_service_ISelfController_Exit(SvcObj* self) {
    g_quit = 1;
    return 0;
}
static int nn_am_service_ISelfController_GetLibraryAppletLaunchableEvent(SvcObj* self, u32* event) {
    *event = ev_handle(am.launch_ev);
    return 0;
}
static int nn_am_service_ISelfController_CreateManagedDisplayLayer(SvcObj* self, u64* layer_id) {
    *layer_id = __atomic_fetch_add(&am.layer, 1, __ATOMIC_RELAXED);
    return 0;
}
static int nn_am_service_ISelfController_CreateManagedDisplaySeparableLayer(SvcObj* self, u64* layer_id,
                                                                            u64* recording_layer_id) {
    *layer_id = __atomic_fetch_add(&am.layer, 2, __ATOMIC_RELAXED); /* two layers, never one handed out before */
    *recording_layer_id = *layer_id + 1;
    return 0;
}
static int nn_am_service_ISelfController_GetAccumulatedSuspendedTickChangedEvent(SvcObj* self, u32* event) {
    return new_event(event, "suspend", 0);
}
static int nn_am_service_IWindowController_GetAppletResourceUserId(SvcObj* self,
                                                                   nn_applet_AppletResourceUserId* aruid) {
    *aruid = 0x1234;
    return 0;
}
static int nn_am_service_IApplicationFunctions_PopLaunchParameter(SvcObj* self, u32 kind, SvcObj** storage) {
    u8 buf[0x88];
    u32 magic = 0xC79497CA, one = 1;
    /* only the PreselectedUser (2), there once */
    if (kind != 2 || __atomic_exchange_n(&am.user_given, 1, __ATOMIC_SEQ_CST)) return RC(128, 2);
    memset(buf, 0, sizeof buf);
    memcpy(buf, &magic, 4);
    memcpy(buf + 4, &one, 4);
    memcpy(buf + 8, g_user_id, 16);
    return out_storage(storage, sizeof buf, buf);
}
static int nn_am_service_IApplicationFunctions_GetDesiredLanguage(SvcObj* self, nn_settings_LanguageCode* code) {
    set_language_code(g_opt_lang, code->v);
    return 0;
}
static int nn_am_service_IApplicationFunctions_GetDisplayVersion(SvcObj* self, nn_oe_DisplayVersion* version) {
    strcpy((char*)version->v, "1.0.0");
    return 0;
}
static int nn_am_service_IApplicationFunctions_GetSaveDataSize(SvcObj* self, u8 type, const nn_account_Uid* uid,
                                                               i64* normal_size, i64* journal_size) {
    *normal_size = 0x4000000;
    *journal_size = 0x4000000;
    return 0;
}
static int nn_am_service_IApplicationFunctions_GetLimitedApplicationLicenseUpgradableEvent(SvcObj* self, u32* event) {
    return new_event(event, "appfn ev", 0);
}
static int nn_am_service_IApplicationFunctions_GetPseudoDeviceId(SvcObj* self, nn_util_Uuid* id) {
    static const u64 pseudo[2] = {0x1111222233334444ull, 0x5555666677778888ull};
    memcpy(id->v, pseudo, 16);
    return 0;
}
static int nn_am_service_IApplicationFunctions_GetGpuErrorDetectedSystemEvent(SvcObj* self, u32* event) {
    return new_event(event, "appfn ev", 0);
}
static int nn_am_service_IApplicationFunctions_GetFriendInvitationStorageChannelEvent(SvcObj* self, u32* event) {
    return new_event(event, "appfn ev", 0);
}
static int nn_am_service_IApplicationFunctions_TryPopFromFriendInvitationStorageChannel(SvcObj* self,
                                                                                        SvcObj** storage) {
    return RC(128, 2); /* nothing queued */
}
static int nn_am_service_IApplicationFunctions_GetNotificationStorageChannelEvent(SvcObj* self, u32* event) {
    return new_event(event, "appfn ev", 0);
}
static int nn_am_service_IApplicationFunctions_TryPopFromNotificationStorageChannel(SvcObj* self, SvcObj** storage) {
    return RC(128, 2); /* nothing queued */
}
static int nn_am_service_IApplicationFunctions_GetHealthWarningDisappearedSystemEvent(SvcObj* self, u32* event) {
    return new_event(event, "appfn ev", 0);
}
static int nn_am_service_IApplicationFunctions_GetUnknownEvent210(SvcObj* self, u32* event) {
    return new_event(event, "appfn ev", 0);
}
/* library applets (swkbd, error...) finish immediately: no applet UI here */
typedef struct {
    u32 id, mode;
    int popped_out, popped_interactive;
    SvcObj* in[8]; /* the storages PushInData passed, in order, under lib_lock: game threads push and pop at once */
    int nin;
} LibApplet;
static PMutex lib_lock = PMUTEX_INIT;
static void lib_applet_destroy(SvcObj* o) {
    LibApplet* la = o->data;
    for (int i = 0; i < la->nin; i++) svcobj_unref(la->in[i]);
}
/* the controller applet (ShowControllerSupport) answers ControllerSupportResultInfo {i8 player_count; pad[3];
   u32 selected_id} and a u32 result, which the SDK turns into success (0) or cancelled (2). The last storage
   pushed is the ControllerSupportArg, whose byte 0 is min_player_count: one controller (npad No1) satisfies at
   most one player, as a console with one controller connected would */
static i8 controller_min_players(LibApplet* la) { /* lib_lock held */
    SvcObj* arg = la->nin ? la->in[la->nin - 1] : NULL;
    return is_storage(arg) && ((StorageData*)arg->data)->size ? (i8)((StorageData*)arg->data)->d[0] : 0;
}
static SvcObj* controller_result(i8 min_players) {
    u8 r[0xC];
    memset(r, 0, sizeof r);
    u32 result = min_players <= 1 ? 0 : 2;
    if (!result) r[0] = 1; /* player_count 1, selected_id 0 (No1) */
    memcpy(r + 8, &result, 4);
    if (result) LOG("applet: controller support asks for %d players, one controller here: cancelled\n", min_players);
    return am_storage_new(sizeof r, r);
}
/* Standard swkbd returns 0x7D8 bytes: u32 close result, then a UTF-16 name. */
static u32 swkbd_text(u8* out, const char* text, u32 max_chars) {
    u32 count = 0;
    const u8* p = (const u8*)text;
    while (*p && count < max_chars && count < (0x7D8 - 4) / 2 - 1) {
        u32 cp = *p++;
        if (cp >= 0xF0 && cp < 0xF5 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
            !(cp == 0xF0 && p[0] < 0x90) && !(cp == 0xF4 && p[0] >= 0x90)) {
            cp = ((cp & 7) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else if (cp >= 0xE0 && cp < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80 &&
                   !(cp == 0xE0 && p[0] < 0xA0) && !(cp == 0xED && p[0] >= 0xA0)) {
            cp = ((cp & 15) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if (cp >= 0xC2 && cp < 0xE0 && (p[0] & 0xC0) == 0x80) {
            cp = ((cp & 31) << 6) | (p[0] & 0x3F);
            p++;
        } else if (cp >= 0x80) {
            cp = 0xFFFD;
        }
        if (cp >= 0x10000) {
            if (count + 2 > max_chars || count + 2 >= (0x7D8 - 4) / 2) break;
            cp -= 0x10000;
            u16 hi = 0xD800 | (cp >> 10), lo = 0xDC00 | (cp & 0x3FF);
            memcpy(out + 4 + count++ * 2, &hi, 2);
            memcpy(out + 4 + count++ * 2, &lo, 2);
        } else {
            u16 ch = cp;
            memcpy(out + 4 + count++ * 2, &ch, 2);
        }
    }
    return count;
}
#define SWKBD_MAX_CHARS ((0x7D8 - 4) / 2 - 1)
/* the text length limits of the config the game pushed second (lib_lock held) */
static void swkbd_limits(LibApplet* la, u32* max_chars, u32* min_chars) {
    StorageData* cfg = la->nin > 1 && is_storage(la->in[1]) ? la->in[1]->data : NULL;
    *max_chars = SWKBD_MAX_CHARS;
    *min_chars = 0;
    if (cfg && cfg->size >= 0x3B4) {
        u32 limit;
        memcpy(&limit, cfg->d + 0x3AC, 4);
        if (limit && limit < *max_chars) *max_chars = limit;
        memcpy(min_chars, cfg->d + 0x3B0, 4);
    }
}
/* asks the host for the text, which can take as long as the player types: no lock held */
static SvcObj* swkbd_result(u32 max_chars, u32 min_chars) {
    u8 r[0x7D8] = {0};
    if (min_chars > max_chars) {
        WARN("applet: swkbd minimum text length %u exceeds maximum %u\n", min_chars, max_chars);
        min_chars = max_chars;
    }
    char input[4096];
    int result;
    u32 chars = 0;
    const char* preset = getenv("SNX_SWKBD_TEXT");
    if (preset) {
        chars = swkbd_text(r, preset, max_chars);
        result = chars < min_chars ? 1 : 0;
    } else {
        do {
            result = plat_text_input(input, sizeof input, "");
            if (result) break;
            memset(r + 4, 0, sizeof r - 4);
            chars = swkbd_text(r, input, max_chars);
        } while (chars < min_chars);
    }
    LOG("applet: swkbd %s with %u UTF-16 code units\n", result ? "cancelled" : "accepted", chars);
    if (result) {
        u32 cancelled = 1;
        memcpy(r, &cancelled, 4);
        if (result < 0) WARN("applet: host software keyboard unavailable (install zenity or kdialog on POSIX)\n");
    }
    return am_storage_new(sizeof r, r);
}
static int nn_am_service_ILibraryAppletAccessor_GetAppletStateChangedEvent(SvcObj* self, u32* event) {
    return new_event(event, "applet state", 1);
}
static int nn_am_service_ILibraryAppletAccessor_IsCompleted(SvcObj* self, u8* completed) {
    *completed = 1;
    return 0;
}
/* the controller applet's answer depends on what was pushed */
static int nn_am_service_ILibraryAppletAccessor_PushInData(SvcObj* self, SvcObj* storage) {
    LibApplet* la = self->data;
    pm_lock(&lib_lock);
    if (storage && la->nin < 8) {
        svcobj_ref(storage);
        la->in[la->nin++] = storage;
    }
    pm_unlock(&lib_lock);
    return 0;
}
/* PopOutData and PopInteractiveOutData are separate channels, each with one storage. Web applets (Web, Shop,
   OfflineWeb, LoginShare, WifiWebAuth) return a 0x1010 byte WebCommonReturnValue (exit reason 0, last url, url size),
   whose size callers check. PlayerSelect returns UiReturnArg {u64 result; Uid uid}: the one user, selected */
static int pop_data(LibApplet* la, int interactive, SvcObj** storage) {
    u32 id = la->id;
    u64 n = 0x10;
    /* Standard swkbd has no interactive output unless it requests application text validation. */
    if (id == 0x11 && interactive) return RC(128, 2);
    if (__atomic_exchange_n(interactive ? &la->popped_interactive : &la->popped_out, 1, __ATOMIC_SEQ_CST))
        return RC(128, 2);
    SvcObj* st;
    if (id == 0x0C && !interactive) {
        pm_lock(&lib_lock);
        i8 min_players = controller_min_players(la);
        pm_unlock(&lib_lock);
        st = controller_result(min_players);
        n = 0xC;
    } else if (id == 0x11 && !interactive) {
        u32 max_chars, min_chars;
        pm_lock(&lib_lock);
        swkbd_limits(la, &max_chars, &min_chars);
        pm_unlock(&lib_lock);
        st = swkbd_result(max_chars, min_chars);
        n = 0x7D8;
    } else if (id == 0x10) {
        u8 r[0x18];
        memset(r, 0, sizeof r);
        memcpy(r + 8, g_user_id, 16);
        n = sizeof r;
        st = am_storage_new(n, r);
    } else {
        if (id == 0x13 || id == 0x14 || id == 0x17 || id == 0x18 || id == 0x19) n = 0x1010;
        st = am_storage_new(n, NULL);
    }
    if (!st) return RC(128, 503);
    if (id != 0x11)
        LOG_LIMIT(4, "applet: library applet %02x closed right away (no applet UI here), %llu bytes of output\n", id,
                  (unsigned long long)n);
    *storage = st;
    return 0;
}
static int nn_am_service_ILibraryAppletAccessor_PopOutData(SvcObj* self, SvcObj** storage) {
    return pop_data(self->data, 0, storage);
}
static int nn_am_service_ILibraryAppletAccessor_PopInteractiveOutData(SvcObj* self, SvcObj** storage) {
    return pop_data(self->data, 1, storage);
}
static int nn_am_service_ILibraryAppletAccessor_GetPopOutDataEvent(SvcObj* self, u32* event) {
    return new_event(event, "applet data", 1);
}
/* swkbd gives no interactive output: that one never signals */
static int nn_am_service_ILibraryAppletAccessor_GetPopInteractiveOutDataEvent(SvcObj* self, u32* event) {
    return new_event(event, "applet data", ((LibApplet*)self->data)->id != 0x11);
}
static int nn_am_service_ILibraryAppletAccessor_GetLibraryAppletInfo(SvcObj* self,
                                                                     nn_am_service_LibraryAppletInfo* info) {
    LibApplet* la = self->data;
    info->applet_id = la->id;
    info->library_applet_mode = la->mode;
    return 0;
}
static int nn_am_service_ILibraryAppletCreator_CreateLibraryApplet(SvcObj* self, u32 applet_id, u32 library_applet_mode,
                                                                   SvcObj** accessor) {
    SvcObj* a = nn_am_service_ILibraryAppletAccessor_new(sizeof(LibApplet));
    ((LibApplet*)a->data)->id = applet_id;
    ((LibApplet*)a->data)->mode = library_applet_mode;
    a->destroy = lib_applet_destroy;
    *accessor = a;
    return 0;
}
static int nn_am_service_ILibraryAppletCreator_CreateStorage(SvcObj* self, i64 size, SvcObj** storage) {
    return out_storage(storage, (u64)size, NULL);
}
/* a storage holding what the transfer memory holds. A copy is enough: the library applets here finish at once and
   only read what was pushed */
static int nn_am_service_ILibraryAppletCreator_CreateTransferMemoryStorage(SvcObj* self, u8 is_writable, i64 size,
                                                                           u32 transfer_memory, SvcObj** storage) {
    KTmem* m = (KTmem*)h_get_ref(transfer_memory, KO_TMEM);
    if (!m) return out_storage(storage, (u64)size, NULL);
    SvcObj* st = am_storage_new((u64)size, NULL);
    if (st) memcpy(((StorageData*)st->data)->d, GP(m->addr), (u64)size < m->size ? (u64)size : m->size);
    kobj_unref(&m->o);
    if (!st) return RC(128, 503);
    *storage = st;
    return 0;
}
static int nn_am_service_IAudioController_GetMainAppletExpectedMasterVolume(SvcObj* self, float* volume) {
    *volume = 1.0f;
    return 0;
}
static int nn_am_service_IAudioController_GetLibraryAppletExpectedMasterVolume(SvcObj* self, float* volume) {
    *volume = 1.0f;
    return 0;
}
static int nn_am_service_IApplicationProxy_GetCommonStateGetter(SvcObj* self, SvcObj** getter) {
    *getter = nn_am_service_ICommonStateGetter_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetSelfController(SvcObj* self, SvcObj** controller) {
    *controller = nn_am_service_ISelfController_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetWindowController(SvcObj* self, SvcObj** controller) {
    *controller = nn_am_service_IWindowController_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetAudioController(SvcObj* self, SvcObj** controller) {
    *controller = nn_am_service_IAudioController_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetDisplayController(SvcObj* self, SvcObj** controller) {
    *controller = nn_am_service_IDisplayController_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetProcessWindingController(SvcObj* self, SvcObj** controller) {
    *controller = nn_am_service_IProcessWindingController_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetLibraryAppletCreator(SvcObj* self, SvcObj** creator) {
    *creator = nn_am_service_ILibraryAppletCreator_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetApplicationFunctions(SvcObj* self, SvcObj** functions) {
    *functions = nn_am_service_IApplicationFunctions_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetDebugFunctions(SvcObj* self, SvcObj** functions) {
    *functions = nn_am_service_IDebugFunctions_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxyService_OpenApplicationProxy(SvcObj* self, u64 pid, u32 process,
                                                                       SvcObj** proxy) {
    *proxy = nn_am_service_IApplicationProxy_new(0);
    return 0;
}
static int nn_am_service_IAllSystemAppletProxiesService_OpenSystemApplicationProxy(SvcObj* self, u64 pid, u32 process,
                                                                                   SvcObj** proxy) {
    *proxy = nn_am_service_IApplicationProxy_new(0);
    return 0;
}
SvcObj* new_appletoe(void) { return nn_am_service_IApplicationProxyService_new(0); }
SvcObj* new_appletae(void) { return nn_am_service_IAllSystemAppletProxiesService_new(0); }
