/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_fmem_budget.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char out[1024];
    int limit = 0;
    const char *mark = "battle" ".net";
    char battle[160], renderer[220];
    const char *wow = "\"C:\\\\Program Files (x86)\\\\Wow\\\\Wow.exe\"";
    snprintf(battle, sizeof(battle), "\"C:\\\\Program Files (x86)\\\\%s\\\\%s.exe\"", mark, mark);
    snprintf(renderer, sizeof(renderer), "\"C:\\\\Program Files (x86)\\\\%s\\\\%s Helper.exe\" --type=renderer --lang=en",
             mark, mark);

    assert(pw_fmem_budget_is_chromium(battle, battle));
    assert(!pw_fmem_budget_is_chromium(wow, wow));
    assert(pw_fmem_budget_is_renderer(renderer, renderer));
    assert(!pw_fmem_budget_is_renderer(battle, battle));
    assert(!pw_fmem_budget_is_renderer(wow, "--type=renderer"));

    assert(pw_fmem_budget_limit(NULL, &limit) == 1 && limit == 2);
    assert(pw_fmem_budget_limit("0", &limit) == 0);
    assert(pw_fmem_budget_limit("4", &limit) == 1 && limit == 4);
    assert(pw_fmem_budget_limit("nope", &limit) == 1 && limit == 2);
    assert(pw_fmem_budget_limit("99", &limit) == 1 && limit == 2);

    assert(pw_fmem_budget_rewrite(wow, wow, 2, out, sizeof(out)) == 0);
    assert(!strcmp(out, wow));
    assert(pw_fmem_budget_rewrite(renderer, renderer, 2, out, sizeof(out)) == 0);
    assert(!strcmp(out, renderer));
    assert(pw_fmem_budget_rewrite(battle, "", 2, out, sizeof(out)) == 0);
    assert(pw_fmem_budget_rewrite(battle, battle, 2, out, sizeof(out)) == 1);
    assert(strstr(out, "--in-process-gpu"));
    assert(strstr(out, "--renderer-process-limit=2"));
    assert(strstr(out, "--disable-features=AudioServiceOutOfProcess,SpareRendererForSitePerProcess"));
    assert(strstr(out, "--enable-features=NetworkServiceInProcess2"));
    assert(!strncmp(out, battle, strlen(battle)));

    {
        char already[240];
        snprintf(already, sizeof(already),
                 "\"Z:\\\\mihawk\\\\%s.exe\" --renderer-process-limit=1 --disable-features=SpareRendererForSitePerProcess",
                 mark);
        assert(pw_fmem_budget_rewrite(already, already, 2, out, sizeof(out)) == 1);
        assert(strstr(out, "--renderer-process-limit=1"));
        assert(!strstr(out, "--renderer-process-limit=2"));
        assert(strstr(out, "--disable-features=SpareRendererForSitePerProcess"));
        assert(!strstr(out, "AudioServiceOutOfProcess"));
        assert(strstr(out, "--in-process-gpu"));
    }
    assert(pw_fmem_budget_rewrite(battle, battle, 2, out, 32) == -1);
    return 0;
}
