/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_USB_INPUT_PS5_H
#define PW_USB_INPUT_PS5_H
/* A USB keyboard and mouse on the console, read for the title's input (src/pw_usb_input.h turns what they report into
 * Windows input).
 *
 * The system's libSceKeyboard and libSceMouse are loaded as its modules (sceSysmoduleLoadModule, ids 0x0106 and
 * 0x00a9). pw_usb_input_ps5_preload loads them once, before elevation, while the title is still a normal app
 * (ps5/src/main.cpp). After the /data grant that load is refused (0x80020063 on firmware 12.20). A title may
 * also not load them by path (the same code, measured by prospero-win). The open then reuses the module the
 * preload left mapped, through module_handle. sce_sys/param.json has no library allow-list and does not name
 * either module. The payload SDK has no libSceMouse stub, so their functions are looked up by name
 * (sceKernelDlsym). They are opened for the user the pad is opened for. One mouse handle takes every mouse (the open parameter's first byte 1, "merged"); one opened
 * without it is tried next. Every keyboard index the system accepts is opened (PW_USB_INPUT_KEYBOARDS, as
 * ProsperoLight's validated input path does) and their keys are merged. A keyboard plugged in again is a new device
 * that the system assigns to a user again: 2026-10-19 (klog/usb-replug/run1), deviceId ...030100 then ...080100 with
 * userId 0xffffffff until the account prompt gave it back to the same user, while index 0's handle, opened before,
 * kept reading rc=0 connected=1 with no keys. While no keyboard and no mouse handle is open, a device is looked for
 * again every PW_USB_INPUT_RETRY_TICKS; a failure is logged once.
 *
 * Observability: each open is logged with the module handle and the init and open results (every keyboard index's);
 * each change of a keyboard handle's read result (rc, connected) is logged with its index and the reads so far; the
 * first PW_USB_INPUT_DUMPS keyboard states of each index and mouse records that carry data are logged in hex, so the
 * layouts in src/pw_usb_input.h are checked by the first run that touches a key. */
#include "../src/pw_usb_input.h"
#include <stdint.h>

enum {
    PW_USB_INPUT_RETRY_TICKS = 360, PW_USB_INPUT_DUMPS = 3, PW_USB_INPUT_MOUSE_READ = 64, PW_USB_INPUT_KEYBOARDS = 12,
};

typedef struct PwUsbReads {
    int rc, connected;                               /* the last read's; connected -1 before one says */
    unsigned count, dumps;                           /* reads; states logged in hex */
} PwUsbReads;

typedef struct PwUsbInputPs5 {
    int32_t user;
    int32_t keyboard[PW_USB_INPUT_KEYBOARDS], mouse; /* handles by keyboard index; an error (< 0) when not open */
    unsigned keyboards;                              /* keyboard handles open */
    int keyboard_logged, mouse_logged;               /* a failed open was logged */
    int (*keyboard_read_state)(int32_t handle, void *state);
    int (*mouse_read)(int32_t handle, void *records, int32_t count);
    PwUsbReads keyboard_reads[PW_USB_INPUT_KEYBOARDS];
    PwUsbKeyboard keys;                              /* every keyboard's, merged */
    uint32_t buttons;
    uint64_t mouse_time;
    unsigned mouse_dumps;
    uint64_t keys_sent, mouse_sent;                  /* events, for the title's log */
    void (*log)(const char *line);
} PwUsbInputPs5;

#ifdef __cplusplus
extern "C" {
#endif
/* Loads both modules once, before elevation, while the title is still a normal app. Logs
 * "usb: preload <lib> id=... status=... handle=...". */
void pw_usb_input_ps5_preload(void (*log)(const char *line));
/* Opens whichever devices the system has for user; the state is ready either way. */
void pw_usb_input_ps5_open(PwUsbInputPs5 *usb, int32_t user, void (*log)(const char *line));
/* One poll: the events since the last one into out (at most capacity), and a retry of a closed device every
 * PW_USB_INPUT_RETRY_TICKS ticks. Returns how many events were written. */
unsigned pw_usb_input_ps5_poll(PwUsbInputPs5 *usb, unsigned tick, PwWineInput *out, unsigned capacity);
#ifdef __cplusplus
}
#endif
#endif
