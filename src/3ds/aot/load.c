/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/aot/ctr_module.h"
#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32)
#include "common/filesystem/host.h"
#include <stdint.h>
#else
#include <dlfcn.h>
#endif

/* The recompiled code is a shared library the host compiler built: a DLL on Windows, which has no dlopen */
#if defined(_WIN32)
static void* library_open(const char* path, char* error, size_t error_size) {
    wchar_t* wide = snx_host_wpath(path);
    HMODULE library = wide ? LoadLibraryW(wide) : NULL;
    if (!library)
        snprintf(error, error_size, "cannot load fresh native module: error %lu", (unsigned long)GetLastError());
    free(wide);
    return (void*)library;
}
static void* library_symbol(void* library, const char* name) {
    return (void*)(uintptr_t)GetProcAddress((HMODULE)library, name);
}
static void library_close(void* library) { FreeLibrary((HMODULE)library); }
#else
static void* library_open(const char* path, char* error, size_t error_size) {
    void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) snprintf(error, error_size, "cannot load fresh native module: %s", dlerror());
    return library;
}
static void* library_symbol(void* library, const char* name) { return dlsym(library, name); }
static void library_close(void* library) { dlclose(library); }
#endif

void snx_ctr_native_close(SnxCtrNative* native) {
    if (!native) return;
    if (native->library) library_close(native->library);
    *native = (SnxCtrNative){0};
}
bool snx_ctr_native_open(const char* path, const SnxCtrImage* image, SnxCtrNative* native, char* error,
                         size_t error_size) {
    if (!native) return false;
    *native = (SnxCtrNative){0};
    if (error_size) error[0] = 0;
    if (!path || !image) {
        snprintf(error, error_size, "invalid native module request");
        return false;
    }
    native->library = library_open(path, error, error_size);
    if (!native->library) return false;
    native->module = library_symbol(native->library, "snx_ctr_module_v1");
    if (!snx_ctr_module_compatible(native->module, image)) {
        snprintf(error, error_size, "native module ABI/profile/image identity mismatch");
        snx_ctr_native_close(native);
        return false;
    }
    return true;
}
