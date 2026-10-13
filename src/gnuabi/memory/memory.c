/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "memory.h"
#include "../abi/errno/errno.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

static uint64_t up(uint64_t value) { return (value + GNU_PAGE_SIZE - 1) & ~(uint64_t)(GNU_PAGE_SIZE - 1); }
static int range(GnuMemory* m, uint64_t address, uint64_t length) {
    return m->base && address >= GNU_PAGE_SIZE && !(address & (GNU_PAGE_SIZE - 1)) && length && length <= m->size &&
           address <= m->size - length && up(length) <= m->size - address;
}
static int host_protect(GnuMemory* m, uint64_t address, uint64_t length, unsigned permissions) {
#ifdef _WIN32
    DWORD previous;
    DWORD p = (permissions & GNU_PROT_WRITE)                    ? PAGE_READWRITE
              : (permissions & (GNU_PROT_READ | GNU_PROT_EXEC)) ? PAGE_READONLY
                                                                : PAGE_NOACCESS;
    return VirtualProtect(m->base + address, (SIZE_T)length, p, &previous) ? 0 : -GNU_ENOMEM;
#else
    /* Guest instructions are data: host execution permission is never granted. */
    int p = permissions & GNU_PROT_WRITE                    ? PROT_READ | PROT_WRITE
            : permissions & (GNU_PROT_READ | GNU_PROT_EXEC) ? PROT_READ
                                                            : PROT_NONE;
    return mprotect(m->base + address, (size_t)length, p) ? -GNU_ENOMEM : 0;
#endif
}
int gnu_memory_init(GnuMemory* m) {
    memset(m, 0, sizeof *m);
    m->size = GNU_ADDRESS_LIMIT;
#ifdef _WIN32
    m->base = VirtualAlloc(NULL, (SIZE_T)m->size, MEM_RESERVE, PAGE_NOACCESS);
#else
    m->base = mmap(NULL, (size_t)m->size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m->base == MAP_FAILED) m->base = NULL;
#endif
    m->pages = calloc((size_t)(m->size / GNU_PAGE_SIZE), 1);
    m->mmap_next = UINT64_C(0x40000000);
    if (!m->base || !m->pages) {
        gnu_memory_destroy(m);
        return -GNU_ENOMEM;
    }
    return 0;
}
void gnu_memory_destroy(GnuMemory* m) {
    if (m->base) {
#ifdef _WIN32
        VirtualFree(m->base, 0, MEM_RELEASE);
#else
        munmap(m->base, (size_t)m->size);
#endif
    }
    free(m->pages);
    memset(m, 0, sizeof *m);
}
void* gnu_memory_access(GnuMemory* m, uint64_t a, size_t n, unsigned p) {
    if (!m->base || a > m->size || n > m->size - a) return NULL;
    if (!n) return m->base + a;
    for (uint64_t i = a / GNU_PAGE_SIZE; i <= (a + n - 1) / GNU_PAGE_SIZE; i++)
        if ((m->pages[i] & (GNU_PAGE_MAPPED | p)) != (GNU_PAGE_MAPPED | p)) return NULL;
    return m->base + a;
}
int gnu_memory_read(GnuMemory* m, uint64_t a, void* d, size_t n) {
    void* source = gnu_memory_access(m, a, n, GNU_PROT_READ);
    if (!source) return -GNU_EFAULT;
    if (n) memcpy(d, source, n);
    return 0;
}
int gnu_memory_write(GnuMemory* m, uint64_t a, const void* s, size_t n) {
    void* destination = gnu_memory_access(m, a, n, GNU_PROT_WRITE);
    if (!destination) return -GNU_EFAULT;
    if (n) memcpy(destination, s, n);
    return 0;
}
int gnu_memory_read_string(GnuMemory* m, uint64_t a, char* d, size_t capacity) {
    for (size_t i = 0; i < capacity; i++) {
        if (gnu_memory_read(m, a + i, d + i, 1)) return -GNU_EFAULT;
        if (!d[i]) return 0;
    }
    return -GNU_ENAMETOOLONG;
}
int gnu_memory_map(GnuMemory* m, uint64_t a, uint64_t n, unsigned p, int replace) {
    if (!range(m, a, n) || (p & ~7u)) return -GNU_EINVAL;
    n = up(n);
    if (!replace)
        for (uint64_t i = a / GNU_PAGE_SIZE; i < (a + n) / GNU_PAGE_SIZE; i++)
            if (m->pages[i] & GNU_PAGE_MAPPED) return -GNU_EEXIST;
#ifdef _WIN32
    if (!VirtualAlloc(m->base + a, (SIZE_T)n, MEM_COMMIT, PAGE_READWRITE)) return -GNU_ENOMEM;
#endif
    if (host_protect(m, a, n, GNU_PROT_READ | GNU_PROT_WRITE)) return -GNU_ENOMEM;
    memset(m->base + a, 0, (size_t)n);
    if (host_protect(m, a, n, p)) return -GNU_ENOMEM;
    memset(m->pages + a / GNU_PAGE_SIZE, (int)(GNU_PAGE_MAPPED | p), (size_t)(n / GNU_PAGE_SIZE));
    return 0;
}
int gnu_memory_unmap(GnuMemory* m, uint64_t a, uint64_t n) {
    if (!range(m, a, n)) return -GNU_EINVAL;
    n = up(n);
#ifdef _WIN32
    if (!VirtualFree(m->base + a, (SIZE_T)n, MEM_DECOMMIT)) return -GNU_ENOMEM;
#else
    if (host_protect(m, a, n, 0)) return -GNU_ENOMEM;
    (void)madvise(m->base + a, (size_t)n, MADV_DONTNEED);
#endif
    memset(m->pages + a / GNU_PAGE_SIZE, 0, (size_t)(n / GNU_PAGE_SIZE));
    return 0;
}
int gnu_memory_protect(GnuMemory* m, uint64_t a, uint64_t n, unsigned p) {
    if (!n) return (a & (GNU_PAGE_SIZE - 1)) || (p & ~7u) ? -GNU_EINVAL : 0;
    if (!range(m, a, n) || (p & ~7u)) return -GNU_EINVAL;
    n = up(n);
    for (uint64_t i = a / GNU_PAGE_SIZE; i < (a + n) / GNU_PAGE_SIZE; i++)
        if (!(m->pages[i] & GNU_PAGE_MAPPED)) return -GNU_ENOMEM;
    if (host_protect(m, a, n, p)) return -GNU_ENOMEM;
    memset(m->pages + a / GNU_PAGE_SIZE, (int)(GNU_PAGE_MAPPED | p), (size_t)(n / GNU_PAGE_SIZE));
    return 0;
}
int64_t gnu_memory_mmap(GnuMemory* m, uint64_t a, uint64_t n, unsigned p, uint64_t flags) {
    const uint64_t fixed = 0x10, noreplace = 0x100000;
    if (!n || n > m->size - GNU_PAGE_SIZE || p & ~7u || (flags & 3) == 0 || (flags & 3) == 3) return -GNU_EINVAL;
    if (!(flags & 0x20)) return -GNU_ENODEV; /* file mappings are handled by the VFS caller */
    if (flags & ~(UINT64_C(3) | 0x10 | 0x20 | 0x100 | 0x800 | 0x1000 | 0x2000 | 0x8000 | 0x10000 | 0x20000 | 0x100000))
        return -GNU_EINVAL;
    n = up(n);
    if (flags & (fixed | noreplace)) {
        int r = gnu_memory_map(m, a, n, p, (flags & fixed) && !(flags & noreplace));
        return r ? r : (int64_t)a;
    }
    a &= ~(uint64_t)(GNU_PAGE_SIZE - 1);
    if (a >= GNU_PAGE_SIZE && a <= m->size - n) {
        int r = gnu_memory_map(m, a, n, p, 0);
        if (!r) return (int64_t)a;
    }
    /* Keep mappings below the fixed stack region; search wraps for freed holes. */
    const uint64_t low = UINT64_C(0x40000000), high = UINT64_C(0xe0000000);
    if (n > high - low) return -GNU_ENOMEM;
    for (unsigned pass = 0; pass < 2; pass++) {
        uint64_t start = pass ? low : m->mmap_next;
        uint64_t end = pass ? m->mmap_next : high;
        if (start > end || n > end - start) continue;
        for (uint64_t candidate = start; candidate <= end - n;) {
            uint64_t i = candidate / GNU_PAGE_SIZE, last = (candidate + n) / GNU_PAGE_SIZE;
            while (i < last && !(m->pages[i] & GNU_PAGE_MAPPED)) i++;
            if (i == last) {
                int r = gnu_memory_map(m, candidate, n, p, 0);
                if (r) return r;
                m->mmap_next = candidate + n;
                return (int64_t)candidate;
            }
            candidate = (i + 1) * GNU_PAGE_SIZE;
        }
    }
    return -GNU_ENOMEM;
}
uint64_t gnu_memory_brk(GnuMemory* m, uint64_t requested) {
    if (!requested || requested < m->brk_base || requested > m->brk_limit) return m->brk_current;
    uint64_t old_page = up(m->brk_current), new_page = up(requested);
    if (new_page > old_page && gnu_memory_map(m, old_page, new_page - old_page, 3, 0)) return m->brk_current;
    if (new_page < old_page && gnu_memory_unmap(m, new_page, old_page - new_page)) return m->brk_current;
    m->brk_current = requested;
    return requested;
}
size_t gnu_memory_maps(GnuMemory* m, char* destination, size_t capacity) {
    size_t used = 0;
    for (uint64_t i = 0; i < m->size / GNU_PAGE_SIZE;) {
        if (!(m->pages[i] & GNU_PAGE_MAPPED)) {
            i++;
            continue;
        }
        uint64_t begin = i;
        unsigned p = m->pages[i];
        do { i++; } while (i < m->size / GNU_PAGE_SIZE && m->pages[i] == p);
        char line[128];
        int n = snprintf(line, sizeof line, "%016llx-%016llx %c%c%cp 00000000 00:00 0\n",
                         (unsigned long long)(begin * GNU_PAGE_SIZE), (unsigned long long)(i * GNU_PAGE_SIZE),
                         p & 1 ? 'r' : '-', p & 2 ? 'w' : '-', p & 4 ? 'x' : '-');
        if (n > 0) {
            size_t copy = (size_t)n;
            if (used >= capacity)
                copy = 0;
            else if (copy > capacity - used)
                copy = capacity - used;
            if (copy) memcpy(destination + used, line, copy);
            used += (size_t)n;
        }
    }
    return used;
}
