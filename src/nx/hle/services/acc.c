/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* acc: user accounts */
#include "hle/services/services_int.h"
#include "hle/services/acc.swipc.inc"
/* ================================================================ acc */
/* the one user's profile: no image, so its nickname and id only */
static void profile_base(nn_account_profile_ProfileBase* base) {
    memcpy(base->uid.v, g_user_id, 16);
    base->last_edit_timestamp = 1600000000;
    strcpy((char*)base->nickname, "Player");
}
static int nn_account_profile_IProfile_Get(SvcObj* self, IpcBuf user_data, nn_account_profile_ProfileBase* base) {
    if (user_data.addr) memset(GP(user_data.addr), 0, user_data.size);
    profile_base(base);
    return 0;
}
static int nn_account_profile_IProfile_GetBase(SvcObj* self, nn_account_profile_ProfileBase* base) {
    profile_base(base);
    return 0;
}
/* the account list of the user commands (ListAllUsers, ListOpenUsers, ListQualifiedUsers...): the one user */
static int list_users(IpcBuf users) {
    if (users.addr) {
        memset(GP(users.addr), 0, users.size);
        if (users.size >= 16) memcpy(GP(users.addr), g_user_id, 16);
    }
    return 0;
}
static int nn_account_detail_IAsyncContext_GetSystemEvent(SvcObj* self, u32* event) {
    return new_event(event, "acc async", 1);
}
static int nn_account_detail_IAsyncContext_HasDone(SvcObj* self, u8* done) {
    *done = 1;
    return 0;
}
static int nn_account_detail_IAsyncContext_GetResult(SvcObj* self) { return RC(110, 300); }
static int nn_account_baas_IManagerForApplication_GetAccountId(SvcObj* self, nn_account_NetworkServiceAccountId* id) {
    *id = 0x0123456789ABCDEFull;
    return 0;
}
static int nn_account_baas_IManagerForApplication_EnsureIdTokenCacheAsync(SvcObj* self, SvcObj** context) {
    *context = nn_account_detail_IAsyncContext_new(0);
    return 0;
}
static int nn_account_IAccountServiceForApplication_GetUserCount(SvcObj* self, i32* count) {
    *count = 1;
    return 0;
}
static int nn_account_IAccountServiceForApplication_GetUserExistence(SvcObj* self, const nn_account_Uid* uid,
                                                                     u8* exists) {
    *exists = 1;
    return 0;
}
static int nn_account_IAccountServiceForApplication_ListAllUsers(SvcObj* self, IpcBuf users) {
    return list_users(users);
}
static int nn_account_IAccountServiceForApplication_ListOpenUsers(SvcObj* self, IpcBuf users) {
    return list_users(users);
}
static int nn_account_IAccountServiceForApplication_GetLastOpenedUser(SvcObj* self, nn_account_Uid* uid) {
    memcpy(uid->v, g_user_id, 16);
    return 0;
}
static int nn_account_IAccountServiceForApplication_GetProfile(SvcObj* self, const nn_account_Uid* uid,
                                                               SvcObj** profile) {
    *profile = nn_account_profile_IProfile_new(0);
    return 0;
}
static int nn_account_IAccountServiceForApplication_TrySelectUserWithoutInteraction(
    SvcObj* self, u8 is_network_service_account_required, nn_account_Uid* uid) {
    memcpy(uid->v, g_user_id, 16);
    return 0;
}
static int nn_account_IAccountServiceForApplication_GetBaasAccountManagerForApplication(SvcObj* self,
                                                                                        const nn_account_Uid* uid,
                                                                                        SvcObj** manager) {
    *manager = nn_account_baas_IManagerForApplication_new(0);
    return 0;
}
static int nn_account_IAccountServiceForApplication_LoadOpenContext(SvcObj* self, const nn_account_Uid* uid,
                                                                    SvcObj** manager) {
    *manager = nn_account_baas_IManagerForApplication_new(0);
    return 0;
}
static int nn_account_IAccountServiceForApplication_ListOpenContextStoredUsers(SvcObj* self, IpcBuf users) {
    return list_users(users);
}
static int nn_account_IAccountServiceForApplication_ListQualifiedUsers(SvcObj* self, IpcBuf users) {
    return list_users(users);
}
SvcObj* new_acc(void) { return nn_account_IAccountServiceForApplication_new(0); }
