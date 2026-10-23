/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_fmem_census.h"
#include <stdio.h>
#include <string.h>

static const char *class_name(uint8_t klass)
{
    static const char *const names[PW_FMEM_CLASSES] = {"jit", "data", "file", "stack", "direct", "other"};
    return klass < PW_FMEM_CLASSES ? names[klass] : "other";
}

static int contains_ci(const char *text, const char *needle)
{
    size_t n;
    if (!text || !needle) return 0;
    n = strlen(needle);
    for (; *text; text++) {
        size_t i = 0;
        while (i < n && text[i] && (text[i] | 32) == needle[i]) i++;
        if (i == n) return 1;
    }
    return 0;
}

static int file_name(const char *name)
{
    size_t n;
    if (!name || !name[0]) return 0;
    if (strchr(name, '/') || strchr(name, '\\')) return 1;
    n = strlen(name);
    return (n >= 4 && (!strcmp(name + n - 4, ".dll") || !strcmp(name + n - 4, ".elf") ||
                       !strcmp(name + n - 4, ".prx") || !strcmp(name + n - 3, ".so")));
}

uint8_t pw_fmem_class(uint8_t flags, int32_t protection, const char *name)
{
    if (!protection) return PW_FMEM_OTHER;
    if ((flags & PW_VM_DIRECT) && !(flags & PW_VM_FLEXIBLE)) return PW_FMEM_DIRECT;
    if (flags & PW_VM_STACK) return PW_FMEM_STACK;
    if ((protection & 4) || contains_ci(name, "jit")) return PW_FMEM_JIT;
    if (flags & PW_VM_FLEXIBLE) return file_name(name) ? PW_FMEM_FILE : PW_FMEM_DATA;
    return PW_FMEM_OTHER;
}

size_t pw_fmem_format(const PwVmCensus *census, char *out, size_t size)
{
    uint64_t bytes[PW_FMEM_CLASSES];
    unsigned i, shown = 0;
    size_t used;
    int n;
    if (!out || !size) return 0;
    out[0] = 0;
    if (!census) return 0;
    memset(bytes, 0, sizeof(bytes));
    for (i = 0; i < census->count; i++) {
        const PwVmGroup *g = &census->groups[i];
        uint8_t klass = pw_fmem_class(g->flags, g->protection, g->name);
        bytes[klass] += g->resident;
    }
    n = snprintf(out, size, "jit=%llu data=%llu file=%llu stack=%llu direct=%llu other=%llu KiB",
                 (unsigned long long)(bytes[PW_FMEM_JIT] >> 10), (unsigned long long)(bytes[PW_FMEM_DATA] >> 10),
                 (unsigned long long)(bytes[PW_FMEM_FILE] >> 10), (unsigned long long)(bytes[PW_FMEM_STACK] >> 10),
                 (unsigned long long)(bytes[PW_FMEM_DIRECT] >> 10), (unsigned long long)(bytes[PW_FMEM_OTHER] >> 10));
    if (n < 0 || (size_t)n >= size) {
        out[size - 1] = 0;
        return size - 1;
    }
    used = (size_t)n;
    for (i = 0; i < census->count && shown < 8; i++) {
        const PwVmGroup *g = &census->groups[i];
        uint8_t klass;
        int piece;
        if (g->resident < (1u << 20)) continue;
        klass = pw_fmem_class(g->flags, g->protection, g->name);
        if (klass != PW_FMEM_JIT && klass != PW_FMEM_DATA && klass != PW_FMEM_FILE) continue;
        piece = snprintf(out + used, size - used, "%s%s=%s:%llu", shown ? "," : " ",
                         g->name[0] ? g->name : "-", class_name(klass),
                         (unsigned long long)(g->resident >> 10));
        if (piece < 0 || (size_t)piece >= size - used) break;
        used += (size_t)piece;
        shown++;
    }
    return used;
}

void pw_fmem_rollup(const PwFmemSample *samples, unsigned count, PwFmemRollup *out)
{
    typedef struct Acc {
        char name[PW_VM_NAME + 1];
        int32_t pid;
        uint64_t bytes;
        unsigned used;
    } Acc;
    Acc acc[PW_FMEM_SAMPLES];
    unsigned nacc = 0, i, j, processes = 0;
    int32_t pids[64];
    unsigned npids = 0;
    memset(out, 0, sizeof(*out));
    if (!samples) return;
    for (i = 0; i < count; i++) {
        const PwFmemSample *s = &samples[i];
        unsigned k;
        int seen = 0;
        if (s->klass < PW_FMEM_CLASSES) out->class_bytes[s->klass] += s->resident;
        for (k = 0; k < npids; k++) if (pids[k] == s->pid) { seen = 1; break; }
        if (!seen && npids < 64) pids[npids++] = s->pid;
        if (s->klass != PW_FMEM_JIT) continue;
        for (j = 0; j < nacc; j++)
            if (acc[j].pid == s->pid && !strcmp(acc[j].name, s->name)) {
                acc[j].bytes += s->resident;
                break;
            }
        if (j < nacc) continue;
        if (nacc == PW_FMEM_SAMPLES) { out->truncated = 1; continue; }
        memcpy(acc[nacc].name, s->name, sizeof(acc[nacc].name));
        acc[nacc].pid = s->pid;
        acc[nacc].bytes = s->resident;
        acc[nacc].used = 0;
        nacc++;
    }
    processes = npids;
    out->processes = processes;
    out->movable = out->class_bytes[PW_FMEM_DATA] + out->class_bytes[PW_FMEM_FILE];
    for (i = 0; i < nacc; i++) {
        unsigned copies = 0;
        uint64_t smallest = 0;
        if (acc[i].used) continue;
        for (j = i; j < nacc; j++) {
            if (strcmp(acc[i].name, acc[j].name)) continue;
            acc[j].used = 1;
            if (!copies || acc[j].bytes < smallest) smallest = acc[j].bytes;
            copies++;
        }
        if (copies >= 2) out->duplicate_code += (copies - 1) * smallest;
    }
}
