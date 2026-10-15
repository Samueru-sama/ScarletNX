/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* set and set:sys: system settings */
#include "hle/services/services_int.h"
#include "hle/services/set.swipc.inc"
/* ================================================================ set */
/* the LanguageCode list in index order (nn::settings::Language), used by am's GetDesiredLanguage too */
static const char* const g_lang_codes[] = {"ja",    "en-US", "fr",     "de",      "it",      "es",
                                           "zh-CN", "ko",    "nl",     "pt",      "ru",      "zh-TW",
                                           "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
#define NLANG ((int)(sizeof g_lang_codes / sizeof g_lang_codes[0]))
static void lang_code(int i, u8 code[8]) {
    memset(code, 0, 8);
    memcpy(code, g_lang_codes[i], strlen(g_lang_codes[i]));
}
void set_language_code(int lang, u8 code[8]) { lang_code(lang >= 0 && lang < NLANG ? lang : 1, code); }
int set_language_count(void) { return NLANG; }
static int nn_settings_ISettingsServer_GetLanguageCode(SvcObj* self, nn_settings_LanguageCode* code) {
    set_language_code(g_opt_lang, code->v);
    return 0;
}
/* the language codes, in index order, as many as the buffer holds up to max */
static i32 language_codes(IpcBuf codes, int max) {
    u64 i, n = codes.size / 8;
    if (n > (u64)max) n = (u64)max;
    for (i = 0; i < n; i++) {
        nn_settings_LanguageCode c;
        lang_code((int)i, c.v);
        memcpy(GP(codes.addr + i * 8), c.v, 8);
    }
    return (i32)n;
}
/* the 15 languages of the first firmwares */
static int nn_settings_ISettingsServer_GetAvailableLanguageCodes(SvcObj* self, IpcBuf codes, i32* count) {
    *count = language_codes(codes, 15);
    return 0;
}
static int nn_settings_ISettingsServer_MakeLanguageCode(SvcObj* self, u32 language, nn_settings_LanguageCode* code) {
    if (language < NLANG) lang_code((int)language, code->v); /* else zeros */
    return 0;
}
static int nn_settings_ISettingsServer_GetAvailableLanguageCodeCount(SvcObj* self, i32* count) {
    *count = 15;
    return 0;
}
static int nn_settings_ISettingsServer_GetRegionCode(SvcObj* self, i32* region) {
    *region = 1; /* usa */
    return 0;
}
static int nn_settings_ISettingsServer_GetAvailableLanguageCodes2(SvcObj* self, IpcBuf codes, i32* count) {
    *count = language_codes(codes, NLANG);
    return 0;
}
static int nn_settings_ISettingsServer_GetAvailableLanguageCodeCount2(SvcObj* self, i32* count) {
    *count = NLANG;
    return 0;
}
static void zero_buffer(IpcBuf b) {
    if (b.addr) memset(GP(b.addr), 0, b.size);
}
static int nn_settings_ISettingsServer_GetKeyCodeMap(SvcObj* self, IpcBuf map) {
    zero_buffer(map);
    return 0;
}
static int nn_settings_ISettingsServer_GetKeyCodeMap2(SvcObj* self, IpcBuf map) {
    zero_buffer(map);
    return 0;
}
static int nn_settings_ISettingsServer_GetDeviceNickName(SvcObj* self, IpcBuf name) {
    static const char nick[] = "ScarletNX";
    if (!name.addr || !name.size) return 0;
    memset(GP(name.addr), 0, name.size);
    memcpy(GP(name.addr), nick, name.size < sizeof nick ? name.size - 1 : sizeof nick);
    return 0;
}
SvcObj* new_set(void) { return nn_settings_ISettingsServer_new(0); }
static int firmware_version(IpcBuf version) {
    u8 fw[0x100];
    memset(fw, 0, sizeof fw);
    /* FirmwareVersion: major, minor, micro, padding, revision major and minor, padding, then the platform (0x20),
       the version hash (0x40, left empty: no build of the system is behind it), the display version (0x18) and
       the display title (0x80) */
    fw[0] = 12;
    fw[1] = 1;
    fw[2] = 0;
    fw[4] = 1; /* the "-1.0" of 12.1.0-1.0 */
    fw[5] = 0;
    strcpy((char*)fw + 8, "NX");
    strcpy((char*)fw + 0x68, "12.1.0");
    strcpy((char*)fw + 0x80, "NintendoSDK Firmware for NX 12.1.0-1.0");
    if (version.addr) memcpy(GP(version.addr), fw, version.size < 0x100 ? version.size : 0x100);
    return 0;
}
static int nn_settings_ISystemSettingsServer_GetFirmwareVersion(SvcObj* self, IpcBuf version) {
    return firmware_version(version);
}
static int nn_settings_ISystemSettingsServer_GetFirmwareVersion2(SvcObj* self, IpcBuf version) {
    return firmware_version(version);
}
/* no setting is there: the value is empty */
static int nn_settings_ISystemSettingsServer_GetSettingsItemValue(SvcObj* self, IpcBuf name, IpcBuf key, IpcBuf value,
                                                                  u64* size) {
    zero_buffer(value);
    return 0;
}
SvcObj* new_setsys(void) { return nn_settings_ISystemSettingsServer_new(0); }
