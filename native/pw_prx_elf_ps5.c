/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_prx_elf_ps5.h"
#include "../src/pw_prx_data.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern int sceKernelReserveVirtualRange(void **address, size_t bytes, int flags, size_t alignment);
extern int sceKernelJitCreateSharedMemory(const char *name, size_t bytes, int max_protection, int *fd);
extern int sceKernelMmap(void *address, size_t bytes, int protection, int flags, int fd, off_t offset, void **out);
extern int sceKernelMprotect(const void *address, size_t bytes, int protection);
extern int sceKernelMunmap(void *address, size_t bytes);
extern int sceKernelAllocateDirectMemoryForApp(int64_t search_start, int64_t search_end, size_t bytes,
                                               size_t alignment, int type, int64_t *offset);
extern int64_t sceKernelGetDirectMemorySize(void);
extern int sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags, int64_t offset,
                                    size_t alignment);
extern int sceKernelReleaseDirectMemory(int64_t offset, size_t bytes);
extern int sceKernelDlsym(int handle, const char *name, void **address);

#define PAGE 0x4000u
#define MAX_IMAGES 24
#define MAX_PROTS 16

typedef struct Prot { uintptr_t address; size_t bytes; int prot; } Prot;
#define MAX_IMPORT_LIBS 16
typedef struct Loader {
    uintptr_t base, span;
    int mapped, failed;
    Prot prots[MAX_PROTS];
    unsigned prot_count;
    char import_libs[MAX_IMPORT_LIBS][64];   /* SCE_IMPORT_LIB index -> library name */
} Loader;

static PwPrxElfImage images[MAX_IMAGES];
static char image_paths[MAX_IMAGES][512];
static unsigned image_count;
typedef struct Export { char nid[32]; uintptr_t address; } Export;
static Export exports[16384];
static unsigned export_count;
static void *libc_handle, *kernel_handle;
static int libc_tried, kernel_tried;
static int libkernel_handle = 1, libc_module = -1, modules_scanned;

extern int sceKernelGetModuleList(int32_t *handles, size_t capacity, size_t *count);
extern int sceKernelGetModuleInfo(int handle, void *info);

/* The kernel's own modules (libkernel, and libSceLibcInternal when the title or the payload loaded it) are the
 * cheapest source for an import NID; find their handles once. */
static void scan_modules(void)
{
    int32_t handles[256];
    size_t count = 0;
    if (modules_scanned) return;
    modules_scanned = 1;
    if (sceKernelGetModuleList(handles, 256, &count)) return;
    for (size_t i = 0; i < count && i < 256; i++) {
        union { uint64_t size; unsigned char bytes[PW_PRX_MODULE_INFO_BYTES]; } info = { .size = PW_PRX_MODULE_INFO_BYTES };
        char name[PW_PRX_MAX_NAME];
        PwPrxSegment segments[PW_PRX_MAX_SEGMENTS];
        uint32_t segment_count = 0;
        if (sceKernelGetModuleInfo(handles[i], &info)) continue;
        if (pw_prx_parse_module_info(&info, name, segments, &segment_count)) continue;
        if (!strncmp(name, "libkernel", 9)) libkernel_handle = handles[i];
        else if (!strncmp(name, "libSceLibcInternal", 18)) libc_module = handles[i];
    }
}

static int elf_span(const unsigned char *bytes, size_t size, uintptr_t *min, uintptr_t *max)
{
    if (size < 64 || memcmp(bytes, "\177ELF", 4) || bytes[4] != 2 || bytes[5] != 1) return -1;
    const uint64_t phoff = *(const uint64_t *)(bytes + 0x20);
    const uint16_t phentsize = *(const uint16_t *)(bytes + 0x36), phnum = *(const uint16_t *)(bytes + 0x38);
    if (phentsize != 56 || phnum > 32 || phoff + (uint64_t)phentsize * phnum > size) return -1;
    uintptr_t lo = ~(uintptr_t)0, hi = 0;
    for (unsigned i = 0; i < phnum; i++) {
        const unsigned char *ph = bytes + phoff + (size_t)i * phentsize;
        if (*(const uint32_t *)ph != 1) continue;
        const uint64_t vaddr = *(const uint64_t *)(ph + 0x10), memsz = *(const uint64_t *)(ph + 0x28);
        if (!memsz) continue;
        if (vaddr < lo) lo = (uintptr_t)vaddr;
        if (vaddr + memsz > hi) hi = (uintptr_t)(vaddr + memsz);
    }
    if (hi <= lo) return -1;
    *min = lo; *max = hi;
    return 0;
}

static void *map_segment(void *opaque, uintptr_t address, size_t bytes, int prot)
{
    Loader *loader = opaque;
    (void)prot;
    if (loader->failed) return NULL;
    if (!loader->mapped) {
        void *out = NULL;
        if (sceKernelMmap((void *)loader->base, loader->span, PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0, &out) || out != (void *)loader->base) {
            loader->failed = 1;
            return NULL;
        }
        loader->mapped = 1;
    }
    if (address < loader->base || address + bytes > loader->base + loader->span) {
        loader->failed = 1;
        return NULL;
    }
    return (void *)address;
}

static int record_protection(void *opaque, uintptr_t address, size_t bytes, int prot)
{
    Loader *loader = opaque;
    if (loader->prot_count == MAX_PROTS) return -1;
    loader->prots[loader->prot_count++] = (Prot){address, bytes, prot};
    return 0;
}

/* Non-executable runs of a user-space .prx.elf are private anonymous flexible memory, one copy per process, and
 * the resident census counts them. Move each run onto the application pool (src/pw_prx_data.h). Text stays JIT:
 * execute does not stick on direct memory. WINE_PS5_PRX_DATA_DMEM=0 keeps today's mprotect. */
#define DMEM_TYPE 0x0c
#define DMEM_PROT 0x03
#define DMEM_FIXED 0x10

static int data_allocate(void *context, size_t bytes, uint64_t *offset)
{
    int64_t at = 0;
    (void)context;
    if (sceKernelAllocateDirectMemoryForApp(0, sceKernelGetDirectMemorySize(), bytes, PAGE, DMEM_TYPE, &at)) return -1;
    *offset = (uint64_t)at;
    return 0;
}
static int data_map_new(void *context, size_t bytes, uint64_t offset, uintptr_t *where)
{
    void *at = NULL;
    (void)context;
    if (sceKernelMapDirectMemory(&at, bytes, DMEM_PROT, 0, (int64_t)offset, PAGE) || !at) return -1;
    *where = (uintptr_t)at;
    return 0;
}
static int data_map_fixed(void *context, uintptr_t address, size_t bytes, uint64_t offset)
{
    void *at = (void *)address;
    (void)context;
    if (sceKernelMapDirectMemory(&at, bytes, DMEM_PROT, DMEM_FIXED, (int64_t)offset, PAGE) || at != (void *)address)
        return -1;
    return 0;
}
static int data_map_anon(void *context, uintptr_t address, size_t bytes)
{
    void *out = NULL;
    (void)context;
    if (sceKernelMmap((void *)address, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0, &out) ||
        out != (void *)address)
        return -1;
    return 0;
}
static int data_unmap(void *context, uintptr_t address, size_t bytes)
{
    (void)context;
    return sceKernelMunmap((void *)address, bytes) ? -1 : 0;
}
static int data_release(void *context, uint64_t offset, size_t bytes)
{
    (void)context;
    return sceKernelReleaseDirectMemory((int64_t)offset, bytes) ? -1 : 0;
}
static int data_protect(void *context, uintptr_t address, size_t bytes, int protection)
{
    (void)context;
    return sceKernelMprotect((void *)address, bytes, protection) ? -1 : 0;
}
static void data_copy(void *dst, const void *src, size_t bytes) { memcpy(dst, src, bytes); }

static const PwPrxDataOps data_ops = {
    NULL, PAGE, data_allocate, data_map_new, data_map_fixed, data_map_anon, data_unmap, data_release, data_protect,
    data_copy
};

/* The text is ordinary anonymous memory until here; a payload cannot keep execute on it, so copy it into a JIT
 * object mapped over the range and take RX (the same conversion wine/ps5/pw_wine_dmem_ps5.c measures). */
static int jit_over(uintptr_t address, size_t bytes)
{
    int fd = -1, status;
    void *scratch = NULL, *over = NULL;
    size_t rounded = (bytes + PAGE - 1) & ~(size_t)(PAGE - 1);
    if (!rounded) return -1;
    if ((status = sceKernelJitCreateSharedMemory("pw-prx", rounded, PROT_READ | PROT_WRITE | PROT_EXEC, &fd))) return status;
    if (sceKernelMmap(NULL, rounded, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0, &scratch) || !scratch) {
        close(fd); return -1;
    }
    memcpy(scratch, (const void *)address, bytes);
    if (sceKernelMmap((void *)address, rounded, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0, &over) ||
        over != (void *)address) {
        sceKernelMunmap(scratch, rounded); close(fd); return -1;
    }
    close(fd);
    memcpy((void *)address, scratch, bytes);
    sceKernelMunmap(scratch, rounded);
    return sceKernelMprotect((void *)address, rounded, PROT_READ | PROT_EXEC);
}

static void apply_protections(Loader *loader)
{
    uintptr_t at = loader->base, end = loader->base + loader->span;
    size_t direct_bytes = 0, flexible_bytes = 0;
    const char *skip = getenv("WINE_PS5_PRX_DATA_DMEM");
    const int move_data = !(skip && skip[0] == '0' && skip[1] == 0);
    while (at < end) {
        int want = 0;
        for (unsigned i = 0; i < loader->prot_count; i++) {
            const Prot *p = &loader->prots[i];
            if (at >= p->address && at < p->address + p->bytes) {
                if (p->prot & PROT_WRITE) want = PROT_READ | PROT_WRITE;
                else if ((p->prot & PROT_EXEC) && want != (PROT_READ | PROT_WRITE)) want = PROT_READ | PROT_EXEC;
                else if (!want) want = PROT_READ;
            }
        }
        if (!want) want = PROT_READ;
        uintptr_t run_end = at + PAGE;
        for (; run_end < end; run_end += PAGE) {
            int same = 1;
            for (unsigned i = 0; i < loader->prot_count; i++) {
                const Prot *p = &loader->prots[i];
                if (run_end >= p->address && run_end < p->address + p->bytes) {
                    int other = (p->prot & PROT_WRITE) ? (PROT_READ | PROT_WRITE)
                              : (p->prot & PROT_EXEC) ? (PROT_READ | PROT_EXEC) : PROT_READ;
                    if (other != want) { same = 0; break; }
                }
            }
            if (!same) break;
        }
        if (want == (PROT_READ | PROT_EXEC)) {
            if (jit_over(at, run_end - at)) fprintf(stderr, "pw_prx_elf: JIT %#lx+%#lx failed errno=%d\n",
                                                    (unsigned long)at, (unsigned long)(run_end - at), errno);
        } else {
            int moved = PW_PRX_DATA_KEPT;
            if (move_data) moved = pw_prx_data_move(&data_ops, at, run_end - at, want);
            if (moved == PW_PRX_DATA_BROKEN) {
                fprintf(stderr, "pw_prx_elf: data %#lx+%#lx lost errno=%d\n",
                        (unsigned long)at, (unsigned long)(run_end - at), errno);
                loader->failed = 1;
                return;
            }
            if (moved == PW_PRX_DATA_DIRECT) direct_bytes += run_end - at;
            else {
                flexible_bytes += run_end - at;
                if (sceKernelMprotect((void *)at, run_end - at, want))
                    fprintf(stderr, "pw_prx_elf: protect %#lx+%#lx -> %d failed errno=%d\n",
                            (unsigned long)at, (unsigned long)(run_end - at), want, errno);
            }
        }
        at = run_end;
    }
    fprintf(stderr, "pw_prx_elf: data direct=%zu flexible=%zu bytes\n", direct_bytes, flexible_bytes);
}

/* The import's middle field is a one-letter token, not a library name: the module's DT_SCE_IMPORT_LIB tags map the
 * token's number to the library (tools/check-title-imports.py reads the same table). Parse them once per image. */
static void parse_import_libs(const unsigned char *bytes, size_t size, Loader *loader)
{
    const uint64_t phoff = *(const uint64_t *)(bytes + 0x20);
    const uint16_t phentsize = *(const uint16_t *)(bytes + 0x36), phnum = *(const uint16_t *)(bytes + 0x38);
    uint64_t dynamic_off = 0, dynamic_size = 0;
    for (unsigned i = 0; i < phnum && phentsize == 56; i++) {
        const unsigned char *ph = bytes + phoff + (size_t)i * phentsize;
        const uint32_t kind = *(const uint32_t *)ph;
        const uint64_t offset = *(const uint64_t *)(ph + 8), filesz = *(const uint64_t *)(ph + 32);
        if (kind == 2) { dynamic_off = offset; dynamic_size = filesz; }
    }
    if (!dynamic_off || dynamic_off + dynamic_size > size) return;
    int64_t strtab_off = -1;
    uint64_t strsz = 0;
    for (uint64_t at = dynamic_off; at + 16 <= dynamic_off + dynamic_size; at += 16) {
        const int64_t tag = *(const int64_t *)(bytes + at);
        const uint64_t value = *(const uint64_t *)(bytes + at + 8);
        if (tag == 5) {
            for (unsigned i = 0; i < phnum && phentsize == 56 && strtab_off < 0; i++) {
                const unsigned char *ph = bytes + phoff + (size_t)i * phentsize;
                const uint64_t offset = *(const uint64_t *)(ph + 8), vaddr = *(const uint64_t *)(ph + 16);
                const uint64_t filesz = *(const uint64_t *)(ph + 32);
                if (*(const uint32_t *)ph == 1 && value >= vaddr && value < vaddr + filesz)
                    strtab_off = (int64_t)(offset + value - vaddr);
            }
        } else if (tag == 10) strsz = value;
    }
    if (strtab_off < 0 || (uint64_t)strtab_off + strsz > size) return;
    for (uint64_t at = dynamic_off; at + 16 <= dynamic_off + dynamic_size; at += 16) {
        const int64_t tag = *(const int64_t *)(bytes + at);
        const uint64_t value = *(const uint64_t *)(bytes + at + 8);
        if (tag != 0x61000049) continue;
        const unsigned index = (unsigned)(value >> 48);
        const uint32_t offset = (uint32_t)value;
        if (index >= MAX_IMPORT_LIBS || offset >= strsz) continue;
        const char *from = (const char *)bytes + strtab_off + offset;
        size_t length = strnlen(from, (size_t)(strsz - offset));
        if (length >= sizeof(loader->import_libs[0])) length = sizeof(loader->import_libs[0]) - 1;
        memcpy(loader->import_libs[index], from, length);
        loader->import_libs[index][length] = 0;
    }
}

/* A loaded module's own dynamic symbols are the exports the kernel loader can see: they are named NID#C#A (the NID
 * the importing modules use, plus the library and module tags). The descriptor's Wine names cover Wine's imports;
 * this covers an import written against the NID. */
static void parse_exports(const unsigned char *bytes, size_t size, uintptr_t base)
{
    const uint64_t phoff = *(const uint64_t *)(bytes + 0x20);
    const uint16_t phentsize = *(const uint16_t *)(bytes + 0x36), phnum = *(const uint16_t *)(bytes + 0x38);
    uint64_t dynamic_off = 0, dynamic_size = 0;
    if (phentsize != 56) return;
    for (unsigned i = 0; i < phnum; i++) {
        const unsigned char *ph = bytes + phoff + (size_t)i * phentsize;
        if (*(const uint32_t *)ph == 2) { dynamic_off = *(const uint64_t *)(ph + 8); dynamic_size = *(const uint64_t *)(ph + 32); }
    }
    if (!dynamic_off || dynamic_off + dynamic_size > size) return;
    uint64_t symtab = 0, strtab = 0, symsize = 0, strsz = 0;
    for (uint64_t at = dynamic_off; at + 16 <= dynamic_off + dynamic_size; at += 16) {
        const int64_t tag = *(const int64_t *)(bytes + at);
        const uint64_t value = *(const uint64_t *)(bytes + at + 8);
        if (tag == 6) symtab = value;
        else if (tag == 5) strtab = value;
        else if (tag == 10) strsz = value;
        else if (tag == 0x6100003f) symsize = value;
    }
    if (!symtab || !strtab || !symsize) return;
    int64_t sym_off = -1, str_off = -1;
    for (unsigned i = 0; i < phnum && phentsize == 56; i++) {
        const unsigned char *ph = bytes + phoff + (size_t)i * phentsize;
        const uint32_t kind = *(const uint32_t *)ph;
        const uint64_t offset = *(const uint64_t *)(ph + 8), vaddr = *(const uint64_t *)(ph + 16);
        const uint64_t filesz = *(const uint64_t *)(ph + 32);
        if (kind != 1) continue;
        if (symtab >= vaddr && symtab < vaddr + filesz) sym_off = (int64_t)(offset + symtab - vaddr);
        if (strtab >= vaddr && strtab < vaddr + filesz) str_off = (int64_t)(offset + strtab - vaddr);
    }
    if (sym_off < 0 || str_off < 0) return;
    for (uint64_t at = 0; at + 24 <= symsize && export_count < sizeof(exports) / sizeof(exports[0]); at += 24) {
        const unsigned char *sym = bytes + sym_off + at;
        const uint32_t name = *(const uint32_t *)sym;
        const uint16_t shndx = *(const uint16_t *)(sym + 6);
        const uint64_t value = *(const uint64_t *)(sym + 8);
        if (!shndx || !name || (uint64_t)str_off + name >= size) continue;
        const char *from = (const char *)bytes + str_off + name;
        size_t length = strnlen(from, 31);
        if (length < 1) continue;
        const char *hash = memchr(from, '#', length);
        if (!hash) continue;
        const size_t nid_length = (size_t)(hash - from);
        if (nid_length >= sizeof(exports[0].nid)) continue;
        memcpy(exports[export_count].nid, from, nid_length);
        exports[export_count].nid[nid_length] = 0;
        exports[export_count].address = base + (uintptr_t)value;
        export_count++;
    }
}

static uintptr_t resolve_import(void *opaque, const char *name, const char *library)
{
    Loader *loader = opaque;
    const char *exact = "";
    if (library && library[0] && !library[1]) {
        static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
        const char *hit = strchr(alphabet, library[0]);
        if (hit && (unsigned)(hit - alphabet) < MAX_IMPORT_LIBS) exact = loader->import_libs[hit - alphabet];
    }
    /* Wine's own exports first: a module's plain import names one of them. */
    for (unsigned i = 0; i < image_count; i++) {
        if (!images[i].descriptor) continue;
        const void *address = pw_prx_lookup(images[i].descriptor, name);
        if (address) return (uintptr_t)address;
    }
    for (unsigned i = 0; i < export_count; i++)
        if (!strcmp(exports[i].nid, name)) return exports[i].address;
    /* The library the module's own tag names, before the guesses below. */
    if (strstr(exact, "libSceLibcInternal")) {
        if (!libc_tried) {
            libc_tried = 1;
            libc_handle = dlopen("/system/common/lib/libSceLibcInternal.sprx", RTLD_LAZY);
            if (!libc_handle) libc_handle = dlopen("libSceLibcInternal.sprx", RTLD_LAZY);
        }
        if (libc_handle) { void *address = dlsym(libc_handle, name); if (address) return (uintptr_t)address; }
    } else if (strstr(exact, "libkernel")) {
        if (!kernel_tried) { kernel_tried = 1; kernel_handle = dlopen("libkernel.sprx", RTLD_LAZY); }
        if (kernel_handle) { void *address = dlsym(kernel_handle, name); if (address) return (uintptr_t)address; }
    } else if (exact[0]) {
        void *handle = dlopen(exact, RTLD_LAZY);
        if (handle) { void *address = dlsym(handle, name); if (address) return (uintptr_t)address; }
    }
    /* The system libraries: the module list's handles first, then the payload's dlopen for libc if the kernel has
     * not loaded it. The NID is an 11-character name; the payload SDK's symbol matcher accepts it. */
    scan_modules();
    {
        int handles[3] = { libkernel_handle, libc_module, 0x2001 };
        for (unsigned i = 0; i < 3; i++) {
            void *address = NULL;
            if (handles[i] != 0 && !sceKernelDlsym(handles[i], name, &address) && address) return (uintptr_t)address;
        }
    }
    if (!kernel_tried) {
        kernel_tried = 1;
        kernel_handle = dlopen("libkernel.sprx", RTLD_LAZY);
        fprintf(stderr, "pw_prx_elf: kernel handle=%d dlopen=%p errno=%d\n", libkernel_handle, kernel_handle, errno);
    }
    if (kernel_handle) {
        void *address = dlsym(kernel_handle, name);
        if (address) return (uintptr_t)address;
    }
    if (!libc_tried) {
        libc_tried = 1;
        libc_handle = dlopen("/system/common/lib/libSceLibcInternal.sprx", RTLD_LAZY);
        if (!libc_handle) libc_handle = dlopen("libSceLibcInternal.sprx", RTLD_LAZY);
        fprintf(stderr, "pw_prx_elf: libc module=%d dlopen=%p errno=%d\n", libc_module, libc_handle, errno);
    }
    if (libc_handle) {
        void *address = dlsym(libc_handle, name);
        if (address) return (uintptr_t)address;
    }
    if (libc_module < 0) { modules_scanned = 0; scan_modules(); }
    if (libc_module > 0) {
        void *address = NULL;
        if (!sceKernelDlsym(libc_module, name, &address) && address) return (uintptr_t)address;
    }
    fprintf(stderr, "pw_prx_elf: unresolved '%s' lib='%s' exact='%s' kernel=%d libc=%d\n", name,
            library ? library : "-", exact, libkernel_handle, libc_module);
    return 0;
}

int pw_prx_elf_ps5_needed(unsigned firmware)
{
    /* 10.01 loads the kernel's fake-signed modules in a payload; 12.00 and later stop at the pager. The word is
     * BCD: 0x13590001 is 13.59. */
    return (firmware >> 24) >= 0x12;
}

int pw_prx_elf_ps5_load(const char *path, PwPrxElfImage *image, char *error, size_t error_size)
{
    struct stat info;
    unsigned char *bytes = NULL;
    uintptr_t min = 0, max = 0;
    Loader loader = {0};
    int fd = -1, status = PW_PRX_ELF_ERR_ARGUMENT;

    if (error && error_size) error[0] = 0;
    if (!path || !image) return PW_PRX_ELF_ERR_ARGUMENT;
    if ((fd = open(path, O_RDONLY)) < 0) { status = PW_PRX_ELF_ERR_TRUNCATED; goto fail; }
    if (fstat(fd, &info) || info.st_size <= 0 || info.st_size > (64 << 20)) goto fail;
    bytes = malloc((size_t)info.st_size);
    if (!bytes) { status = PW_PRX_ELF_ERR_LIMIT; goto fail; }
    if (read(fd, bytes, (size_t)info.st_size) != info.st_size) goto fail;
    close(fd); fd = -1;
    if (elf_span(bytes, (size_t)info.st_size, &min, &max)) { status = PW_PRX_ELF_ERR_FORMAT; goto fail; }
    parse_import_libs(bytes, (size_t)info.st_size, &loader);
    loader.span = (max - min + PAGE - 1) & ~(size_t)(PAGE - 1);
    void *wanted = NULL;
    if (sceKernelReserveVirtualRange(&wanted, loader.span, 0, PAGE) || !wanted) {
        status = PW_PRX_ELF_ERR_MAP; goto fail;
    }
    loader.base = (uintptr_t)wanted - min;
    status = pw_prx_elf_load(bytes, (size_t)info.st_size, loader.base, map_segment, record_protection,
                             resolve_import, &loader, image);
    if (status != PW_PRX_ELF_OK || loader.failed) {
        if (status == PW_PRX_ELF_OK) status = PW_PRX_ELF_ERR_MAP;
        goto fail;
    }
    apply_protections(&loader);
    if (loader.failed) { status = PW_PRX_ELF_ERR_MAP; goto fail; }
    parse_exports(bytes, (size_t)info.st_size, image->base);
    /* Where the kernel put it: Wine's own tables do not know a mapping this loader makes (late-library on the base PS5). */
    fprintf(stderr, "pw_prx_elf: loaded %s at %#lx+%#lx\n", path, (unsigned long)(loader.base + min), (unsigned long)loader.span);
    if (image_count < MAX_IMAGES) {
        images[image_count] = *image;
        snprintf(image_paths[image_count], sizeof(image_paths[0]), "%s", path);
        image_count++;
    }
    free(bytes);
    return PW_PRX_ELF_OK;
fail:
    if (fd >= 0) close(fd);
    free(bytes);
    if (error && error_size)
        snprintf(error, error_size, "%s: status %d errno %d", path, status, errno);
    return status;
}

/* The ntdll's PwWineDlOps: the path it passes already names the .prx; the user-space image is the .prx.elf beside
 * it. The handle is the image's index + 1. */
int32_t pw_prx_elf_ps5_load_start(const char *path, size_t argc, const void *argv, uint32_t flags,
                                  const void *option, int *result)
{
    char elf_path[1300], error[192];
    PwPrxElfImage image;
    (void)argc; (void)argv; (void)flags; (void)option;
    if (result) *result = 0;
    if (!path) return -1;
    snprintf(elf_path, sizeof(elf_path), "%s.elf", path);
    if (pw_prx_elf_ps5_load(elf_path, &image, error, sizeof(error))) {
        if (result) *result = -1;
        fprintf(stderr, "pw_prx_elf: sibling %s failed: %s\n", elf_path, error);
        return -1;
    }
    (void)image;
    return (int32_t)image_count;
}

int pw_prx_elf_ps5_module_info(int32_t handle, void *info)
{
    PwPrxElfImage *image;
    uint8_t *raw = info;
    uint64_t size = 0;
    uint32_t count;
    char base[PW_PRX_MAX_NAME];
    const char *slash;
    if (!info || handle <= 0 || (unsigned)handle > image_count) return -1;
    image = &images[handle - 1];
    memset(info, 0, PW_PRX_MODULE_INFO_BYTES);
    memcpy(raw, &size, sizeof(size));
    memcpy(raw + 8, "module.prx", 10);
    slash = strrchr(image_paths[handle - 1], '/');
    snprintf(base, sizeof(base), "%s", slash ? slash + 1 : image_paths[handle - 1]);
    memcpy(raw + 8, base, sizeof(base) > PW_PRX_MAX_NAME ? PW_PRX_MAX_NAME - 1 : sizeof(base));
    count = image->segment_count < PW_PRX_MAX_SEGMENTS ? image->segment_count : PW_PRX_MAX_SEGMENTS;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *entry = raw + 0x108 + i * 16u;
        const uint64_t address = (uint64_t)(uintptr_t)image->segments[i].address;
        const uint32_t elf = image->segments[i].protection;
        const uint32_t prot = (elf & 2u) ? 3u : (elf & 1u) ? 5u : 1u;   /* RW, RX, R */
        memcpy(entry, &address, sizeof(address));
        memcpy(entry + 8, &image->segments[i].size, sizeof(uint32_t));
        memcpy(entry + 12, &prot, sizeof(prot));
    }
    memcpy(raw + 0x148, &count, sizeof(count));
    return 0;
}

int pw_prx_elf_ps5_last_handle(void)
{
    return (int)image_count;
}
