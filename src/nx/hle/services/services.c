/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the events the services share, and the registration of the services in sm.c, am.c, set.c, time.c, acc.c,
   audio.c, misc.c, ldr.c and network/ */
#include "hle/services/services_int.h"
#include "hle/services/network/network.h"
#include "hle/services/acc.swipc.h"
#include "hle/services/am.swipc.h"
#include "hle/services/audio.swipc.h"
#include "hle/services/misc.swipc.h"
#include "hle/services/network/bsd.swipc.h"
#include "hle/services/network/dns.swipc.h"
#include "hle/services/network/nifm.swipc.h"
#include "hle/services/network/ssl.swipc.h"
#include "hle/services/set.swipc.h"
#include "hle/services/sm.swipc.h"
#include "hle/services/time.swipc.h"

/* ================================================================ shared events */
u32 ev_handle(KEvent* e) { return h_new(&e->o); }
int new_event(u32* event, const char* name, int signaled) {
    KEvent* e = kevent_new(name);
    e->signaled = signaled;
    *event = ev_handle(e);
    return 0;
}
/* an event the service keeps (for the whole run, or for an object's life, dropped in its destroy): its reference keeps
   it alive when the game closes the handles it was given */
KEvent* owned_event(const char* name) {
    KEvent* e = kevent_new(name);
    kobj_ref(&e->o);
    return e;
}

void svc_core_init(void) {
    am_init();
    audio_init();
    time_init();
    network_init();
    plat_thread(audio_clock, NULL, 64 * 1024);
    nn_sm_detail_IUserInterface_register(new_sm);
    svc_register("ldr:ro", new_ldr);
    svc_register("ro:1", new_ro1);
    nn_am_service_IApplicationProxyService_register(new_appletoe);
    nn_am_service_IAllSystemAppletProxiesService_register(new_appletae);
    nn_settings_ISettingsServer_register(new_set);
    nn_settings_ISystemSettingsServer_register(new_setsys);
    nn_timesrv_detail_service_IStaticService_register(new_time);
    nn_account_IAccountServiceForApplication_register(new_acc);
    nn_apm_IManager_register(new_apm);
    nn_lm_ILogService_register(new_lm);
    nn_pctl_detail_ipc_IParentalControlServiceFactory_register(new_pctl);
    nn_nifm_detail_IStaticService_register(new_nifm);
    nn_friends_detail_ipc_IServiceCreator_register(new_friend);
    nn_ns_detail_IServiceGetterInterface_register(new_ns);
    nn_audio_detail_IAudioRendererManager_register(new_audren);
    nn_audio_detail_IAudioOutManager_register(new_audout);
    nn_audio_detail_IAudioInManager_register(new_audin);
    nn_codec_detail_IHardwareOpusDecoderManager_register(new_hwopus);
    svc_register("bsd:u", new_bsd_u);
    svc_register("bsd:a", new_bsd_u);
    svc_register("bsd:s", new_bsd_s);
    nn_bsdsocket_cfg_ServerInterface_register(new_bsdcfg);
    nn_socket_resolver_IResolver_register(new_sfdnsres);
    nn_nsd_detail_IManager_register(new_nsd);
    nn_ssl_sf_ISslService_register(new_ssl);
    nn_prepo_detail_ipc_IPrepoService_register(new_prepo);
    nn_psm_IPsmServer_register(new_psm);
    nn_fatalsrv_IService_register(new_fatal);
    nn_spl_detail_IRandomInterface_register(new_csrng);
    nn_mmnv_IRequest_register(new_mm);
    nn_nfp_detail_IUserManager_register(new_nfp);
    nn_nfc_detail_IUserManager_register(new_nfc);
    nn_capsrv_sf_IAlbumApplicationService_register(new_caps);
    nn_bcat_detail_ipc_IServiceCreator_register(new_bcat);
    nn_aocsrv_detail_IAddOnContentManager_register(new_aoc);
    nn_irsensor_IIrSensorServer_register(new_irs);
    nn_mii_detail_IStaticService_register(new_mii);
    nn_ldn_detail_IUserServiceCreator_register(new_ldn);
    nn_err_context_IWriterForApplication_register(new_ectx);
    nn_spl_detail_IGeneralInterface_register(new_spl);
    nn_btm_IBtmUser_register(new_btmu);
}
