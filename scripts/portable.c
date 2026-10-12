/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 the ScarletNX contributors
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE   200809L
#define _FILE_OFFSET_BITS 64
#endif
#include "portable.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#define get_cwd _getcwd
#else
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define get_cwd getcwd
#endif

/* Track allocations for atexit cleanup, including fatal-error paths. */
typedef union Block Block;
union Block {
    struct {
        Block* prev;
        Block* next;
    } links;
    long double alignment;
    void* pointer_alignment;
};
static Block* blocks;
static int allocator_ready;

static void free_blocks(void) {
    while (blocks) {
        Block* next;
        next = blocks->links.next;
        free(blocks);
        blocks = next;
    }
}

void die(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

void* alloc(size_t size) {
    Block* block;
    if (!allocator_ready) {
        if (atexit(free_blocks)) die("cannot register memory cleanup");
        allocator_ready = 1;
    }
    if (size > (size_t)-1 - sizeof(Block)) die("allocation too large");
    block = (Block*)malloc(sizeof(Block) + size);
    if (!block) die("out of memory");
    block->links.prev = NULL;
    block->links.next = blocks;
    if (blocks) blocks->links.prev = block;
    blocks = block;
    return block + 1;
}

void release(void* ptr) {
    Block* block;
    if (!ptr) return;
    block = (Block*)ptr - 1;
    if (block->links.prev)
        block->links.prev->links.next = block->links.next;
    else
        blocks = block->links.next;
    if (block->links.next) block->links.next->links.prev = block->links.prev;
    free(block);
}

#ifdef _WIN32
static wchar_t* wide_string(const char* text) {
    int count;
    wchar_t* result;
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!count) die("invalid UTF-8");
    result = (wchar_t*)alloc((size_t)count * sizeof(wchar_t));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result, count))
        die("UTF-8 -> UTF-16 conversion failed");
    return result;
}

static char* narrow_string(const wchar_t* text) {
    int count;
    char* result;
    count = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (!count) die("UTF-16 -> UTF-8 conversion failed");
    result = (char*)alloc((size_t)count);
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, result, count, NULL, NULL)) die("UTF-16 -> UTF-8 conversion failed");
    return result;
}
#endif

void host_arguments(int* argc, char*** argv) {
#ifdef _WIN32
    wchar_t** wide;
    char** args;
    int i;
    wide = CommandLineToArgvW(GetCommandLineW(), argc);
    if (!wide) die("cannot read command line");
    args = (char**)alloc(((size_t)*argc + 1) * sizeof(char*));
    for (i = 0; i < *argc; ++i) args[i] = narrow_string(wide[i]);
    args[*argc] = NULL;
    LocalFree(wide);
    *argv = args;
#else
    (void)argc;
    (void)argv;
#endif
}

const char* environment(const char* name) {
#ifdef _WIN32
    wchar_t* key;
    wchar_t* value;
    char* result;
    DWORD length;
    key = wide_string(name);
    length = GetEnvironmentVariableW(key, NULL, 0);
    if (!length) {
        release(key);
        return NULL;
    }
    value = (wchar_t*)alloc((size_t)length * sizeof(wchar_t));
    if (!GetEnvironmentVariableW(key, value, length)) die("cannot read env var: %s", name);
    result = narrow_string(value);
    release(key);
    release(value);
    return result;
#else
    return getenv(name);
#endif
}

char* copy_string(const char* text) {
    char* result;
    result = (char*)alloc(strlen(text) + 1);
    strcpy(result, text);
    return result;
}

char* concat(const char* a, const char* b) {
    size_t na, nb;
    char* result;
    na = strlen(a);
    nb = strlen(b);
    if (nb > (size_t)-1 - na - 1) die("string too large");
    result = (char*)alloc(na + nb + 1);
    memcpy(result, a, na);
    memcpy(result + na, b, nb + 1);
    return result;
}

static int separator(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

char* join(const char* a, const char* b) {
    char* prefix;
    char* result;
    if (!*a) return copy_string(b);
    if (separator(b[0])) return copy_string(b);
#ifdef _WIN32
    if (b[0] && b[1] == ':') return copy_string(b);
#endif
    if (separator(a[strlen(a) - 1])) return concat(a, b);
    prefix = concat(a, "/");
    result = concat(prefix, b);
    release(prefix);
    return result;
}

char* parent_path(const char* path) {
    char* result;
    size_t n;
    result = copy_string(path);
    n = strlen(result);
    while (n > 1 && separator(result[n - 1])) --n;
    while (n && !separator(result[n - 1])) --n;
    if (!n) {
        release(result);
        return copy_string(".");
    }
    while (n > 1 && separator(result[n - 1])) {
#ifdef _WIN32
        if (n == 3 && result[1] == ':') break;
#endif
        --n;
    }
    result[n] = '\0';
    return result;
}

#ifndef _WIN32
/* Lexical normalization, like abspath: do not resolve filesystem symlinks. */
static char* normalize_absolute(const char* path) {
    char* result;
    const char* p;
    const char* start;
    size_t used, root, length;
    result = (char*)alloc(strlen(path) + 2);
    root = path[0] == '/' && path[1] == '/' && path[2] != '/' ? 2 : 1;
    used = root;
    result[0] = '/';
    if (root == 2) result[1] = '/';
    p = path;
    while (*p) {
        while (*p == '/') ++p;
        start = p;
        while (*p && *p != '/') ++p;
        length = (size_t)(p - start);
        if (!length || (length == 1 && start[0] == '.')) continue;
        if (length == 2 && start[0] == '.' && start[1] == '.') {
            while (used > root && result[used - 1] != '/') --used;
            if (used > root) --used;
        } else {
            if (used > root) result[used++] = '/';
            memcpy(result + used, start, length);
            used += length;
        }
    }
    result[used] = '\0';
    return result;
}
#endif

char* absolute_path(const char* path) {
#ifdef _WIN32
    DWORD length;
    wchar_t* input;
    wchar_t* buffer;
    char* result;
    input = wide_string(path);
    length = GetFullPathNameW(input, 0, NULL, NULL);
    if (!length) die("cannot resolve path: %s", path);
    buffer = (wchar_t*)alloc((size_t)length * sizeof(wchar_t));
    if (!GetFullPathNameW(input, length, buffer, NULL)) die("cannot resolve path: %s", path);
    result = narrow_string(buffer);
    release(input);
    release(buffer);
    return result;
#else
    size_t capacity;
    char* cwd;
    char* result;
    char* normalized;
    if (path[0] == '/') return normalize_absolute(path);
    capacity = 256;
    for (;;) {
        cwd = (char*)alloc(capacity);
        if (get_cwd(cwd, capacity)) break;
        release(cwd);
        if (errno != ERANGE || capacity > (size_t)-1 / 2) die("cannot get cwd");
        capacity *= 2;
    }
    result = join(cwd, path);
    release(cwd);
    normalized = normalize_absolute(result);
    release(result);
    return normalized;
#endif
}

int ends_with(const char* text, const char* suffix) {
    size_t a, b;
    a = strlen(text);
    b = strlen(suffix);
    return a >= b && strcmp(text + a - b, suffix) == 0;
}

#ifdef _WIN32
static DWORD attributes(const char* path) {
    wchar_t* wide;
    DWORD result;
    wide = wide_string(path);
    result = GetFileAttributesW(wide);
    release(wide);
    return result;
}
int path_exists(const char* path) { return attributes(path) != INVALID_FILE_ATTRIBUTES; }
int is_dir(const char* path) {
    DWORD a;
    a = attributes(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
int is_file(const char* path) { return path_exists(path) && !is_dir(path); }
int is_link(const char* path) {
    DWORD a;
    a = attributes(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
double file_size(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    wchar_t* wide;
    wide = wide_string(path);
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) die("cannot stat %s", path);
    release(wide);
    return (double)data.nFileSizeHigh * 4294967296.0 + (double)data.nFileSizeLow;
}
#else
int path_exists(const char* path) {
    struct stat s;
    return stat(path, &s) == 0;
}
int is_dir(const char* path) {
    struct stat s;
    return stat(path, &s) == 0 && S_ISDIR(s.st_mode);
}
int is_file(const char* path) {
    struct stat s;
    return stat(path, &s) == 0 && S_ISREG(s.st_mode);
}
int is_link(const char* path) {
    struct stat s;
    return lstat(path, &s) == 0 && S_ISLNK(s.st_mode);
}
double file_size(const char* path) {
    struct stat s;
    if (stat(path, &s)) die("cannot stat %s: %s", path, strerror(errno));
    return (double)s.st_size;
}
#endif

void make_dirs(const char* path) {
    char* parent;
    int rc;
    if (is_dir(path)) return;
    parent = parent_path(path);
    if (strcmp(parent, path) == 0) die("cannot create directory: %s", path);
    make_dirs(parent);
    release(parent);
#ifdef _WIN32
    {
        wchar_t* wide;
        wide = wide_string(path);
        rc = _wmkdir(wide);
        release(wide);
    }
#else
    rc = mkdir(path, 0777);
#endif
    if (rc && !is_dir(path)) die("cannot create directory %s: %s", path, strerror(errno));
}

void strings_add(Strings* list, const char* text) {
    char** items;
    size_t capacity;
    if (list->count + 1 >= list->capacity) {
        if (list->capacity > (size_t)-1 / sizeof(char*) / 2) die("too many strings");
        capacity = list->capacity ? list->capacity * 2 : 16;
        items = (char**)alloc(capacity * sizeof(char*));
        if (list->count) memcpy(items, list->items, list->count * sizeof(char*));
        release(list->items);
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = copy_string(text);
    list->items[list->count] = NULL;
}

void strings_free(Strings* list) {
    size_t i;
    for (i = 0; i < list->count; ++i) release(list->items[i]);
    release(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

Strings list_dir(const char* path, int required) {
    Strings result;
#ifdef _WIN32
    WIN32_FIND_DATAW entry;
    HANDLE handle;
    char* pattern;
    char* name;
    wchar_t* wide;
    DWORD error;
#else
    DIR* dir;
    struct dirent* entry;
    int error;
#endif
    result.items = NULL;
    result.count = 0;
    result.capacity = 0;
#ifdef _WIN32
    pattern = join(path, "*");
    wide = wide_string(pattern);
    handle = FindFirstFileW(wide, &entry);
    error = GetLastError();
    release(wide);
    release(pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        if (required && (!is_dir(path) || error != ERROR_FILE_NOT_FOUND)) die("cannot list %s", path);
        return result;
    }
    do {
        if (wcscmp(entry.cFileName, L".") && wcscmp(entry.cFileName, L"..")) {
            name = narrow_string(entry.cFileName);
            strings_add(&result, name);
            release(name);
        }
    } while (FindNextFileW(handle, &entry));
    error = GetLastError();
    FindClose(handle);
    if (required && error != ERROR_NO_MORE_FILES) die("cannot list %s", path);
#else
    dir = opendir(path);
    if (!dir) {
        if (required) die("cannot list %s: %s", path, strerror(errno));
        return result;
    }
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) strings_add(&result, entry->d_name);
        errno = 0;
    }
    error = errno;
    closedir(dir);
    if (required && error) die("cannot list %s: %s", path, strerror(error));
#endif
    return result;
}

/* Never follow directory symlinks or Windows junctions during cleanup. */
void remove_tree(const char* path) {
    Strings entries;
    size_t i;
    char* child;
    if (is_dir(path) && !is_link(path)) {
        entries = list_dir(path, 0);
        for (i = 0; i < entries.count; ++i) {
            child = join(path, entries.items[i]);
            remove_tree(child);
            release(child);
        }
        strings_free(&entries);
    }
#ifdef _WIN32
    {
        wchar_t* wide;
        wide = wide_string(path);
        if (is_dir(path))
            RemoveDirectoryW(wide);
        else
            DeleteFileW(wide);
        release(wide);
    }
#else
    if (is_dir(path) && !is_link(path))
        rmdir(path);
    else
        unlink(path);
#endif
}

char* user_home(void) {
    const char* home;
#ifdef _WIN32
    home = environment("USERPROFILE");
    if (!home || !*home) {
        const char* drive;
        const char* path;
        drive = environment("HOMEDRIVE");
        path = environment("HOMEPATH");
        if (path) return concat(drive ? drive : "", path);
    }
#else
    home = environment("HOME");
    if (!home) {
        struct passwd* account;
        account = getpwuid(getuid());
        if (account) home = account->pw_dir;
    }
#endif
    return copy_string(home ? home : "~");
}

char* temporary_parent(const char* out, double need) {
    char* system;
#ifdef _WIN32
    DWORD length;
    ULARGE_INTEGER available;
    wchar_t buffer[MAX_PATH + 1];
    length = GetTempPathW(MAX_PATH + 1, buffer);
    if (length && length <= MAX_PATH && GetDiskFreeSpaceExW(buffer, &available, NULL, NULL) &&
        (double)available.HighPart * 4294967296.0 + (double)available.LowPart >= need)
        return narrow_string(buffer);
    system = NULL;
#else
    const char* env;
    struct statvfs fs;
    env = getenv("TMPDIR");
    if (!env || !*env) env = getenv("TEMP");
    if (!env || !*env) env = getenv("TMP");
    system = absolute_path(env && *env ? env : "/tmp");
    if (statvfs(system, &fs) == 0 && access(system, W_OK | X_OK) == 0 &&
        (double)fs.f_bavail * (double)fs.f_frsize >= need)
        return system;
#endif
    release(system);
    return parent_path(out);
}

char* temporary_dir(const char* parent) {
    char* result;
#ifdef _WIN32
    unsigned long attempt;
    char suffix[80];
    /* CreateDirectory is atomic; never reuse an existing directory. */
    for (attempt = 0; attempt < 10000; ++attempt) {
        sprintf(suffix, ".extract-%lu-%lu-%lu", (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(),
                attempt);
        result = join(parent, suffix);
        {
            wchar_t* wide;
            BOOL created;
            DWORD error;
            wide = wide_string(result);
            created = CreateDirectoryW(wide, NULL);
            error = GetLastError();
            release(wide);
            if (created) return result;
            release(result);
            if (error != ERROR_ALREADY_EXISTS) break;
        }
    }
#else
    result = join(parent, ".extract-XXXXXX");
    if (mkdtemp(result)) return result;
    release(result);
#endif
    die("cannot create temp dir in %s", parent);
    return NULL;
}

FILE* open_file(const char* path, const char* mode) {
    FILE* file;
#ifdef _WIN32
    {
        wchar_t* wide_path;
        wchar_t* wide_mode;
        wide_path = wide_string(path);
        wide_mode = wide_string(mode);
        file = _wfopen(wide_path, wide_mode);
        release(wide_path);
        release(wide_mode);
    }
#else
    file = fopen(path, mode);
#endif
    if (!file) die("cannot open %s: %s", path, strerror(errno));
    return file;
}

void close_file(FILE* file, const char* path) {
    int failed;
    failed = ferror(file);
    if (fclose(file)) failed = 1;
    if (failed) die("I/O error: %s", path);
}

static char* read_text(FILE* file, int line) {
    size_t used, capacity;
    char* buffer;
    char* larger;
    int c;
    used = 0;
    capacity = 256;
    buffer = (char*)alloc(capacity);
    while ((c = fgetc(file)) != EOF) {
        if (used == capacity - 1) {
            if (capacity > (size_t)-1 / 2) die("input too large");
            capacity *= 2;
            larger = (char*)alloc(capacity);
            memcpy(larger, buffer, used);
            release(buffer);
            buffer = larger;
        }
        buffer[used++] = (char)c;
        if (line && c == '\n') break;
    }
    if (ferror(file)) die("error reading file");
    if (line && !used) {
        release(buffer);
        return NULL;
    }
    buffer[used] = '\0';
    return buffer;
}
char* read_stream(FILE* file) { return read_text(file, 0); }
char* read_line(FILE* file) { return read_text(file, 1); }

static int looks_like_repo(const char* path) {
    char* scripts;
    char* package;
    int yes;
    scripts = join(path, "scripts");
    package = join(path, "scarletnx");
    yes = is_dir(scripts) && is_dir(package);
    release(scripts);
    release(package);
    return yes;
}

static char* repo_above(const char* start) {
    char* dir;
    char* parent;
    dir = absolute_path(start);
    while (!looks_like_repo(dir)) {
        parent = parent_path(dir);
        if (!strcmp(parent, dir)) {
            release(parent);
            release(dir);
            return NULL;
        }
        release(dir);
        dir = parent;
    }
    return dir;
}

char* repo_path(const char* argv0) {
    const char* override;
    char* result;
    char* executable;
    char* parent;
    override = environment("SNX_REPO");
    if (override && *override) {
        result = absolute_path(override);
        if (!looks_like_repo(result)) die("SNX_REPO is not a checkout: %s", result);
        return result;
    }
    executable = NULL;
#ifdef _WIN32
    {
        DWORD n;
        size_t cap;
        wchar_t* buffer;
        cap = 256;
        for (;;) {
            buffer = (wchar_t*)alloc(cap * sizeof(wchar_t));
            n = GetModuleFileNameW(NULL, buffer, (DWORD)cap);
            if (!n) {
                release(buffer);
                break;
            }
            if (n < cap) {
                executable = narrow_string(buffer);
                release(buffer);
                break;
            }
            release(buffer);
            cap *= 2;
        }
    }
#else
    if (strchr(argv0, '/'))
        executable = absolute_path(argv0);
    else {
        const char* path;
        const char* end;
        char* part;
        char* candidate;
        size_t n;
        path = getenv("PATH");
        while (path) {
            end = strchr(path, ':');
            n = end ? (size_t)(end - path) : strlen(path);
            part = (char*)alloc(n + 1);
            memcpy(part, path, n);
            part[n] = '\0';
            candidate = join(*part ? part : ".", argv0);
            release(part);
            if (is_file(candidate) && access(candidate, X_OK) == 0) {
                executable = absolute_path(candidate);
                release(candidate);
                break;
            }
            release(candidate);
            path = end ? end + 1 : NULL;
        }
    }
#endif
    (void)argv0;
    if (executable) {
        parent = parent_path(executable);
        result = repo_above(parent);
        release(parent);
        release(executable);
        if (result) return result;
    }
#ifdef SNX_SOURCE_ROOT
    if (looks_like_repo(SNX_SOURCE_ROOT)) return absolute_path(SNX_SOURCE_ROOT);
#endif
    result = repo_above(".");
    if (!result) die("checkout not found; set SNX_REPO");
    return result;
}

#ifdef _WIN32
/* CRT argv quoting: preserve empty args, quotes + trailing backslashes. No shell. */
static char* command_line(char* const* args) {
    size_t i, n, slashes, j;
    char* result;
    char* out;
    n = 1;
    for (i = 0; args[i]; ++i) {
        if (strlen(args[i]) > ((size_t)-1 - n - 3) / 2) die("command too long");
        n += strlen(args[i]) * 2 + 3;
    }
    result = (char*)alloc(n);
    out = result;
    for (i = 0; args[i]; ++i) {
        const char* p;
        if (i) *out++ = ' ';
        *out++ = '"';
        p = args[i];
        for (;;) {
            slashes = 0;
            while (*p == '\\') {
                ++slashes;
                ++p;
            }
            n = (*p == '"' || !*p) ? slashes * 2 : slashes;
            for (j = 0; j < n; ++j) *out++ = '\\';
            if (!*p) break;
            if (*p == '"') *out++ = '\\';
            *out++ = *p++;
        }
        *out++ = '"';
    }
    *out = '\0';
    return result;
}

static int compare_environment(const void* a, const void* b) {
    return _wcsicmp(*(const wchar_t* const*)a, *(const wchar_t* const*)b);
}

static wchar_t* child_environment(const char* home) {
    LPWCH env;
    const wchar_t* p;
    wchar_t* result;
    wchar_t* out;
    wchar_t* wide_home;
    const wchar_t** entries;
    char* home_entry;
    size_t n, length, count, i;
    env = GetEnvironmentStringsW();
    if (!env) die("cannot read environment");
    home_entry = concat("HOME=", home);
    wide_home = wide_string(home_entry);
    release(home_entry);
    n = wcslen(wide_home) + 2;
    count = 1;
    for (p = env; *p; p += wcslen(p) + 1) {
        n += wcslen(p) + 1;
        ++count;
    }
    entries = (const wchar_t**)alloc(count * sizeof(wchar_t*));
    count = 0;
    for (p = env; *p; p += wcslen(p) + 1) {
        if (!_wcsnicmp(p, L"HOME=", 5) || !_wcsnicmp(p, L"XDG_CONFIG_HOME=", 16)) continue;
        entries[count++] = p;
    }
    entries[count++] = wide_home;
    qsort(entries, count, sizeof(wchar_t*), compare_environment);
    result = (wchar_t*)alloc(n * sizeof(wchar_t));
    out = result;
    for (i = 0; i < count; ++i) {
        length = wcslen(entries[i]) + 1;
        memcpy(out, entries[i], length * sizeof(wchar_t));
        out += length;
    }
    *out = L'\0';
    release(entries);
    release(wide_home);
    FreeEnvironmentStringsW(env);
    return result;
}

static HANDLE inheritable(HANDLE source, DWORD access_mode) {
    HANDLE result;
    SECURITY_ATTRIBUTES security;
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = NULL;
    security.bInheritHandle = TRUE;
    if (!source || source == INVALID_HANDLE_VALUE) {
        result = CreateFileA("NUL", access_mode, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, NULL);
        if (result == INVALID_HANDLE_VALUE) die("cannot open NUL");
    } else if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &result, 0, TRUE,
                                DUPLICATE_SAME_ACCESS))
        die("cannot duplicate child stream");
    return result;
}
#endif

Process run_process(char* const* args, const char* cwd, int quiet, int capture, const char* home) {
    Process result;
    FILE* output;
    FILE* errors;
#ifdef _WIN32
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    char* command;
    wchar_t* wide_command;
    wchar_t* wide_cwd;
    wchar_t* env;
    DWORD status, error;
    BOOL started;
#else
    pid_t pid, waited;
    int status;
#endif
    result.out = NULL;
    result.err = NULL;
    result.status = 1;
    output = capture ? tmpfile() : NULL;
    errors = capture ? tmpfile() : NULL;
    if (capture && (!output || !errors)) die("cannot create process capture files");
    if (quiet && !capture) {
#ifdef _WIN32
        output = open_file("NUL", "wb");
#else
        output = open_file("/dev/null", "wb");
#endif
    }
    fflush(NULL);
#ifdef _WIN32
    memset(&startup, 0, sizeof(startup));
    memset(&process, 0, sizeof(process));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inheritable(GetStdHandle(STD_INPUT_HANDLE), GENERIC_READ);
    startup.hStdOutput =
        inheritable(output ? (HANDLE)_get_osfhandle(_fileno(output)) : GetStdHandle(STD_OUTPUT_HANDLE), GENERIC_WRITE);
    startup.hStdError =
        inheritable(errors ? (HANDLE)_get_osfhandle(_fileno(errors)) : GetStdHandle(STD_ERROR_HANDLE), GENERIC_WRITE);
    command = command_line(args);
    env = home ? child_environment(home) : NULL;
    wide_command = wide_string(command);
    wide_cwd = cwd ? wide_string(cwd) : NULL;
    started = CreateProcessW(NULL, wide_command, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT, env, wide_cwd, &startup,
                             &process);
    error = GetLastError();
    release(command);
    release(wide_command);
    release(wide_cwd);
    release(env);
    CloseHandle(startup.hStdInput);
    CloseHandle(startup.hStdOutput);
    CloseHandle(startup.hStdError);
    if (!started) die("cannot run %s (Windows error %lu)", args[0], (unsigned long)error);
    status = 1;
    if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &status))
        die("cannot wait for %s", args[0]);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    result.status = status <= INT_MAX ? (int)status : 1;
#else
    pid = fork();
    if (pid < 0) die("cannot start %s: %s", args[0], strerror(errno));
    if (!pid) {
        if ((cwd && chdir(cwd)) || (home && (setenv("HOME", home, 1) || unsetenv("XDG_CONFIG_HOME"))) ||
            (output && dup2(fileno(output), STDOUT_FILENO) < 0) ||
            (errors && dup2(fileno(errors), STDERR_FILENO) < 0)) {
            fputs("cannot prepare child process\n", stderr);
            _exit(127);
        }
        if (output) fclose(output);
        if (errors) fclose(errors);
        execvp(args[0], args);
        fprintf(stderr, "cannot run %s: %s\n", args[0], strerror(errno));
        _exit(127);
    }
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) die("cannot wait for %s", args[0]);
    if (WIFEXITED(status))
        result.status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.status = 128 + WTERMSIG(status);
#endif
    if (capture) {
        if (fseek(output, 0, SEEK_SET) || fseek(errors, 0, SEEK_SET)) die("cannot rewind process capture");
        result.out = read_stream(output);
        result.err = read_stream(errors);
    }
    if (output) close_file(output, "process stdout");
    if (errors) close_file(errors, "process stderr");
    return result;
}

void process_free(Process* process) {
    release(process->out);
    release(process->err);
    process->out = NULL;
    process->err = NULL;
}

void require_process(char* const* args, const char* cwd, int quiet) {
    Process result;
    result = run_process(args, cwd, quiet, 0, NULL);
    if (result.status) die("%s failed (exit %d)", args[0], result.status);
    process_free(&result);
}
