/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* fs: RomFS (built on the fly from an extracted romfs/ folder, or a romfs.bin), plus save data
   and SD card mapped to normal folders. */
#include "hle/filesystem/fs.h"
#include "hle/filesystem/fs_tree.h"
#include "runtime/main.h"
#include "hle/services/services.h"
#include "hle/kernel/ipc.h"
#include "memory/mem.h"
#include "runtime/platform/backend.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/* nn::fs results */
#define FS_NOTFOUND            RC(2, 1)
#define FS_EXISTS              RC(2, 2)
#define FS_DIR_NOT_EMPTY       RC(2, 8)
#define FS_NO_SPACE            RC(2, 30) /* UsableSpaceNotEnough */
#define FS_OUT_OF_RANGE        RC(2, 3005)
#define FS_PATH_TOO_LONG       RC(2, 6003)
#define FS_DIR_UNOBTAINABLE    RC(2, 6006) /* a ".." above the root */
#define FS_INVALID_OFFSET      RC(2, 6061)
#define FS_INVALID_SIZE        RC(2, 6062)
#define FS_INVALID_OPEN_MODE   RC(2, 6072)
#define FS_NO_APPEND           RC(2, 6201) /* FileExtensionWithoutOpenModeAllowAppend */
#define FS_READ_NOT_PERMITTED  RC(2, 6202)
#define FS_WRITE_NOT_PERMITTED RC(2, 6203)

/* ============================================================ RomFS virtual */
typedef struct {
    FsNode* node;
    u64 off, size;
    int logged;
} RFile;
typedef struct DNode {
    char* name;
    int parent;
    int first_child, next_sib, first_file;
    u32 off;
} DNode;
typedef struct FNode {
    char* name;
    int parent, next_sib;
    u32 off;
    u64 size;
    FsNode* node;
} FNode;
#define NOPEN 48
static struct {
    FsTree tree;
    RFile* rfiles; /* in offset order: romfs_build lays the files out in this order */
    int nrfiles;
    u8* rmeta;
    u64 rmeta_size; /* header + tables sit at the start of the image */
    u64 rdata_off, rtotal;
    FILE* rraw;     /* plain romfs.bin, when it couldn't be mapped */
    FsNode* rimage; /* plain romfs.bin, mapped */
    DNode* dn;
    int ndn, capdn;
    FNode* fn;
    int nfn, capfn;
    /* cache of open host files */
    struct {
        FsNode* node;
        FILE* f;
        u64 last;
    } openf[NOPEN];
    u64 openclk;
    u8 emptyrom[0x200]; /* the empty RomFS of the system archives (emptyrom_init) */
} fsp;

static u32 tree_result(FsTreeResult r) {
    switch (r) {
        case FS_TREE_OK: return 0;
        case FS_TREE_NOTFOUND: return FS_NOTFOUND;
        case FS_TREE_EXISTS: return FS_EXISTS;
        case FS_TREE_NOTEMPTY: return FS_DIR_NOT_EMPTY;
        case FS_TREE_NOTDELETABLE: return RC(2, 6031);
        case FS_TREE_NOTRENAMABLE: return RC(2, 6032);
        case FS_TREE_INVALID: return FS_DIR_UNOBTAINABLE;
        case FS_TREE_TOOLONG: return FS_PATH_TOO_LONG;
        case FS_TREE_READONLY: return FS_WRITE_NOT_PERMITTED;
        default: return FS_NO_SPACE;
    }
}

void fs_init(void) {
    FsTreeResult r = fs_tree_init(&fsp.tree, g_datadir);
    if (r != FS_TREE_OK) fatal("Could not index game filesystem %s (error %d)", g_datadir, r);
    size_t n = strlen(fsp.tree.base);
    if (n + 2 > sizeof g_datadir) fatal("Game filesystem path is too long");
    memcpy(g_datadir, fsp.tree.base, n);
    if (n && g_datadir[n - 1] != '/' && g_datadir[n - 1] != '\\') g_datadir[n++] = '/';
    g_datadir[n] = 0;
    /* RomFS and ExeFS are what games read most and never change: open and map them once, for the whole run, instead
       of opening them on demand like the save and SD card files */
    static const char* const shared_paths[] = {"romfs", "exefs", "romfs.bin"};
    unsigned shared = 0, failed = 0;
    pm_lock(&fsp.tree.lock);
    for (size_t i = 0; i < sizeof shared_paths / sizeof shared_paths[0]; i++) {
        FsNode* node;
        if (fs_tree_lookup(&fsp.tree, NULL, shared_paths[i], &node)) continue;
        r = fs_tree_share(&fsp.tree, node, &shared, &failed);
        if (r != FS_TREE_OK) fatal("Could not map %s%s (error %d)", g_datadir, shared_paths[i], r);
    }
    pm_unlock(&fsp.tree.lock);
    LOG("fs: %u RomFS/ExeFS files mapped for the whole run\n", shared);
    if (failed) WARN("fs: %u RomFS/ExeFS files could not be mapped, they are read on demand\n", failed);
}

/* Contents held in memory, read without the host: an injected file, or one mapped at startup */
static int node_in_memory(const FsNode* node) { return node->is_virtual || node->fd >= 0; }
static const u8* node_memory(const FsNode* node) { return node->is_virtual ? node->virtual_data : node->map; }

/* Callers hold tree.lock: content I/O is permitted only after resolving an indexed node. */
static FILE* node_open(FsNode* node, const char* mode) {
    char path[FS_TREE_PATH_MAX];
    if (node->is_dir || node->is_virtual || fs_tree_path(&fsp.tree, node, path, sizeof path)) return NULL;
    FILE* f = fopen(path, mode);
    if (f) setvbuf(f, NULL, _IONBF, 0); /* keep different handles coherent */
    return f;
}

FILE* fs_open_read(const char* path, u64* size) {
    FsNode* node;
    FILE* f = NULL;
    if (size) *size = 0;
    pm_lock(&fsp.tree.lock);
    if (!fs_tree_lookup(&fsp.tree, NULL, path, &node)) {
        f = node_open(node, "rb");
        if (f && size) *size = node->size;
    }
    pm_unlock(&fsp.tree.lock);
    return f;
}

int fs_map_file(const char* path, PFileMap* map) {
    FsNode* node;
    char host[FS_TREE_PATH_MAX];
    int r = -1;
    memset(map, 0, sizeof *map);
    pm_lock(&fsp.tree.lock);
    if (!fs_tree_lookup(&fsp.tree, NULL, path, &node) && !node->is_dir) {
        if (node->fd >= 0) { /* already mapped for the whole run: lend that view */
            map->data = node->map;
            map->size = node->size;
            map->borrowed = 1;
            r = 0;
        } else if (!node->is_virtual && !fs_tree_path(&fsp.tree, node, host, sizeof host))
            r = plat_file_map_sized(map, host, node->size);
    }
    pm_unlock(&fsp.tree.lock);
    return r;
}

u8* fs_read_file(const char* path, u64* size) {
    FsNode* node;
    u8* data = NULL;
    if (size) *size = 0;
    pm_lock(&fsp.tree.lock);
    if (!fs_tree_lookup(&fsp.tree, NULL, path, &node) && !node->is_dir && node->size <= SIZE_MAX) {
        data = malloc(node->size ? (size_t)node->size : 1);
        if (data) {
            int ok = 1;
            if (node_in_memory(node)) {
                if (node->size) memcpy(data, node_memory(node), (size_t)node->size);
            } else {
                FILE* f = node_open(node, "rb");
                ok = f && fread(data, 1, (size_t)node->size, f) == node->size;
                if (f) fclose(f);
            }
            if (!ok) {
                free(data);
                data = NULL;
            } else if (size)
                *size = node->size;
        }
    }
    pm_unlock(&fsp.tree.lock);
    return data;
}

int fs_add_virtual_file(const char* path, const void* data, u64 size) {
    pm_lock(&fsp.tree.lock);
    FsTreeResult r = fs_tree_add_virtual(&fsp.tree, path, data, size, NULL);
    pm_unlock(&fsp.tree.lock);
    return r ? -1 : 0;
}

static int add_dir(const char* name, int parent) {
    if (fsp.ndn == fsp.capdn) {
        if (fsp.capdn > INT32_MAX / 2) fatal("Too many RomFS directories");
        fsp.capdn = fsp.capdn ? fsp.capdn * 2 : 256;
        fsp.dn = realloc(fsp.dn, sizeof(DNode) * fsp.capdn);
        if (!fsp.dn) fatal("Could not allocate RomFS directories");
    }
    DNode* d = &fsp.dn[fsp.ndn];
    d->name = strdup(name);
    if (!d->name) fatal("Could not allocate RomFS directory name");
    d->parent = parent;
    d->first_child = -1;
    d->next_sib = -1;
    d->first_file = -1;
    return fsp.ndn++;
}
static int add_file(const char* name, int parent, FsNode* node) {
    if (fsp.nfn == fsp.capfn) {
        if (fsp.capfn > INT32_MAX / 2) fatal("Too many RomFS files");
        fsp.capfn = fsp.capfn ? fsp.capfn * 2 : 1024;
        fsp.fn = realloc(fsp.fn, sizeof(FNode) * fsp.capfn);
        if (!fsp.fn) fatal("Could not allocate RomFS files");
    }
    FNode* f = &fsp.fn[fsp.nfn];
    f->name = strdup(name);
    if (!f->name) fatal("Could not allocate RomFS file name");
    f->parent = parent;
    f->next_sib = -1;
    f->size = node->size;
    f->node = node;
    fs_tree_retain(node);
    return fsp.nfn++;
}
/* The startup index already has a stable, sorted directory order. */
static void scan(FsNode* dir, int di) {
    for (int pass = 0; pass < 2; pass++) {
        int last = -1;
        for (FsNode* node = dir->children; node; node = node->next) {
            if (node->is_dir != (pass == 0)) continue;
            if (node->is_dir) {
                int child = add_dir(node->name, di);
                if (last < 0)
                    fsp.dn[di].first_child = child;
                else
                    fsp.dn[last].next_sib = child;
                last = child;
                scan(node, child);
            } else {
                int file = add_file(node->name, di, node);
                if (last < 0)
                    fsp.dn[di].first_file = file;
                else
                    fsp.fn[last].next_sib = file;
                last = file;
            }
        }
    }
}
static u32 rhash(u32 parent, const char* s) {
    u32 h = parent ^ 123456789u;
    for (; *s; s++) {
        h = (h >> 5) | (h << 27);
        h ^= (u8)*s;
    }
    return h;
}
static u32 hcount(u32 n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    u32 c = n;
    while (!(c % 2) || !(c % 3) || !(c % 5) || !(c % 7) || !(c % 11) || !(c % 13) || !(c % 17)) c++;
    return c;
}

static void romfs_build(FsNode* dir) {
    add_dir("", 0);
    fsp.dn[0].parent = 0;
    scan(dir, 0);
    /* table offsets */
    u32 doff = 0;
    for (int i = 0; i < fsp.ndn; i++) {
        fsp.dn[i].off = doff;
        size_t entry = 0x18 + ((strlen(fsp.dn[i].name) + 3) & ~(size_t)3);
        if (entry > UINT32_MAX - doff) fatal("RomFS directory table is too large");
        doff += (u32)entry;
    }
    u32 foff = 0;
    for (int i = 0; i < fsp.nfn; i++) {
        fsp.fn[i].off = foff;
        size_t entry = 0x20 + ((strlen(fsp.fn[i].name) + 3) & ~(size_t)3);
        if (entry > UINT32_MAX - foff) fatal("RomFS file table is too large");
        foff += (u32)entry;
    }
    u32 dhc = hcount(fsp.ndn), fhc = hcount(fsp.nfn);
    u64 hdr = 0x50, dht = hdr, dmt = dht + dhc * 4ull, fht = dmt + doff, fmt = fht + fhc * 4ull, end = fmt + foff;
    fsp.rdata_off = (end + 0x1FF) & ~0x1FFull;
    fsp.rmeta_size = fsp.rdata_off;
    fsp.rmeta = calloc(1, fsp.rmeta_size);
    if (!fsp.rmeta) fatal("Could not allocate RomFS metadata");
    u64 H[10] = {0x50, dht, dhc * 4ull, dmt, doff, fht, fhc * 4ull, fmt, foff, fsp.rdata_off};
    memcpy(fsp.rmeta, H, 0x50);
    u32* dh = (u32*)(fsp.rmeta + dht);
    for (u32 i = 0; i < dhc; i++) dh[i] = 0xFFFFFFFFu;
    u32* fh = (u32*)(fsp.rmeta + fht);
    for (u32 i = 0; i < fhc; i++) fh[i] = 0xFFFFFFFFu;
    u64 data = 0;
    fsp.rfiles = calloc(fsp.nfn ? fsp.nfn : 1, sizeof(RFile));
    if (!fsp.rfiles) fatal("Could not allocate RomFS file index");
    for (int i = 0; i < fsp.nfn; i++) {
        if (data > UINT64_MAX - 0xF) fatal("RomFS data is too large");
        data = (data + 0xF) & ~0xFull;
        if (data > UINT64_MAX - fsp.rdata_off || fsp.fn[i].size > UINT64_MAX - fsp.rdata_off - data)
            fatal("RomFS data is too large");
        fsp.rfiles[fsp.nrfiles].node = fsp.fn[i].node;
        fsp.rfiles[fsp.nrfiles].off = fsp.rdata_off + data;
        fsp.rfiles[fsp.nrfiles].size = fsp.fn[i].size;
        fsp.nrfiles++;
        u8* e = fsp.rmeta + fmt + fsp.fn[i].off;
        u32 w[2] = {fsp.dn[fsp.fn[i].parent].off,
                    fsp.fn[i].next_sib >= 0 ? fsp.fn[fsp.fn[i].next_sib].off : 0xFFFFFFFFu};
        memcpy(e, w, 8);
        memcpy(e + 8, &data, 8);
        memcpy(e + 16, &fsp.fn[i].size, 8);
        u32 h = rhash(fsp.dn[fsp.fn[i].parent].off, fsp.fn[i].name) % fhc;
        u32 nl = (u32)strlen(fsp.fn[i].name);
        memcpy(e + 0x18, &fh[h], 4);
        fh[h] = fsp.fn[i].off;
        memcpy(e + 0x1C, &nl, 4);
        memcpy(e + 0x20, fsp.fn[i].name, nl);
        data += fsp.fn[i].size;
    }
    for (int i = 0; i < fsp.ndn; i++) {
        u8* e = fsp.rmeta + dmt + fsp.dn[i].off;
        u32 w[4] = {fsp.dn[fsp.dn[i].parent].off,
                    fsp.dn[i].next_sib >= 0 ? fsp.dn[fsp.dn[i].next_sib].off : 0xFFFFFFFFu,
                    fsp.dn[i].first_child >= 0 ? fsp.dn[fsp.dn[i].first_child].off : 0xFFFFFFFFu,
                    fsp.dn[i].first_file >= 0 ? fsp.fn[fsp.dn[i].first_file].off : 0xFFFFFFFFu};
        memcpy(e, w, 16);
        u32 h = rhash(i ? fsp.dn[fsp.dn[i].parent].off : 0, fsp.dn[i].name) % dhc;
        u32 nl = (u32)strlen(fsp.dn[i].name);
        memcpy(e + 16, &dh[h], 4);
        dh[h] = fsp.dn[i].off;
        memcpy(e + 20, &nl, 4);
        memcpy(e + 24, fsp.dn[i].name, nl);
    }
    fsp.rtotal = fsp.rdata_off + data;
    /* rfiles is already in offset order (offsets grow with i). No sort: one without a tie-break could put a zero
       size file after the file that starts at the same offset, and romfs_read would then find the empty one */
    LOG("RomFS: %d folders, %d files, %.1f MB\n", fsp.ndn, fsp.nfn, (double)fsp.rtotal / 1048576.0);
}

void romfs_init(void) {
    pm_lock(&fsp.tree.lock);
    FsNode* node;
    if (!fs_tree_lookup(&fsp.tree, NULL, "romfs.bin", &node) && !node->is_dir) {
        if (node->fd >= 0)
            fsp.rimage = node;
        else if (!(fsp.rraw = node_open(node, "rb")))
            fatal("Could not open indexed romfs.bin");
        fsp.rtotal = node->size;
        LOG("RomFS: image (%.1f MB)\n", (double)fsp.rtotal / 1048576.0);
    } else if (!fs_tree_lookup(&fsp.tree, NULL, "romfs:/", &node) && node->is_dir) {
        romfs_build(node);
    } else {
        LOG("RomFS: none in %s\n", g_datadir);
        if (fs_tree_lookup(&fsp.tree, NULL, "exefs/main.nro", &node))
            WARN("Extract the game's RomFS into %sromfs/ before running it\n", g_datadir);
    }
    pm_unlock(&fsp.tree.lock);
}

static FILE* rf_open(FsNode* node) { /* tree.lock held */
    if (!node->attached) return NULL;
    int lru = 0;
    for (int i = 0; i < NOPEN; i++) {
        if (fsp.openf[i].node == node) {
            fsp.openf[i].last = ++fsp.openclk;
            return fsp.openf[i].f;
        }
        if (fsp.openf[i].last < fsp.openf[lru].last) lru = i;
    }
    if (fsp.openf[lru].f) fclose(fsp.openf[lru].f);
    fsp.openf[lru].f = node_open(node, "rb");
    fsp.openf[lru].node = fsp.openf[lru].f ? node : NULL;
    fsp.openf[lru].last = fsp.openf[lru].f ? ++fsp.openclk : 0;
    return fsp.openf[lru].f;
}
/* -1 when a host file couldn't be read (that part is zeros) */
static int romfs_read(u64 off, void* dst, u64 size) {
    u8* d = dst;
    if (fsp.rimage) { /* romstorage keeps off and size within rtotal, the mapping's length */
        memcpy(d, fsp.rimage->map + off, (size_t)size);
        return 0;
    }
    if (fsp.rraw) {
        pm_lock(&fsp.tree.lock);
        i64 r = plat_pread(fsp.rraw, d, size, off);
        pm_unlock(&fsp.tree.lock);
        return r < 0 || (u64)r < size ? -1 : 0;
    }
    if (off < fsp.rmeta_size) {
        u64 n = fsp.rmeta_size - off;
        if (n > size) n = size;
        memcpy(d, fsp.rmeta + off, n);
        d += n;
        off += n;
        size -= n;
    }
    /* rfiles doesn't change after romfs_build, and its nodes are readonly: only a file that isn't mapped needs the lock */
    int rc = 0;
    while (size) {
        int lo = 0, hi = fsp.nrfiles - 1, k = -1;
        while (lo <= hi) {
            int m = (lo + hi) / 2;
            if (fsp.rfiles[m].off <= off) {
                k = m;
                lo = m + 1;
            } else
                hi = m - 1;
        }
        if (k < 0) {
            memset(d, 0, size);
            break;
        }
        RFile* f = &fsp.rfiles[k];
        if (off >= f->off + f->size) { /* inter-file padding */
            u64 nx = k + 1 < fsp.nrfiles ? fsp.rfiles[k + 1].off : fsp.rtotal;
            u64 n = nx > off ? nx - off : size;
            if (n > size) n = size;
            if (!n) n = size;
            memset(d, 0, n);
            d += n;
            off += n;
            size -= n;
            continue;
        }
        u64 n = f->off + f->size - off;
        if (n > size) n = size;
        int unlogged = 0;
        if (__atomic_compare_exchange_n(&f->logged, &unlogged, 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            LOG("romfs: %s (%.1f KB)\n", f->node->name, (double)f->size / 1024.0);
        i64 got;
        if (f->node->fd >= 0) {
            memcpy(d, f->node->map + (off - f->off), (size_t)n);
            got = (i64)n;
        } else {
            pm_lock(&fsp.tree.lock);
            if (f->node->is_virtual && f->node->attached) {
                memcpy(d, f->node->virtual_data + off - f->off, n);
                got = (i64)n;
            } else {
                FILE* fp = rf_open(f->node);
                got = fp ? plat_pread(fp, d, n, off - f->off) : -1;
            }
            pm_unlock(&fsp.tree.lock);
        }
        if (got < 0 || (u64)got < n) {
            if (__atomic_load_n(&f->logged, __ATOMIC_RELAXED) < 2) {
                __atomic_store_n(&f->logged, 2, __ATOMIC_RELAXED);
                WARN("romfs: could not read %s (at %llu, %llu bytes), the game gets zeros\n", f->node->name,
                     (unsigned long long)(off - f->off), (unsigned long long)n);
            }
            memset(d, 0, n);
            rc = -1;
        }
        d += n;
        off += n;
        size -= n;
    }
    return rc;
}
/* QueryRange (OperateRange): an all-zero QueryRangeInfo */
static int query_range(IpcCtx* c) {
    u8 z[0x40];
    memset(z, 0, sizeof z);
    outbytes(c, z, 0x40);
    return 0;
}

/* ReadOnlyStorageAdapter still receives the StorageInterfaceAdapter's argument checks.
   Flush succeeds for these immutable images; their read-only backend rejects Write and SetSize. */
static int readonly_storage_command(IpcCtx* c) {
    if (c->cmd == 1) {
        u64 off = in64(c, 0), sz = in64(c, 8), a = 0, bs = 0;
        if ((i64)off < 0) return FS_INVALID_OFFSET;
        if ((i64)sz < 0) return FS_INVALID_SIZE;
        buf_in(c, 0, &a, &bs);
        if ((i64)bs < 0 || sz > bs) return FS_INVALID_SIZE;
    } else if (c->cmd == 2)
        return 0;
    else if (c->cmd == 3 && (i64)in64(c, 0) < 0)
        return FS_INVALID_SIZE;
    return RC(2, 6300);
}

/* ============================================================ IStorage (RomFS) */
static int romstorage(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: {
            u64 off = in64(c, 0), sz = in64(c, 8), a = 0, bs = 0;
            /* Atmosphere StorageInterfaceAdapter validates the signed arguments before the backend. */
            if ((i64)off < 0) return FS_INVALID_OFFSET;
            if ((i64)sz < 0) return FS_INVALID_SIZE;
            buf_out(c, 0, &a, &bs);
            if ((i64)bs < 0 || sz > bs) return FS_INVALID_SIZE;
            if (!sz) return 0;
            if (sz > (u64)INT64_MAX - off || off > fsp.rtotal || sz > fsp.rtotal - off) return FS_OUT_OF_RANGE;
            if (a > AS_SIZE || sz > AS_SIZE - a) return FS_OUT_OF_RANGE;
            return romfs_read(off, GP(a), sz) ? FS_OUT_OF_RANGE : 0;
        }
        case 4: out64(c, fsp.rtotal); return 0;
        case 1:
        case 2:
        case 3: return readonly_storage_command(c);
        case 5: return query_range(c);
    }
    return stub_ok(o, c);
}

/* ============================================================ Indexed IFileSystem */
typedef struct {
    FsNode* root;
} FsData;
typedef struct {
    FILE* f;
    FsNode* node;
    u32 mode; /* OpenMode: 1 read, 2 write, 4 allow append */
} FileData;
typedef struct {
    FsNode* node;
    u32 filter;
    char last[FS_TREE_PATH_MAX]; /* names survive deletion/rename of the previous entry */
    int done;
} DirData;

static int buffer_valid(u64 addr, u64 size) { return addr <= AS_SIZE && size <= AS_SIZE - addr; }

static u32 read_path(IpcCtx* c, int idx, char out[0x301]) {
    u64 a = 0, s = 0;
    if (!buf_xs(c, idx, &a, &s) || !a) buf_in(c, idx, &a, &s);
    if (!a || !s || !buffer_valid(a, s)) return FS_DIR_UNOBTAINABLE;
    size_t limit = s < 0x301 ? (size_t)s : 0x301;
    const char* start = (const char*)GP(a);
    const char* end = memchr(start, 0, limit);
    if (!end) return FS_PATH_TOO_LONG;
    memcpy(out, start, (size_t)(end - start) + 1);
    return 0;
}

/* 64-bit file offsets: long is 32 bits on Windows. */
static int file_seek(FILE* f, u64 off) {
#if defined(_WIN32)
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}
static int file_truncate(FILE* f, u64 n, u64* actual) {
    if (fflush(f)) return -1;
#if defined(_WIN32)
    int rc = _chsize_s(_fileno(f), (long long)n) ? -1 : 0;
    if (rc) {
        /* A failed extension can still change the file. Inspect this open handle, never rediscover its name. */
        i64 size = _filelengthi64(_fileno(f));
        if (size >= 0) *actual = (u64)size;
    }
#else
    int rc = ftruncate(fileno(f), (off_t)n);
    if (rc) {
        struct stat st;
        if (!fstat(fileno(f), &st) && st.st_size >= 0) *actual = (u64)st.st_size;
    }
#endif
    if (!rc) *actual = n;
    return rc;
}

/* All service operations share tree.lock, including file size updates across different handles. */
static int file_dispatch_locked(SvcObj* o, IpcCtx* c) {
    FileData* f = o->data;
    switch (c->cmd) {
        case 0: {
            u64 off = in64(c, 8), sz = in64(c, 16), a = 0, bs = 0;
            /* Atmosphere FileInterfaceAdapter -> IFile::Read -> DryRead: order is observable on errors. */
            if ((i64)off < 0) return FS_INVALID_OFFSET;
            if ((i64)sz < 0) return FS_INVALID_SIZE;
            buf_out(c, 0, &a, &bs);
            if ((i64)bs < 0 || sz > bs) return FS_INVALID_SIZE;
            if (!sz) {
                out64(c, 0);
                return 0;
            }
            if (sz > (u64)INT64_MAX - off) return FS_OUT_OF_RANGE;
            if (!(f->mode & 1)) return FS_READ_NOT_PERMITTED;
            if (off > f->node->size) return FS_OUT_OF_RANGE;
            if (sz > f->node->size - off) sz = f->node->size - off;
            if (!buffer_valid(a, sz)) return FS_OUT_OF_RANGE;
            u64 n = sz;
            if (node_in_memory(f->node)) {
                if (sz) memcpy(GP(a), node_memory(f->node) + off, (size_t)sz);
            } else {
                if (file_seek(f->f, off)) return FS_OUT_OF_RANGE;
                clearerr(f->f);
                n = fread(GP(a), 1, (size_t)sz, f->f);
                if (ferror(f->f)) return FS_OUT_OF_RANGE;
            }
            out64(c, n);
            return 0;
        }
        case 1: {
            u64 off = in64(c, 8), sz = in64(c, 16), a = 0, bs = 0;
            if ((i64)off < 0) return FS_INVALID_OFFSET;
            if ((i64)sz < 0) return FS_INVALID_SIZE;
            buf_in(c, 0, &a, &bs);
            if ((i64)bs < 0 || sz > bs) return FS_INVALID_SIZE;
            /* IFile::Write succeeds without checking mode/EOF for an empty write, but honors Flush. */
            if (!sz) return (in32(c, 0) & 1) && f->f && fflush(f->f) ? FS_NO_SPACE : 0;
            if (sz > (u64)INT64_MAX - off) return FS_OUT_OF_RANGE;
            if (!(f->mode & 2) || f->node->readonly) return FS_WRITE_NOT_PERMITTED;
            if (!buffer_valid(a, sz)) return FS_OUT_OF_RANGE;
            if (off + sz > f->node->size && !(f->mode & 4)) return FS_NO_APPEND;
            if (file_seek(f->f, off)) return FS_OUT_OF_RANGE;
            size_t n = fwrite(GP(a), 1, (size_t)sz, f->f);
            if (n && off + n > f->node->size) f->node->size = off + n;
            if (n != sz || fflush(f->f)) return FS_NO_SPACE;
            return 0;
        }
        case 2: return f->f && fflush(f->f) ? FS_NO_SPACE : 0;
        case 3: {
            u64 n = in64(c, 0);
            if ((i64)n < 0) return FS_INVALID_SIZE;
            if (!(f->mode & 2) || f->node->readonly) return FS_WRITE_NOT_PERMITTED;
            if (file_truncate(f->f, n, &f->node->size)) return FS_NO_SPACE;
            return 0;
        }
        case 4: out64(c, f->node->size); return 0;
        case 5: return query_range(c);
    }
    return stub_ok(o, c);
}
static int file_disp(SvcObj* o, IpcCtx* c) {
    pm_lock(&fsp.tree.lock);
    int r = file_dispatch_locked(o, c);
    pm_unlock(&fsp.tree.lock);
    return r;
}
static void file_destroy(SvcObj* o) {
    FileData* f = o->data;
    pm_lock(&fsp.tree.lock);
    if (f->f) fclose(f->f);
    fs_tree_release(f->node);
    pm_unlock(&fsp.tree.lock);
}
static SvcObj* file_new(FsNode* node, u32 mode, u32* rc) {
    if (node->is_dir) {
        *rc = FS_NOTFOUND;
        return NULL;
    }
    if (!(mode & 3) || (mode & ~7u)) {
        *rc = FS_INVALID_OPEN_MODE;
        return NULL;
    }
    if ((mode & 2) && node->readonly) {
        *rc = FS_WRITE_NOT_PERMITTED;
        return NULL;
    }
    /* a mapped node is readonly, so this handle only reads, from the mapping */
    FILE* host = node_in_memory(node) ? NULL : node_open(node, (mode & 2) ? "rb+" : "rb");
    if (!node_in_memory(node) && !host) {
        *rc = FS_NOTFOUND;
        return NULL;
    }
    SvcObj* o = svcobj_new("IFile", file_disp, sizeof(FileData));
    FileData* data = o->data;
    data->f = host;
    data->node = node;
    data->mode = mode;
    fs_tree_retain(node);
    o->destroy = file_destroy;
    *rc = 0;
    return o;
}

static int dir_dispatch_locked(SvcObj* o, IpcCtx* c) {
    DirData* d = o->data;
    if (!d->node->attached) return FS_NOTFOUND;
    switch (c->cmd) {
        case 0: {
            u64 a = 0, s = 0, n = 0;
            buf_out(c, 0, &a, &s);
            if (!buffer_valid(a, s)) return FS_OUT_OF_RANGE;
            u64 max = s / 0x310;
            FsNode* node = d->node->children;
            while (node && strcmp(node->name, d->last) <= 0) node = node->next;
            while (node && n < max && !d->done) {
                strcpy(d->last, node->name);
                if (node->is_dir ? (d->filter & 1) : (d->filter & 2)) {
                    u8* e = GP(a + n * 0x310);
                    memset(e, 0, 0x310);
                    strncpy((char*)e, node->name, 0x300);
                    e[0x304] = node->is_dir ? 0 : 1;
                    u64 size = node->is_dir ? 0 : node->size;
                    memcpy(e + 0x308, &size, 8);
                    n++;
                }
                node = node->next;
            }
            if (!node) d->done = 1;
            out64(c, n);
            return 0;
        }
        case 1: {
            u64 n = 0;
            for (FsNode* node = d->node->children; node; node = node->next)
                if (node->is_dir ? (d->filter & 1) : (d->filter & 2)) n++;
            out64(c, n);
            return 0;
        }
    }
    return stub_ok(o, c);
}
static int dir_disp(SvcObj* o, IpcCtx* c) {
    pm_lock(&fsp.tree.lock);
    int r = dir_dispatch_locked(o, c);
    pm_unlock(&fsp.tree.lock);
    return r;
}
static void dir_destroy(SvcObj* o) {
    DirData* d = o->data;
    pm_lock(&fsp.tree.lock);
    fs_tree_release(d->node);
    pm_unlock(&fsp.tree.lock);
}
static int fs_dispatch_locked(SvcObj* o, IpcCtx* c) {
    FsData* fs = o->data;
    char p[0x301], p2[0x301];
    u32 rc;
    FsNode* node;
    if (!fs->root->attached) return FS_NOTFOUND;
    switch (c->cmd) {
        case 0: {
            u64 size = in64(c, 8);
            if ((i64)size < 0) return FS_INVALID_SIZE;
            if ((rc = read_path(c, 0, p))) return (int)rc;
            return tree_result(fs_tree_create(&fsp.tree, fs->root, p, 0, size, NULL));
        }
        case 1:
            if ((rc = read_path(c, 0, p))) return (int)rc;
            return tree_result(fs_tree_remove(&fsp.tree, fs->root, p, 0, 0, 0));
        case 2:
            if ((rc = read_path(c, 0, p))) return (int)rc;
            return tree_result(fs_tree_create(&fsp.tree, fs->root, p, 1, 0, NULL));
        case 3:
        case 4:
        case 13:
            if ((rc = read_path(c, 0, p))) return (int)rc;
            return tree_result(fs_tree_remove(&fsp.tree, fs->root, p, 1, c->cmd != 3, c->cmd == 13));
        case 5:
        case 6:
            if ((rc = read_path(c, 0, p))) return (int)rc;
            if ((rc = read_path(c, 1, p2))) return (int)rc;
            return tree_result(fs_tree_rename(&fsp.tree, fs->root, p, p2, c->cmd == 6));
        case 7:
            if ((rc = read_path(c, 0, p))) return (int)rc;
            if ((rc = tree_result(fs_tree_lookup(&fsp.tree, fs->root, p, &node)))) return (int)rc;
            out32(c, node->is_dir ? 0 : 1);
            return 0;
        case 8: {
            if ((rc = read_path(c, 0, p))) return (int)rc;
            rc = tree_result(fs_tree_lookup(&fsp.tree, fs->root, p, &node));
            /* Lookup also normalizes the path. Normalization errors precede mode validation,
               but IFileSystem checks mode before its backend reports a missing file. */
            if (rc && rc != FS_NOTFOUND) return (int)rc;
            u32 mode = in32(c, 0);
            if (!(mode & 3) || (mode & ~7u)) return FS_INVALID_OPEN_MODE;
            if (rc) return (int)rc;
            SvcObj* file = file_new(node, mode, &rc);
            if (file) out_obj(c, file);
            return (int)rc;
        }
        case 9: {
            if ((rc = read_path(c, 0, p))) return (int)rc;
            rc = tree_result(fs_tree_lookup(&fsp.tree, fs->root, p, &node));
            if (rc && rc != FS_NOTFOUND) return (int)rc;
            u32 mode = in32(c, 0);
            /* OpenDirectoryMode_NotRequireFileSize is bit 31, not another entry-type filter. */
            if (!(mode & 3) || (mode & ~0x80000003u)) return FS_INVALID_OPEN_MODE;
            if (rc) return (int)rc;
            if (!node->is_dir) return FS_NOTFOUND;
            SvcObj* dir = svcobj_new("IDirectory", dir_disp, sizeof(DirData));
            DirData* data = dir->data;
            data->node = node;
            data->filter = mode;
            fs_tree_retain(node);
            dir->destroy = dir_destroy;
            out_obj(c, dir);
            return 0;
        }
        case 10: return 0;
        case 11: out64(c, 4ull << 30); return 0;
        case 12: out64(c, 8ull << 30); return 0;
        case 14: {
            u8 z[0x20] = {0};
            outbytes(c, z, sizeof z);
            return 0;
        }
        case 15: return 0;
        case 16: out64(c, 0); return 0;
    }
    return stub_ok(o, c);
}
static int fs_disp(SvcObj* o, IpcCtx* c) {
    pm_lock(&fsp.tree.lock);
    int r = fs_dispatch_locked(o, c);
    pm_unlock(&fsp.tree.lock);
    return r;
}
static void fs_destroy(SvcObj* o) {
    FsData* data = o->data;
    pm_lock(&fsp.tree.lock);
    fs_tree_release(data->root);
    pm_unlock(&fsp.tree.lock);
}
static SvcObj* hostfs_new(const char* root, u32* rc) { /* tree.lock held */
    FsNode* node;
    *rc = tree_result(fs_tree_mkdirs(&fsp.tree, root, &node));
    if (*rc) return NULL;
    SvcObj* o = svcobj_new("IFileSystem", fs_disp, sizeof(FsData));
    ((FsData*)o->data)->root = node;
    fs_tree_retain(node);
    o->destroy = fs_destroy;
    return o;
}

static int savereader(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out64(c, 0);
        return 0;
    }
    return stub_ok(o, c);
}
/* system archives (NgWord, fonts...) aren't available, hand out an empty but valid RomFS */
static void emptyrom_init(void) {
    u8* b = fsp.emptyrom;
    u64 H[10] = {0x50, 0x50, 4, 0x54, 0x18, 0x6C, 4, 0x70, 0, 0x200};
    memcpy(b, H, 0x50);
    u32 dh = 0;
    memcpy(b + 0x50, &dh, 4);
    u32 root[6] = {0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0};
    memcpy(b + 0x54, root, 0x18);
    u32 fh = 0xFFFFFFFFu;
    memcpy(b + 0x6C, &fh, 4);
}
static int systemstorage(SvcObj* o, IpcCtx* c) {
    FileData* data = o->data;
    if (c->cmd == 4) return file_disp(o, c);
    if (c->cmd == 5) return query_range(c);
    if (c->cmd != 0) return readonly_storage_command(c);
    u64 off = in64(c, 0), sz = in64(c, 8), a = 0, bs = 0;
    if ((i64)off < 0) return FS_INVALID_OFFSET;
    if ((i64)sz < 0) return FS_INVALID_SIZE;
    buf_out(c, 0, &a, &bs);
    if ((i64)bs < 0 || sz > bs) return FS_INVALID_SIZE;
    if (!sz) return 0;
    if (sz > (u64)INT64_MAX - off || !buffer_valid(a, sz)) return FS_OUT_OF_RANGE;
    pm_lock(&fsp.tree.lock);
    int rc = 0;
    if (off > data->node->size || sz > data->node->size - off)
        rc = FS_OUT_OF_RANGE;
    else if (node_in_memory(data->node)) {
        if (sz) memcpy(GP(a), node_memory(data->node) + off, (size_t)sz);
    } else if (file_seek(data->f, off) || fread(GP(a), 1, (size_t)sz, data->f) != sz) {
        rc = FS_OUT_OF_RANGE;
    }
    pm_unlock(&fsp.tree.lock);
    return rc;
}
/* save data folders, by SaveDataType */
static const char* const g_save_kinds[] = {"system", "user", "bcat", "device", "temp", "cache", "sysbcat"};
/* Earlier versions read the save type from the wrong byte, which is always 0, so every save went to save/system.
   An Account save (save/user) that doesn't exist yet takes over that folder, once */
static u32 save_migrate(void) { /* tree.lock held */
    FsNode* src;
    FsNode* dst;
    if (!fs_tree_lookup(&fsp.tree, NULL, "save/user", &dst)) return 0;
    if (fs_tree_lookup(&fsp.tree, NULL, "save/system", &src) || !src->is_dir || !src->children) return 0;
    FsTreeResult r = fs_tree_rename(&fsp.tree, NULL, "save/system", "save/user", 1);
    if (r)
        WARN("fs: could not move save/system to save/user\n");
    else
        LOG("fs: moved save/system to save/user\n");
    return tree_result(r);
}
static int fsp_disp(SvcObj* o, IpcCtx* c) {
    char p[1200];
    switch (c->cmd) {
        case 1: return 0;
        case 18: {
            u32 rc;
            pm_lock(&fsp.tree.lock);
            SvcObj* fs = hostfs_new("sdmc", &rc);
            pm_unlock(&fsp.tree.lock);
            if (fs) out_obj(c, fs);
            return (int)rc;
        }
        case 51:
        case 52:
        case 53: {
            /* in: u8 space @0, then the attribute @8 = { u64 program_id, uid[16], u64 system_save_id, u8 type, u8 rank,
               u16 index }: the type is at 8 + 0x20 */
            u8 type = in8(c, 8 + 0x20);
            const char* kind = type < sizeof g_save_kinds / sizeof g_save_kinds[0] ? g_save_kinds[type] : "system";
            snprintf(p, sizeof p, "save/%s", kind);
            pm_lock(&fsp.tree.lock);
            u32 rc = type == 1 ? save_migrate() : 0;
            SvcObj* fs = rc ? NULL : hostfs_new(p, &rc);
            pm_unlock(&fsp.tree.lock);
            if (fs) out_obj(c, fs);
            return (int)rc;
        }
        case 22:
        case 23:
        case 25:
        case 26:
        case 27:
        case 28:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37: return 0;
        case 60:
        case 61:
        case 62: out_obj(c, svcobj_new("ISaveDataInfoReader", savereader, 0)); return 0;
        case 200: {
            if (!fsp.rtotal) {
                WARN("fs: the game requests the RomFS but there is none (put the romfs folder in %s)\n", g_datadir);
            }
            out_obj(c, svcobj_new("IStorage(romfs)", romstorage, 0));
            return 0;
        }
        case 202: {
            u64 id = in64(c, 8);
            snprintf(p, sizeof p, "system/archives/%016llx.romfs", (unsigned long long)id);
            pm_lock(&fsp.tree.lock);
            FsNode* node;
            FsTreeResult r = fs_tree_lookup(&fsp.tree, NULL, p, &node);
            if (r == FS_TREE_NOTFOUND) {
                r = fs_tree_add_virtual(&fsp.tree, p, fsp.emptyrom, sizeof fsp.emptyrom, &node);
                if (!r) LOG("fs: provided empty system archive %016llx\n", (unsigned long long)id);
            }
            u32 rc = tree_result(r);
            SvcObj* storage = rc ? NULL : file_new(node, 1, &rc);
            if (storage) {
                storage->name = "IStorage(system)";
                storage->dispatch = systemstorage;
            }
            pm_unlock(&fsp.tree.lock);
            if (storage) out_obj(c, storage);
            return (int)rc;
        }
        case 203: return RC(2, 1001);
        case 400: out_obj(c, svc_generic("IDeviceOperator")); return 0;
        case 500: out_obj(c, svc_generic("IEventNotifier")); return 0;
        case 1003:
        case 1004:
        case 1006:
        case 1007:
        case 1011:
        case 1012:
        case 1013: return 0;
        case 1005: out32(c, 0); return 0;
        case 1008: out_obj(c, svc_generic("IMultiCommitManager")); return 0; /* OpenMultiCommitManager */
        case 1009: {                                                         /* GetAndClearMemoryReportInfo */
            u8 z[0x80];
            memset(z, 0, sizeof z);
            outbytes(c, z, sizeof z);
            return 0;
        }
        case 620:
        case 630:
        case 631:
        case 632:
        case 800: return 0;
        case 8:
        case 11:
        case 17:
        case 19:
        case 31: return RC(2, 1001);
        case 2:
        case 7: return RC(2, 1001);
    }
    return stub_ok(o, c);
}
static SvcObj* new_fsp(void) { return svcobj_new("fsp-srv", fsp_disp, 0); }
static int fsppr_disp(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
static SvcObj* new_fsppr(void) { return svcobj_new("fsp-pr", fsppr_disp, 0); }

void svc_fs_init(void) {
    emptyrom_init();
    svc_register("fsp-srv", new_fsp);
    svc_register("fsp-pr", new_fsppr);
    svc_register("fsp-ldr", new_fsppr);
}
