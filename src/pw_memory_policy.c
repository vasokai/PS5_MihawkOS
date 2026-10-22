/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_memory_policy.h"
#include "pw_memory_guard.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip(const char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r') text++;
    return text;
}

/* 1 with *out a decimal MiB, 0 when text is not only a number (so "0" is off, and "abc" is not). */
static int parse_mib(const char *text, unsigned *out)
{
    char *end = NULL;
    unsigned long value;

    text = skip(text);
    if (!*text) return 0;
    value = strtoul(text, &end, 10);
    end = (char *)skip(end);
    if (end == text || *end || value > 1000000ul) return 0;
    *out = (unsigned)value;
    return 1;
}

static int key_is(const char *text, const char *key, const char **value)
{
    const size_t length = strlen(key);

    if (strncmp(text, key, length) || text[length] != '=') return 0;
    *value = text + length + 1;
    return 1;
}

int pw_session_memory_parse(const char *text, struct PwSessionMemory *out)
{
    memset(out, 0, sizeof(*out));
    if (!text) return 0;
    while (*text) {
        const char *line = skip(text);
        const char *eol = strpbrk(line, "\n");
        const char *value = NULL;
        char row[128];
        size_t length;
        unsigned mib = 0;

        if (!eol) eol = line + strlen(line);
        text = *eol ? eol + 1 : eol;
        length = (size_t)(eol - line);
        while (length && (line[length - 1] == '\r' || line[length - 1] == ' ' || line[length - 1] == '\t'))
            length--;
        if (!length || line[0] == '#') continue;
        if (length >= sizeof(row)) {
            out->ignored++;
            continue;
        }
        memcpy(row, line, length);
        row[length] = 0;
        if (key_is(row, "PROTON_SYSTEM_MEMORY_LIMIT", &value) && parse_mib(value, &mib)) {
            out->limit_set = 1;
            out->limit_mib = mib;
        } else if (key_is(row, "PROTON_SYSTEM_MEMORY_END", &value) && parse_mib(value, &mib)) {
            out->end_set = 1;
            out->end_mib = mib;
        } else if (key_is(row, "PROTON_SYSTEM_MEMORY_RAW", &value) && parse_mib(value, &mib)) {
            out->raw_set = 1;
            out->raw = mib != 0;
        } else {
            out->ignored++;
        }
    }
    return 0;
}

int pw_session_memory_load(struct PwSessionMemory *out, const char *path)
{
    FILE *file;
    char *text = NULL;
    size_t used = 0, capacity = 0;
    char buffer[512];
    size_t got;

    memset(out, 0, sizeof(*out));
    if (!path) return EINVAL;
    file = fopen(path, "rb");
    if (!file) return errno ? errno : ENOENT;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (used + got + 1 > capacity) {
            size_t next = capacity ? capacity * 2 : 1024;
            char *grown;
            while (next < used + got + 1) next *= 2;
            grown = realloc(text, next);
            if (!grown) {
                free(text);
                fclose(file);
                return ENOMEM;
            }
            text = grown;
            capacity = next;
        }
        memcpy(text + used, buffer, got);
        used += got;
    }
    if (ferror(file)) {
        const int error = errno ? errno : EIO;
        free(text);
        fclose(file);
        return error;
    }
    fclose(file);
    if (text) text[used] = 0;
    pw_session_memory_parse(text ? text : "", out);
    free(text);
    return 0;
}

const char *pw_memory_limit_source_name(int source)
{
    if (source == PW_MEMORY_LIMIT_FILE) return PW_SESSION_MEMORY_PATH;
    if (source == PW_MEMORY_LIMIT_ENVIRONMENT) return "the environment";
    return "the firmware default";
}

void pw_memory_limit_choose(unsigned firmware_default_mib, const struct PwSessionMemory *file,
                            const char *env_limit, const char *env_end, const char *env_raw,
                            struct PwMemoryLimitChoice *out)
{
    unsigned limit = firmware_default_mib;
    unsigned end_asked;
    unsigned firmware_end = firmware_default_mib + PW_MEMORY_GUARD_END_MARGIN_MIB;
    unsigned parsed = 0;
    int limit_explicit = 0, end_explicit = 0, raised, env_set_limit = 0, env_set_end = 0;

    memset(out, 0, sizeof(*out));
    out->source = PW_MEMORY_LIMIT_FIRMWARE;
    if (file && file->limit_set) {
        limit = file->limit_mib;
        limit_explicit = 1;
        out->source = PW_MEMORY_LIMIT_FILE;
    }
    if (file && file->end_set) {
        end_asked = file->end_mib;
        end_explicit = 1;
    } else {
        end_asked = 0;
    }
    if (file && file->raw_set) out->raw = file->raw;
    if (env_limit && parse_mib(env_limit, &parsed)) {
        limit = parsed;
        limit_explicit = 1;
        env_set_limit = 1;
        out->source = PW_MEMORY_LIMIT_ENVIRONMENT;
    }
    if (env_end && parse_mib(env_end, &parsed)) {
        end_asked = parsed;
        end_explicit = 1;
        env_set_end = 1;
    }
    /* An environment limit that names no end replaces the file's end: that end belonged to the file's limit. */
    if (env_set_limit && !env_set_end) end_explicit = 0;
    if (env_raw && parse_mib(env_raw, &parsed)) out->raw = parsed != 0;
    if (limit_explicit && limit == 0) {
        out->off = 1;
        out->limit_mib = 0;
        out->end_mib = 0;
        return;
    }
    if (!end_explicit) end_asked = limit + PW_MEMORY_GUARD_END_MARGIN_MIB;
    raised = limit_explicit && limit > firmware_default_mib;
    /* An end mark above the firmware's, without a raised limit, is how a run used to ask to go past the console
     * and turn the steep-climb rule off. Hold it to the firmware's. A limit the session raised may take its end
     * with it. */
    if (!raised && end_asked > firmware_end) end_asked = firmware_end;
    out->limit_mib = limit;
    out->end_mib = end_asked;
    out->above_default = raised;
}

void pw_memory_pressure_sample(struct PwMemoryPressure *pressure, const struct PwMemoryPressureIn *in,
                               struct PwMemoryPressureOut *out)
{
    uint64_t used = 0;
    int direct_known = in->direct_total > 0 && in->direct_free <= in->direct_total;
    uint64_t growth = 0, census_growth, explained;

    memset(out, 0, sizeof(*out));
    out->flexible_free = in->flexible_free;
    out->flexible_known = in->flexible_free != UINT64_MAX;
    if (direct_known) {
        used = in->direct_total - in->direct_free;
        pressure->direct_last = used;
        if (!pressure->have_direct) {
            pressure->direct_base = used;
            pressure->have_direct = 1;
        }
    } else if (pressure->have_direct) {
        used = pressure->direct_last;
        direct_known = 1;
    }
    out->direct_known = direct_known;
    out->direct_used = direct_known ? used : 0;
    if (pressure->have_direct && used > pressure->direct_base) growth = used - pressure->direct_base;
    if (!pressure->have_census) {
        pressure->census_base = in->census;
        pressure->have_census = 1;
    }
    census_growth = in->census > pressure->census_base ? in->census - pressure->census_base : 0;
    /* Never subtract more than the census rose, and subtract nothing when the session asked for the raw census. */
    if (!in->attribute_direct) explained = 0;
    else if (growth < census_growth) explained = growth;
    else explained = census_growth;
    out->direct_growth = growth;
    out->guarded = in->census - explained;
    out->flexible_low = out->flexible_known && in->flexible_reserve && in->flexible_free <= in->flexible_reserve;
    out->census_backstop = in->backstop && in->census >= in->backstop;
}
