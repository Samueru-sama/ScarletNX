/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the smaller services, most of which answer success: apm, lm, pctl, friend, ns, ssl, prepo, psm, fatal, csrng, mm, nfp, caps, bcat, aoc, irs, mii, ldn, ectx, spl, btm, audin.
   Their interfaces are in misc.swipc */
#include "hle/services/services_int.h"
#include "hle/services/misc.swipc.inc"
/* ================================================================ apm and misc */
static int nn_apm_ISession_GetPerformanceConfiguration(SvcObj* self, nn_apm_PerformanceMode mode,
                                                       nn_apm_PerformanceConfiguration* config) {
    *config = 0x00020003;
    return 0;
}
static int nn_apm_IManager_OpenSession(SvcObj* self, SvcObj** session) {
    *session = nn_apm_ISession_new(0);
    return 0;
}
static int nn_apm_IManager_GetPerformanceMode(SvcObj* self, nn_apm_PerformanceMode* mode) {
    *mode = g_hle_docked;
    return 0;
}
SvcObj* new_apm(void) { return nn_apm_IManager_new(0); }
/* an unsigned LEB128 number of lm's packet fields; 0 past the end */
static u64 uleb128(const u8* p, u64 s, u64* i) {
    u64 v = 0;
    for (int sh = 0; *i < s && sh < 64; sh += 7) {
        u8 b = p[(*i)++];
        v |= (u64)(b & 0x7F) << sh;
        if (!(b & 0x80)) break;
    }
    return v;
}
static int nn_lm_ILogger_Log(SvcObj* self, IpcBuf message) {
    u64 s = message.size;
    if (message.addr && s > 0x18) { /* lm packet: 0x18 hdr, then fields {uleb128 key; uleb128 size; data} */
        u8* p = GP(message.addr);
        u64 i = 0x18;
        char msg[1024];
        u64 n = 0;
        while (i < s) {
            u64 t = uleb128(p, s, &i), l = uleb128(p, s, &i);
            if (l > s - i) break;
            if (t == 2 && n + l < 1000) { /* the text */
                memcpy(msg + n, p + i, l);
                n += l;
            }
            i += l;
        }
        if (n) {
            msg[n] = 0;
            LOG("[game lm] %s%s", msg, (msg[n - 1] == '\n') ? "" : "\n");
        }
    }
    return 0;
}
static int nn_lm_ILogService_OpenLogger(SvcObj* self, u64 pid, SvcObj** logger) {
    *logger = nn_lm_ILogger_new(0);
    return 0;
}
SvcObj* new_lm(void) { return nn_lm_ILogService_new(0); }

static int nn_pctl_detail_ipc_IParentalControlService_IsStereoVisionPermitted(SvcObj* self, u8* permitted) {
    *permitted = 1;
    return 0;
}
static int nn_pctl_detail_ipc_IParentalControlService_GetSynchronizationEvent(SvcObj* self, u32* event) {
    return new_event(event, "pctl ev", 0);
}
static int nn_pctl_detail_ipc_IParentalControlService_GetPlayTimerEventToRequestSuspension(SvcObj* self, u32* event) {
    return new_event(event, "pctl ev", 0);
}
static int nn_pctl_detail_ipc_IParentalControlService_GetUnlinkedEvent(SvcObj* self, u32* event) {
    return new_event(event, "pctl ev", 0);
}
static int nn_pctl_detail_ipc_IParentalControlServiceFactory_CreateService(SvcObj* self, u64 pid, SvcObj** service) {
    *service = nn_pctl_detail_ipc_IParentalControlService_new(0);
    return 0;
}
static int nn_pctl_detail_ipc_IParentalControlServiceFactory_CreateServiceWithoutInitialize(SvcObj* self, u64 pid,
                                                                                            SvcObj** service) {
    *service = nn_pctl_detail_ipc_IParentalControlService_new(0);
    return 0;
}
SvcObj* new_pctl(void) { return nn_pctl_detail_ipc_IParentalControlServiceFactory_new(0); }

static int nn_friends_detail_ipc_IFriendService_GetCompletionEvent(SvcObj* self, u32* event) {
    return new_event(event, "friend", 0);
}
static int nn_friends_detail_ipc_INotificationService_GetEvent(SvcObj* self, u32* event) {
    return new_event(event, "friend notif", 0);
}
static int nn_friends_detail_ipc_INotificationService_Pop(SvcObj* self,
                                                          nn_friends_detail_ipc_SizedNotificationInfo* info) {
    return RC(121, 15); /* no notification */
}
static int nn_friends_detail_ipc_IServiceCreator_CreateFriendService(SvcObj* self, SvcObj** service) {
    *service = nn_friends_detail_ipc_IFriendService_new(0);
    return 0;
}
static int nn_friends_detail_ipc_IServiceCreator_CreateNotificationService(SvcObj* self, const nn_account_Uid* uid,
                                                                           SvcObj** service) {
    *service = nn_friends_detail_ipc_INotificationService_new(0);
    return 0;
}
static int nn_friends_detail_ipc_IServiceCreator_CreateDaemonSuspendSessionService(SvcObj* self, SvcObj** service) {
    *service = nn_friends_detail_ipc_IDaemonSuspendSessionService_new(0);
    return 0;
}
SvcObj* new_friend(void) { return nn_friends_detail_ipc_IServiceCreator_new(0); }

static int nn_ns_detail_IServiceGetterInterface_GetDynamicRightsInterface(SvcObj* self, SvcObj** iface) {
    *iface = nn_ns_detail_IDynamicRightsInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetReadOnlyApplicationControlDataInterface(SvcObj* self,
                                                                                           SvcObj** iface) {
    *iface = nn_ns_detail_IReadOnlyApplicationControlDataInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetReadOnlyApplicationRecordInterface(SvcObj* self, SvcObj** iface) {
    *iface = nn_ns_detail_IReadOnlyApplicationRecordInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetECommerceInterface(SvcObj* self, SvcObj** iface) {
    *iface = nn_ns_detail_IECommerceInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetApplicationVersionInterface(SvcObj* self, SvcObj** iface) {
    *iface = nn_ns_detail_IApplicationVersionInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetFactoryResetInterface(SvcObj* self, SvcObj** iface) {
    *iface = nn_ns_detail_IFactoryResetInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetAccountProxyInterface(SvcObj* self, SvcObj** iface) {
    *iface = nn_ns_detail_IAccountProxyInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetApplicationManagerInterface(SvcObj* self, SvcObj** iface) {
    *iface = nn_ns_detail_IApplicationManagerInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetDownloadTaskInterface(SvcObj* self, SvcObj** iface) {
    *iface = nn_ns_detail_IDownloadTaskInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetContentManagementInterface(SvcObj* self, SvcObj** iface) {
    *iface = nn_ns_detail_IContentManagementInterface_new(0);
    return 0;
}
static int nn_ns_detail_IServiceGetterInterface_GetDocumentInterface(SvcObj* self, SvcObj** iface) {
    *iface = nn_ns_detail_IDocumentInterface_new(0);
    return 0;
}
SvcObj* new_ns(void) { return nn_ns_detail_IServiceGetterInterface_new(0); }

static int nn_ssl_sf_ISslService_CreateContext(SvcObj* self, nn_ssl_sf_SslVersion ssl_version, u64 pid,
                                               SvcObj** context) {
    *context = nn_ssl_sf_ISslContext_new(0);
    return 0;
}
SvcObj* new_ssl(void) { return nn_ssl_sf_ISslService_new(0); }
SvcObj* new_prepo(void) { return nn_prepo_detail_ipc_IPrepoService_new(0); }
static int nn_psm_IPsmSession_BindStateChangeEvent(SvcObj* self, u32* event) { return new_event(event, "psm", 0); }
static int nn_psm_IPsmServer_GetBatteryChargePercentage(SvcObj* self, u32* percentage) {
    *percentage = 100;
    return 0;
}
static int nn_psm_IPsmServer_GetChargerType(SvcObj* self, nn_psm_ChargerType* charger_type) {
    *charger_type = 1;
    return 0;
}
static int nn_psm_IPsmServer_OpenSession(SvcObj* self, SvcObj** session) {
    *session = nn_psm_IPsmSession_new(0);
    return 0;
}
SvcObj* new_psm(void) { return nn_psm_IPsmServer_new(0); }
static int nn_fatalsrv_IService_ThrowFatal(SvcObj* self, u32 result, u64 pid) {
    fatal("The game called fatal:u (code %x)", result);
    return 0;
}
static int nn_fatalsrv_IService_ThrowFatalWithPolicy(SvcObj* self, u32 result, u32 policy, u64 pid) {
    fatal("The game called fatal:u (code %x)", result);
    return 0;
}
static int nn_fatalsrv_IService_ThrowFatalWithCpuContext(SvcObj* self, u32 result, u32 policy, u64 pid,
                                                         IpcBuf context) {
    fatal("The game called fatal:u (code %x)", result);
    return 0;
}
SvcObj* new_fatal(void) { return nn_fatalsrv_IService_new(0); }
#define MM_REQ_MAX 32
static struct {
    u64 random_ctr; /* random_fill's counter */
    struct {
        u32 id, module, rate;
    } mm[MM_REQ_MAX]; /* mm:u's requests */
    u32 mm_next;
    KShmem* irs_shm; /* GetIrsensorSharedMemoryHandle's, made by the first call */
} misc = {.random_ctr = 0x9E3779B97F4A7C15ull, .mm_next = 1};
/* random bytes for csrng and spl: splitmix64 over a shared counter, so parallel callers never share a state */
static int random_fill(IpcBuf b) {
    if (!b.addr) return 0;
    u8* d = GP(b.addr);
    for (u64 i = 0; i < b.size; i += 8) {
        u64 z = __atomic_add_fetch(&misc.random_ctr, 0x9E3779B97F4A7C15ull, __ATOMIC_RELAXED);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        memcpy(d + i, &z, b.size - i < 8 ? b.size - i : 8);
    }
    return 0;
}
static int nn_spl_detail_IRandomInterface_GenerateRandomBytes(SvcObj* self, IpcBuf data) { return random_fill(data); }
SvcObj* new_csrng(void) { return nn_spl_detail_IRandomInterface_new(0); }
/* mm:u (multimedia clock requests): Get has to return what SetAndWait asked for, since NvMM reads the decoder clock
   back. Each Initialize is its own request with a new id, as the SDK keeps them. The old forms name the module
   instead of the id */
static int mm_find(u32 key, int by_module) { /* g_klock held */
    for (int i = 0; i < MM_REQ_MAX; i++)
        if (misc.mm[i].id && (by_module ? misc.mm[i].module == key : misc.mm[i].id == key)) return i;
    return -1;
}
static u32 mm_initialize(u32 module) {
    pm_lock(&g_klock);
    u32 id = misc.mm_next++;
    int i = 0;
    while (i < MM_REQ_MAX && misc.mm[i].id) i++;
    if (i < MM_REQ_MAX) {
        misc.mm[i].id = id;
        misc.mm[i].module = module;
        misc.mm[i].rate = 0;
    }
    pm_unlock(&g_klock);
    if (i == MM_REQ_MAX) WARN("mm:u: more than %d clock requests\n", MM_REQ_MAX);
    return id;
}
static int mm_finalize(u32 key, int by_module) {
    pm_lock(&g_klock);
    int i = mm_find(key, by_module);
    if (i >= 0) misc.mm[i].id = 0;
    pm_unlock(&g_klock);
    return 0;
}
static int mm_set(u32 key, int by_module, u32 min) {
    pm_lock(&g_klock);
    int i = mm_find(key, by_module);
    if (i >= 0) misc.mm[i].rate = min;
    pm_unlock(&g_klock);
    return 0;
}
static int mm_get(u32 key, int by_module, u32* current) {
    pm_lock(&g_klock);
    int i = mm_find(key, by_module);
    *current = i >= 0 ? misc.mm[i].rate : 0;
    pm_unlock(&g_klock);
    return 0;
}
static int nn_mmnv_IRequest_InitializeOld(SvcObj* self, nn_mmnv_Module module, nn_mmnv_Priority priority,
                                          u32 event_clear_mode) {
    mm_initialize(module);
    return 0;
}
static int nn_mmnv_IRequest_FinalizeOld(SvcObj* self, nn_mmnv_Module module) { return mm_finalize(module, 1); }
static int nn_mmnv_IRequest_SetAndWaitOld(SvcObj* self, nn_mmnv_Module module, nn_mmnv_Setting min,
                                          nn_mmnv_Setting max) {
    return mm_set(module, 1, min);
}
static int nn_mmnv_IRequest_GetOld(SvcObj* self, nn_mmnv_Module module, nn_mmnv_Setting* current) {
    return mm_get(module, 1, current);
}
static int nn_mmnv_IRequest_Initialize(SvcObj* self, nn_mmnv_Module module, nn_mmnv_Priority priority,
                                       u32 event_clear_mode, u32* request_id) {
    *request_id = mm_initialize(module);
    return 0;
}
static int nn_mmnv_IRequest_Finalize(SvcObj* self, u32 request_id) { return mm_finalize(request_id, 0); }
static int nn_mmnv_IRequest_SetAndWait(SvcObj* self, u32 request_id, nn_mmnv_Setting min, nn_mmnv_Setting max) {
    return mm_set(request_id, 0, min);
}
static int nn_mmnv_IRequest_Get(SvcObj* self, u32 request_id, nn_mmnv_Setting* current) {
    return mm_get(request_id, 0, current);
}
SvcObj* new_mm(void) { return nn_mmnv_IRequest_new(0); }
static int nn_nfp_detail_IUser_AttachActivateEvent(SvcObj* self, const nn_nfc_DeviceHandle* device_handle, u32* event) {
    return new_event(event, "nfp", 0);
}
static int nn_nfp_detail_IUser_AttachDeactivateEvent(SvcObj* self, const nn_nfc_DeviceHandle* device_handle,
                                                     u32* event) {
    return new_event(event, "nfp", 0);
}
static int nn_nfp_detail_IUser_GetState(SvcObj* self, u32* state) {
    *state = 1; /* initialized */
    return 0;
}
static int nn_nfp_detail_IUserManager_CreateUserInterface(SvcObj* self, SvcObj** user) {
    *user = nn_nfp_detail_IUser_new(0);
    return 0;
}
SvcObj* new_nfp(void) { return nn_nfp_detail_IUserManager_new(0); }
SvcObj* new_caps(void) { return nn_capsrv_sf_IAlbumApplicationService_new(0); }
/* BCAT. behave like an offline console: sync completes immediately (Done/OK) and the delivery cache
   is empty. games do wait on the progress object, so it has to exist */
static int nn_bcat_detail_ipc_IDeliveryCacheProgressService_GetEvent(SvcObj* self, u32* event) {
    return new_event(event, "bcat progress", 1);
}
static int nn_bcat_detail_ipc_IDeliveryCacheProgressService_GetImpl(SvcObj* self, IpcBuf impl) { /* Done/OK */
    if (impl.addr && impl.size) {
        memset(GP(impl.addr), 0, impl.size);
        if (impl.size >= 8) {
            u32 st = 9, rs = 0;
            memcpy(GP(impl.addr), &st, 4);
            memcpy(GP(impl.addr) + 4, &rs, 4);
        }
    }
    return 0;
}
static int nn_bcat_detail_ipc_IDeliveryCacheFileService_Open(SvcObj* self, const nn_bcat_DirectoryName* directory_name,
                                                             const nn_bcat_FileName* file_name) {
    return RC(122, 2); /* not found */
}
static int nn_bcat_detail_ipc_IDeliveryCacheStorageService_CreateFileService(SvcObj* self, SvcObj** file) {
    *file = nn_bcat_detail_ipc_IDeliveryCacheFileService_new(0);
    return 0;
}
static int nn_bcat_detail_ipc_IDeliveryCacheStorageService_CreateDirectoryService(SvcObj* self, SvcObj** directory) {
    *directory = nn_bcat_detail_ipc_IDeliveryCacheDirectoryService_new(0);
    return 0;
}
static int sync_progress(SvcObj** progress) {
    *progress = nn_bcat_detail_ipc_IDeliveryCacheProgressService_new(0);
    return 0;
}
static int nn_bcat_detail_ipc_IBcatService_RequestSyncDeliveryCache(SvcObj* self, SvcObj** progress) {
    return sync_progress(progress);
}
static int nn_bcat_detail_ipc_IBcatService_RequestSyncDeliveryCacheWithDirectoryName(
    SvcObj* self, const nn_bcat_DirectoryName* directory_name, SvcObj** progress) {
    return sync_progress(progress);
}
static int nn_bcat_detail_ipc_IBcatService_RequestSyncDeliveryCacheWithApplicationId(SvcObj* self,
                                                                                     nn_ApplicationId application_id,
                                                                                     u32 unknown, SvcObj** progress) {
    return sync_progress(progress);
}
static int nn_bcat_detail_ipc_IBcatService_RequestSyncDeliveryCacheWithApplicationIdAndDirectoryName(
    SvcObj* self, nn_ApplicationId application_id, u32 unknown, const nn_bcat_DirectoryName* directory_name,
    SvcObj** progress) {
    return sync_progress(progress);
}
static int nn_bcat_detail_ipc_IBcatService_GetDeliveryCacheStorageUpdateNotifier(SvcObj* self,
                                                                                 nn_ApplicationId application_id,
                                                                                 SvcObj** notifier) {
    *notifier = nn_bcat_detail_ipc_INotifierService_new(0);
    return 0;
}
static int nn_bcat_detail_ipc_IBcatService_RequestSuspendDeliveryTask(SvcObj* self, nn_ApplicationId application_id,
                                                                      SvcObj** suspension) {
    *suspension = nn_bcat_detail_ipc_IDeliveryTaskSuspensionService_new(0);
    return 0;
}
static int nn_bcat_detail_ipc_INotifierService_GetEvent(SvcObj* self, u32* event) {
    return new_event(event, "bcat storage update", 0);
}
static int nn_bcat_detail_ipc_IServiceCreator_CreateBcatService(SvcObj* self, u64 pid, SvcObj** service) {
    *service = nn_bcat_detail_ipc_IBcatService_new(0);
    return 0;
}
static int nn_bcat_detail_ipc_IServiceCreator_CreateDeliveryCacheStorageService(SvcObj* self, u64 pid,
                                                                                SvcObj** service) {
    *service = nn_bcat_detail_ipc_IDeliveryCacheStorageService_new(0);
    return 0;
}
static int nn_bcat_detail_ipc_IServiceCreator_CreateDeliveryCacheStorageServiceWithApplicationId(
    SvcObj* self, nn_ApplicationId application_id, SvcObj** service) {
    *service = nn_bcat_detail_ipc_IDeliveryCacheStorageService_new(0);
    return 0;
}
SvcObj* new_bcat(void) { return nn_bcat_detail_ipc_IServiceCreator_new(0); }
static int nn_ec_IPurchaseEventManager_GetPurchasedEventReadableHandle(SvcObj* self, u32* event) {
    return new_event(event, "aoc purchase", 0);
}
static int nn_ec_IPurchaseEventManager_PopPurchasedProductInfo(SvcObj* self,
                                                               nn_ec_srv_detail_PurchasedProductInfo* info) {
    return RC(164, 400); /* nothing purchased */
}
static int nn_ec_IPurchaseEventManager_PopPurchasedProductInfoWithUid(SvcObj* self, const nn_account_Uid* uid,
                                                                      nn_ec_srv_detail_PurchasedProductInfo* info) {
    return RC(164, 400);
}
/* the first add-on content id of the game's */
static u64 aoc_base_id(void) { return (g_title_id & ~0xFFFull) + 0x1000; }
static int nn_aocsrv_detail_IAddOnContentManager_GetAddOnContentBaseIdByApplicationId(
    SvcObj* self, nn_ncm_ApplicationId application_id, u64* base_id) {
    *base_id = aoc_base_id();
    return 0;
}
static int nn_aocsrv_detail_IAddOnContentManager_GetAddOnContentBaseId(SvcObj* self, u64 pid, u64* base_id) {
    *base_id = aoc_base_id();
    return 0;
}
static int nn_aocsrv_detail_IAddOnContentManager_GetAddOnContentListChangedEvent(SvcObj* self, u32* event) {
    return new_event(event, "aoc", 0);
}
static int nn_aocsrv_detail_IAddOnContentManager_GetAddOnContentListChangedEventWithProcessId(SvcObj* self, u64 pid,
                                                                                              u32* event) {
    return new_event(event, "aoc", 0);
}
static int nn_aocsrv_detail_IAddOnContentManager_CreateEcPurchasedEventManager(SvcObj* self, SvcObj** manager) {
    *manager = nn_ec_IPurchaseEventManager_new(0);
    return 0;
}
static int nn_aocsrv_detail_IAddOnContentManager_CreatePermanentEcPurchasedEventManager(SvcObj* self, u64 pid,
                                                                                        SvcObj** manager) {
    *manager = nn_ec_IPurchaseEventManager_new(0);
    return 0;
}
/* offline: nifm's NotConnected, as acc's network requests answer */
static int nn_ec_IContentsServiceManager_RequestContentsAuthorizationToken(SvcObj* self, const u8* nonce, u64 pid,
                                                                           IpcBuf data, u32* event, SvcObj** async) {
    return RC(110, 300);
}
static int nn_aocsrv_detail_IAddOnContentManager_CreateContentsServiceManager(SvcObj* self, SvcObj** manager) {
    *manager = nn_ec_IContentsServiceManager_new(0);
    return 0;
}
SvcObj* new_aoc(void) { return nn_aocsrv_detail_IAddOnContentManager_new(0); }
/* the one shared memory, kept for the whole run */
static int nn_irsensor_IIrSensorServer_GetIrsensorSharedMemoryHandle(SvcObj* self, u64 aruid, u32* shared_memory) {
    KShmem* s = __atomic_load_n(&misc.irs_shm, __ATOMIC_ACQUIRE);
    if (!s) {
        KShmem* n = kshmem_new(0x8000, "irs");
        kobj_ref(&n->o);
        if (__atomic_compare_exchange_n(&misc.irs_shm, &s, n, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            s = n;
        else
            kobj_unref(&n->o); /* another thread made it first */
    }
    *shared_memory = h_new(&s->o);
    return 0;
}
SvcObj* new_irs(void) { return nn_irsensor_IIrSensorServer_new(0); }
static int nn_mii_detail_IStaticService_GetDatabaseService(SvcObj* self, i32 key_code, SvcObj** database) {
    *database = nn_mii_detail_IDatabaseService_new(0);
    return 0;
}
SvcObj* new_mii(void) { return nn_mii_detail_IStaticService_new(0); }
/* ldn: local wireless. Initialize and GetState follow the state in u[0] (an atomic: game threads call both),
   AttachStateChangeEvent gives an event */
static int nn_ldn_detail_IUserLocalCommunicationService_GetState(SvcObj* self, u32* state) {
    *state = __atomic_load_n(&self->u[0], __ATOMIC_RELAXED) ? 1 : 0; /* Initialized, or None */
    return 0;
}
static int nn_ldn_detail_IUserLocalCommunicationService_AttachStateChangeEvent(SvcObj* self, u32* event) {
    return new_event(event, "ldn state", 0);
}
static int nn_ldn_detail_IUserLocalCommunicationService_Initialize(SvcObj* self, u64 pid) {
    __atomic_store_n(&self->u[0], 1, __ATOMIC_RELAXED);
    return 0;
}
static int nn_ldn_detail_IUserLocalCommunicationService_InitializeWithVersion(SvcObj* self, i32 version, u64 pid) {
    __atomic_store_n(&self->u[0], 1, __ATOMIC_RELAXED);
    return 0;
}
static int nn_ldn_detail_IUserLocalCommunicationService_Finalize(SvcObj* self) {
    __atomic_store_n(&self->u[0], 0, __ATOMIC_RELAXED);
    return 0;
}
static int nn_ldn_detail_IUserServiceCreator_CreateUserLocalCommunicationService(SvcObj* self, SvcObj** service) {
    *service = nn_ldn_detail_IUserLocalCommunicationService_new(0);
    return 0;
}
SvcObj* new_ldn(void) { return nn_ldn_detail_IUserServiceCreator_new(0); }
static int nn_err_context_IWriterForApplication_CreateContextRegistrar(SvcObj* self, SvcObj** registrar) {
    *registrar = nn_err_context_IContextRegistrar_new(0);
    return 0;
}
SvcObj* new_ectx(void) { return nn_err_context_IWriterForApplication_new(0); }
static int nn_spl_detail_IGeneralInterface_GenerateRandomBytes(SvcObj* self, IpcBuf data) { return random_fill(data); }
SvcObj* new_spl(void) { return nn_spl_detail_IGeneralInterface_new(0); }

/* btm:u GetCore returns an interface object, which the SDK requires during initialization. BLE operations are not
   emulated: their events stay unsignaled, with the success flag the SDK requires */
static int ble_event(u8* success, u32* event, const char* name) {
    *success = 1;
    return new_event(event, name, 0);
}
static int nn_btm_IBtmUserCore_AcquireBleScanEvent(SvcObj* self, u8* success, u32* event) {
    return ble_event(success, event, "btm BLE scan");
}
static int nn_btm_IBtmUserCore_AcquireBleConnectionEvent(SvcObj* self, u8* success, u32* event) {
    return ble_event(success, event, "btm BLE connection");
}
static int nn_btm_IBtmUserCore_AcquireBlePairingEvent(SvcObj* self, u8* success, u32* event) {
    return ble_event(success, event, "btm BLE pairing");
}
static int nn_btm_IBtmUserCore_AcquireBleServiceDiscoveryEvent(SvcObj* self, u8* success, u32* event) {
    return ble_event(success, event, "btm BLE service discovery");
}
static int nn_btm_IBtmUserCore_AcquireBleMtuConfigEvent(SvcObj* self, u8* success, u32* event) {
    return ble_event(success, event, "btm BLE MTU");
}
static int nn_btm_IBtmUser_GetCore(SvcObj* self, SvcObj** core) {
    *core = nn_btm_IBtmUserCore_new(0);
    return 0;
}
SvcObj* new_btmu(void) { return nn_btm_IBtmUser_new(0); }
SvcObj* new_audin(void) { return nn_audio_detail_IAudioInManager_new(0); }
