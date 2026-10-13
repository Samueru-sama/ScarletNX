/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later (LICENSE). */
#define _POSIX_C_SOURCE 200809L
#include "toolchain.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef struct {
    const char** flags;
    size_t flag_count, count, next, done;
    char **sources, **objects;
    int failed, quiet;
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} CompileQueue;

static void lock_queue(CompileQueue* q) {
#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif
}
static void unlock_queue(CompileQueue* q) {
#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif
}
static void diagnostic(const char* stage, const char* output) {
    size_t n = output ? strlen(output) : 0;
    snx_error("%s failed%s%s", stage, n ? ":\n" : "", n ? output + (n > 4000 ? n - 4000 : 0) : "");
}
static void compile_worker(CompileQueue* q) {
    for (;;) {
        size_t i, k;
        char *output = NULL, *tmp;
        const char** args;
        int rc = 0;
        lock_queue(q);
        if (q->failed || q->next == q->count) {
            unlock_queue(q);
            break;
        }
        i = q->next++;
        unlock_queue(q);
        if (!snx_is_file(q->objects[i])) {
            tmp = snx_format("%s.tmp", q->objects[i]);
            args = (const char**)snx_alloc((q->flag_count + 5) * sizeof(*args));
            for (k = 0; k < q->flag_count; ++k) args[k] = q->flags[k];
            args[k++] = "-c";
            args[k++] = q->sources[i];
            args[k++] = "-o";
            args[k++] = tmp;
            args[k] = NULL;
            rc = snx_run(args, NULL, NULL, &output);
            if (!rc) rc = snx_replace(tmp, q->objects[i]);
            if (rc && snx_is_file(tmp)) snx_remove_tree(tmp);
            free(tmp);
            free(args);
        }
        lock_queue(q);
        ++q->done;
        if (rc && !q->failed) {
            diagnostic("Compilation", output);
            q->failed = 1;
        }
        if (!q->quiet && (q->done % 10 == 0 || q->done == q->count))
            printf("  compiled %lu/%lu\n", (unsigned long)q->done, (unsigned long)q->count);
        unlock_queue(q);
        free(output);
    }
}
#ifdef _WIN32
static DWORD WINAPI thread_worker(LPVOID p) {
    compile_worker((CompileQueue*)p);
    return 0;
}
#else
static void* thread_worker(void* p) {
    compile_worker((CompileQueue*)p);
    return NULL;
}
#endif
static void run_queue(CompileQueue* q, unsigned jobs) {
    unsigned i, created = 0;
#ifdef _WIN32
    HANDLE* threads;
#else
    pthread_t* threads;
#endif
    if ((size_t)jobs > q->count) jobs = (unsigned)q->count;
    if (jobs < 1) jobs = 1;
    threads = snx_alloc(jobs * sizeof(*threads));
#ifdef _WIN32
    InitializeCriticalSection(&q->lock);
#else
    pthread_mutex_init(&q->lock, NULL);
#endif
    for (i = 1; i < jobs; ++i) {
#ifdef _WIN32
        HANDLE h = CreateThread(NULL, 0, thread_worker, q, 0, NULL);
        if (!h) break;
        threads[created++] = h;
#else
        if (pthread_create(&threads[created], NULL, thread_worker, q)) break;
        ++created;
#endif
    }
    compile_worker(q);
    for (i = 0; i < created; ++i) {
#ifdef _WIN32
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], NULL);
#endif
    }
#ifdef _WIN32
    DeleteCriticalSection(&q->lock);
#else
    pthread_mutex_destroy(&q->lock);
#endif
    free(threads);
}
static SnxU64 hash_bytes(SnxU64 h, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    size_t i;
    for (i = 0; i < n; ++i) {
        h ^= p[i];
        h *= ((SnxU64)0x100 << 32) | 0x1b3;
    }
    return h;
}
static int hash_file(SnxU64* h, const char* path) {
    size_t n;
    unsigned char* data = snx_read_file(path, &n);
    if (!data) return 1;
    *h = hash_bytes(*h, data, n);
    free(data);
    return 0;
}
static int hash_abi(SnxU64* h, const char* root) {
    char **names, *path;
    size_t i, n, len;
    int rc = 0;
    if (snx_list_dir(root, &names, &n)) return 1;
    for (i = 0; i < n && !rc; ++i) {
        path = snx_path_join(root, names[i]);
        if (snx_is_dir(path))
            rc = snx_is_link(path) || hash_abi(h, path);
        else {
            len = strlen(names[i]);
            if (len > 2 && names[i][len - 2] == '.' && (names[i][len - 1] == 'c' || names[i][len - 1] == 'h')) {
                *h = hash_bytes(*h, names[i], len + 1);
                rc = hash_file(h, path);
            }
        }
        free(path);
    }
    snx_free_strings(names, n);
    return rc;
}
/* SNX_CFLAGS is whitespace-separated, matching the previous CLI. It is never a shell command. */
static char** extra_flags(size_t* count) {
    const char *s = getenv("SNX_CFLAGS"), *start;
    char** flags = NULL;
    size_t n = 0, len;
    if (s)
        while (*s) {
            while (*s && isspace((unsigned char)*s)) ++s;
            if (!*s) break;
            start = s;
            while (*s && !isspace((unsigned char)*s)) ++s;
            len = (size_t)(s - start);
            flags = (char**)snx_realloc(flags, (n + 1) * sizeof(*flags));
            flags[n] = (char*)snx_alloc(len + 1);
            memcpy(flags[n], start, len);
            flags[n++][len] = 0;
        }
    *count = n;
    return flags;
}
static int response_name(FILE* f, const char* path) {
    const char* p;
    fputc('"', f);
    for (p = path; *p; ++p) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
    fputs("\"\n", f);
    return ferror(f);
}
static void prune_objects(const CompileQueue* q) {
    char* dir = snx_dirname(q->sources[0]);
    char **names, *path;
    const char *base, *p, *signature;
    size_t count, i, j, prefix, len;
    if (snx_list_dir(dir, &names, &count)) {
        free(dir);
        return;
    }
    for (i = 0; i < q->count; ++i) {
        base = q->sources[i];
        for (p = base; *p; ++p)
            if (*p == '/' || *p == '\\') base = p + 1;
        prefix = strlen(base);
        if (prefix < 2) continue;
        prefix -= 2;
        for (j = 0; j < count; ++j) {
            len = strlen(names[j]);
            if (len < prefix + 4 || strncmp(names[j], base, prefix) || names[j][prefix] != '.' ||
                strcmp(names[j] + len - 2, ".o"))
                continue;
            signature = names[j] + prefix + 1;
            p = signature;
            while (isxdigit((unsigned char)*p)) ++p;
            if (strcmp(p, ".o") || ((size_t)(p - signature) != 10 && (size_t)(p - signature) != 16)) continue;
            path = snx_path_join(dir, names[j]);
            if (strcmp(path, q->objects[i]) && snx_is_file(path) && !snx_is_link(path)) snx_remove_tree(path);
            free(path);
        }
    }
    snx_free_strings(names, count);
    free(dir);
}
int snx_compile_sources(char** sources, size_t count, const char* output, const SnxPaths* paths,
                        const SnxCompileOptions* o) {
    char *cc, *abi, *opt, *version = NULL, *dir, *rsp, *rsp_arg, *tmp, **extra;
    const char **flags, *args[9];
    CompileQueue q;
    size_t i, nf = 0, ne, len;
    SnxU64 signature = ((SnxU64)0xcbf29ce4 << 32) | 0x84222325, h;
    int rc = 1;
    FILE* f;
    memset(&q, 0, sizeof(q));
    if (!count) return snx_error("no generated sources");
    if (o->cc)
        cc = snx_find_tool(o->cc);
    else {
        cc = o->target_win ? snx_find_tool("x86_64-w64-mingw32-clang") : snx_find_tool("clang");
#ifdef _WIN32
        if (!cc) cc = snx_find_tool("clang");
#endif
    }
    if (!cc) return snx_error("Clang not found%s%s", o->cc ? ": " : "", o->cc ? o->cc : "");
    args[0] = cc;
    args[1] = "--version";
    args[2] = NULL;
    if (snx_run(args, NULL, NULL, &version) || !strstr(version, "clang")) {
        free(version);
        free(cc);
        return snx_error("the game compiler must be Clang");
    }
    signature = hash_bytes(signature, version, strlen(version));
    free(version);
    version = NULL;
    args[1] = "-dumpmachine";
    if (snx_run(args, NULL, NULL, &version) || !strstr(version, "x86_64") ||
        ((strstr(version, "windows") != NULL || strstr(version, "mingw") != NULL || strstr(version, "win32") != NULL) !=
         o->target_win)) {
        free(version);
        free(cc);
        return snx_error("Clang must target x86_64 and match --target; select a cross compiler with --cc");
    }
    free(version);
    version = NULL;
    abi = snx_path_join(paths->repo, "runtime/abi");
    opt = snx_format("-O%s", o->opt);
    extra = extra_flags(&ne);
    flags = (const char**)snx_alloc((ne + 20) * sizeof(*flags));
    flags[nf++] = cc;
    flags[nf++] = opt;
    flags[nf++] = "-pipe";
    flags[nf++] = "-march=native";
    flags[nf++] = "-mtune=native";
    flags[nf++] = "-fno-strict-aliasing";
    flags[nf++] = "-fwrapv";
    flags[nf++] = "-w";
    flags[nf++] = "-I";
    flags[nf++] = abi;
    /* The ABI's 128-bit exclusives need CMPXCHG16B, not an unresolved libatomic call. */
    flags[nf++] = "-mcx16";
    flags[nf++] = "-foptimize-sibling-calls";
    flags[nf++] = "-fno-semantic-interposition";
    if (!o->target_win) {
        flags[nf++] = "-fPIC";
        flags[nf++] = "-fvisibility=hidden";
    }
    for (i = 0; i < ne; ++i) flags[nf++] = extra[i];
    for (i = 0; i < nf; ++i) signature = hash_bytes(signature, flags[i], strlen(flags[i]) + 1);
    if (hash_abi(&signature, abi)) {
        snx_error("cannot read ABI headers in %s", abi);
        goto done;
    }
    q.flags = flags;
    q.flag_count = nf;
    q.sources = sources;
    q.count = count;
    q.quiet = o->quiet;
    q.objects = (char**)snx_alloc(count * sizeof(*q.objects));
    memset(q.objects, 0, count * sizeof(*q.objects));
    for (i = 0; i < count; ++i) {
        h = signature;
        if (hash_file(&h, sources[i])) {
            snx_error("cannot read %s", sources[i]);
            goto done;
        }
        len = strlen(sources[i]);
        q.objects[i] = snx_format("%.*s.%08lx%08lx.o", (int)(len > 2 ? len - 2 : len), sources[i],
                                  (unsigned long)(SnxU32)(h >> 32), (unsigned long)(SnxU32)h);
    }
    run_queue(&q, o->jobs);
    if (q.failed) goto done;
    dir = snx_dirname(sources[0]);
    rsp = snx_path_join(dir, "objects.rsp");
    free(dir);
    f = snx_fopen(rsp, "wb");
    if (!f) {
        snx_error("cannot write %s", rsp);
        free(rsp);
        goto done;
    }
    rc = 0;
    for (i = 0; i < count; ++i)
        if (response_name(f, q.objects[i])) rc = 1;
    if (fclose(f)) rc = 1;
    if (rc) {
        free(rsp);
        goto done;
    }
    rsp_arg = snx_format("@%s", rsp);
    tmp = snx_format("%s.tmp", output);
    args[0] = cc;
    args[1] = "-shared";
    args[2] = "-o";
    args[3] = tmp;
    args[4] = rsp_arg;
    args[5] = o->target_win ? "-static-libgcc" : "-lm";
    /* Large generated modules can exceed the reach of the linker's GOTPCREL relaxation. */
    args[6] = o->target_win ? NULL : "-Wl,--no-relax";
    args[7] = NULL;
    rc = snx_run(args, NULL, NULL, &version);
    if (!rc) rc = snx_replace(tmp, output);
    if (rc) {
        diagnostic("Linking", version);
        if (snx_is_file(tmp)) snx_remove_tree(tmp);
    }
    snx_remove_tree(rsp);
    free(version);
    free(rsp);
    free(rsp_arg);
    free(tmp);
    if (!rc) prune_objects(&q);
done:
    snx_free_strings(q.objects, q.objects ? count : 0);
    snx_free_strings(extra, ne);
    free(flags);
    free(abi);
    free(opt);
    free(cc);
    return rc;
}
char* snx_build_runtime(const SnxPaths* paths, int target_win, int quiet) {
    char *bdir, *runtime, *output = NULL;
    const char *preset = "default", *args[10];
    int rc;
#ifndef _WIN32
    if (target_win) preset = "windows-cross";
#endif
    bdir = snx_path_join(paths->repo, target_win ? "build/win64-recomp" : "build/native-recomp");
    if (!quiet) printf("Building runtime in %s\n", bdir);
    args[0] = "cmake";
    args[1] = "--preset";
    args[2] = preset;
    args[3] = "-B";
    args[4] = bdir;
    args[5] = "-DSNX_DEPLOY=OFF";
    args[6] = "-DSNX_TOOLS_ONLY=OFF";
    args[7] = NULL;
    rc = snx_run(args, paths->repo, NULL, &output);
    if (!rc) {
        free(output);
        output = NULL;
        args[1] = "--build";
        args[2] = bdir;
        args[3] = NULL;
        rc = snx_run(args, paths->repo, NULL, &output);
    }
    if (rc) {
        diagnostic("Runtime build", output);
        runtime = NULL;
    } else
        runtime = snx_path_join(bdir, target_win ? "scarletnx.exe" : "scarletnx");
    free(bdir);
    free(output);
    return runtime;
}
static int scan_shaders(const char* shaderc, const char* dir, const char* cache, int quiet) {
    char **names, *path, *output;
    const char* args[6];
    size_t n, i;
    unsigned char magic[4];
    int found = 0;
    FILE* f;
    if (snx_list_dir(dir, &names, &n)) return 1;
    for (i = 0; i < n; ++i) {
        path = snx_path_join(dir, names[i]);
        if (snx_is_link(path)) {
            free(path);
            continue;
        }
        if (snx_is_dir(path))
            scan_shaders(shaderc, path, cache, quiet);
        else if (!found && (f = snx_fopen(path, "rb")) != NULL) {
            if (fread(magic, 1, 4, f) == 4 && !memcmp(magic, "\x91\x68\x86\x19", 4)) found = 1;
            fclose(f);
        }
        free(path);
    }
    snx_free_strings(names, n);
    if (found) {
        if (snx_mkdirs(cache)) return 1;
        if (!quiet) printf("Prebuilding shaders: %s\n", dir);
        args[0] = shaderc;
        args[1] = "--nvn";
        args[2] = dir;
        args[3] = "--cache";
        args[4] = cache;
        args[5] = NULL;
        output = NULL;
        if (snx_run(args, NULL, NULL, &output))
            fputs("Shader prebuild failed; shaders will compile at runtime.\n", stderr);
        free(output);
    }
    return 0;
}
int snx_prebuild_shaders(const SnxPaths* paths, const char* romfs, const char* outdir, int quiet) {
    char *shaderc, *cache;
    int rc = 0;
#ifdef _WIN32
    shaderc = snx_path_join(paths->build, "snx-shaderc.exe");
#else
    shaderc = snx_path_join(paths->build, "snx-shaderc");
#endif
    cache = snx_path_join(outdir, "shader_cache/spirv");
    if (snx_is_file(shaderc) && snx_is_dir(romfs)) rc = scan_shaders(shaderc, romfs, cache, quiet);
    free(shaderc);
    free(cache);
    return rc;
}
