/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_GDI_PRESENT_H
#define PW_GDI_PRESENT_H
/* When a GDI unlock publishes its window on the PS5.
 *
 * The broker shows a window when its surface is flushed, and the flushed image is the
 * one DIB the window keeps. A desktop WM_PAINT is many GDI calls (the erase, the plates,
 * the icons, the rubber band). Flushing on the first unlock that is 16 ms after the
 * previous flush publishes that DIB halfway through the paint, and the desktop blinks
 * while the pointer moves. While this thread is inside BeginPaint, an unlock waits.
 * EndPaint publishes the finished frame, so the band and the icons appear together.
 *
 * A thread that draws without BeginPaint (a DirectShow renderer) still flushes once the
 * drawing has lasted period_ms. That is the path that shows a 24 fps cinematic at 24
 * rather than at about 14. Games that present through Vulkan do not come through here.
 *
 * paint_depth is how many BeginPaint calls on this thread have not reached EndPaint.
 * now and started are tick counts; started 0 means drawing has not been timed yet.
 * period_ms is the PS5 flush period (16). PW_GDI_PRESENT_WAIT leaves the surface
 * unpublished. PW_GDI_PRESENT_PERIOD means this unlock is a finished frame of a thread
 * that is not inside a paint. The depth itself lives in win32u's dce.c, one copy for
 * the module; this function only decides.
 */
#include <stdint.h>

enum {
    PW_GDI_PRESENT_WAIT = 0,
    PW_GDI_PRESENT_PERIOD = 1
};

static inline int pw_gdi_present_on_unlock(unsigned paint_depth, uint32_t now, uint32_t started,
                                           unsigned period_ms)
{
    if (paint_depth || !started) return PW_GDI_PRESENT_WAIT;
    if ((uint32_t)(now - started) < period_ms) return PW_GDI_PRESENT_WAIT;
    return PW_GDI_PRESENT_PERIOD;
}

#ifdef WINE_PS5_USER_DRIVER
/* Defined in dlls/win32u/dce.c. dibdrv's unlock reads the same thread-local. */
extern __thread unsigned pw_gdi_paint_depth;
#endif

#endif
