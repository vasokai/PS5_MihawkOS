/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../wine/ps5/pw_gdi_present.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* Inside BeginPaint the unlock waits, however long the paint has already run. */
    assert(pw_gdi_present_on_unlock(1, 100, 0, 16) == PW_GDI_PRESENT_WAIT);
    assert(pw_gdi_present_on_unlock(1, 100, 50, 16) == PW_GDI_PRESENT_WAIT);
    assert(pw_gdi_present_on_unlock(2, 1000, 0, 16) == PW_GDI_PRESENT_WAIT);

    /* No timed drawing yet: nothing to publish. */
    assert(pw_gdi_present_on_unlock(0, 100, 0, 16) == PW_GDI_PRESENT_WAIT);

    /* A video thread, no paint: one 60 Hz frame, then the next unlock publishes. */
    assert(pw_gdi_present_on_unlock(0, 100, 90, 16) == PW_GDI_PRESENT_WAIT);
    assert(pw_gdi_present_on_unlock(0, 100, 85, 16) == PW_GDI_PRESENT_WAIT);
    assert(pw_gdi_present_on_unlock(0, 100, 84, 16) == PW_GDI_PRESENT_PERIOD);
    assert(pw_gdi_present_on_unlock(0, 100, 40, 16) == PW_GDI_PRESENT_PERIOD);

    /* The tick count wraps. Unsigned elapsed still reaches the period: 0xfffffff0 is 16 ticks
     * before the wrap, plus 10 after it is 26. 0xfffffffb is 5 before the wrap, plus 10 is 15. */
    assert(pw_gdi_present_on_unlock(0, 10, 0xfffffff0u, 16) == PW_GDI_PRESENT_PERIOD);
    assert(pw_gdi_present_on_unlock(0, 10, 0xfffffffb, 16) == PW_GDI_PRESENT_WAIT);

    puts("pw_gdi_present: ok");
    return 0;
}
