/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_usb_input_ps5.h"
#include <stdio.h>
#include <string.h>

int sceSysmoduleLoadModule(uint16_t id);
int sceSysmoduleGetModuleHandleInternal(uint32_t id, int32_t *handle);
int sceKernelDlsym(int32_t handle, const char *symbol, void **address);

enum { KEYBOARD_MODULE = 0x0106, MOUSE_MODULE = 0x00a9 };

static void say_line(void (*log)(const char *), const char *line)
{
    if (log) log(line);
}

/* The system's handle for a module it loaded: by its id, or its internal id (0x80000000 | id). */
static int32_t module_handle(uint16_t id)
{
    int32_t handle = -1;
    if (!sceSysmoduleGetModuleHandleInternal(id, &handle) && handle >= 0) return handle;
    handle = -1;
    if (!sceSysmoduleGetModuleHandleInternal(0x80000000u | id, &handle) && handle >= 0) return handle;
    return -1;
}

/* sceSysmoduleLoadModule after elevation is refused (0x80020063 on firmware 12.20). The preload, run while the
 * title was still a normal app, left the module mapped: a failed late load reuses that handle from module_handle. */
static int32_t load(uint16_t id, int *status)
{
    int32_t preloaded = module_handle(id);
    *status = sceSysmoduleLoadModule(id);
    if (*status == 0) {
        int32_t handle = module_handle(id);
        if (handle >= 0) return handle;
    }
    if (preloaded >= 0) return preloaded;
    return module_handle(id);
}

static char preload_line[2][128];
static int preload_logged;

void pw_usb_input_ps5_preload(void (*log)(const char *line))
{
    static const struct { const char *name; uint16_t id; } modules[] = {
        {"libSceKeyboard", KEYBOARD_MODULE}, {"libSceMouse", MOUSE_MODULE}};
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
        int status;
        int32_t handle = load(modules[i].id, &status);
        snprintf(preload_line[i], sizeof(preload_line[i]), "usb: preload %s id=%#x status=%#x handle=%d",
                 modules[i].name, (unsigned)modules[i].id, (unsigned)status, (int)handle);
        say_line(log, preload_line[i]);
    }
    preload_logged = 1;
}

static void open_keyboard(PwUsbInputPs5 *usb)
{
    int (*init)(void) = NULL;
    int (*open)(int32_t, int32_t, int32_t, const void *) = NULL;
    uint8_t param[8] = {0};
    char line[96 + 11 * PW_USB_INPUT_KEYBOARDS];
    int status, init_rc = -1;
    int32_t module = load(KEYBOARD_MODULE, &status);
    size_t at;

    if (module >= 0 && !sceKernelDlsym(module, "sceKeyboardInit", (void **)&init) &&
        !sceKernelDlsym(module, "sceKeyboardOpen", (void **)&open) &&
        !sceKernelDlsym(module, "sceKeyboardReadState", (void **)&usb->keyboard_read_state)) {
        init_rc = init();
        for (int32_t i = 0; i < PW_USB_INPUT_KEYBOARDS; i++)
            if ((usb->keyboard[i] = open(usb->user, 0, i, param)) >= 0) usb->keyboards++;
    }
    if (usb->keyboards || !usb->keyboard_logged) {
        at = (size_t)snprintf(line, sizeof(line), "usb: keyboard module=%d load=%#x init=%#x user=%d open=",
                              (int)module, (unsigned)status, (unsigned)init_rc, (int)usb->user);
        for (int i = 0; i < PW_USB_INPUT_KEYBOARDS && at < sizeof(line); i++)
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%s%#x", i ? "," : "", (unsigned)usb->keyboard[i]);
        if (status && module >= 0 && at < sizeof(line))
            snprintf(line + at, sizeof(line) - at, " preloaded");
        say_line(usb->log, line);
    }
    usb->keyboard_logged = !usb->keyboards;
}

static void open_mouse(PwUsbInputPs5 *usb)
{
    int (*init)(void) = NULL;
    int (*open)(int32_t, int32_t, int32_t, const void *) = NULL;
    uint8_t param[8] = {1};
    char line[160];
    int status, init_rc = -1, handle = -1;
    int32_t module = load(MOUSE_MODULE, &status);

    if (module >= 0 && !sceKernelDlsym(module, "sceMouseInit", (void **)&init) &&
        !sceKernelDlsym(module, "sceMouseOpen", (void **)&open) &&
        !sceKernelDlsym(module, "sceMouseRead", (void **)&usb->mouse_read)) {
        init_rc = init();
        handle = open(usb->user, 0, 0, param);
        if (handle < 0) {
            param[0] = 0;
            handle = open(usb->user, 0, 0, param);
        }
    }
    if (handle >= 0) usb->mouse = handle;
    if (handle >= 0 || !usb->mouse_logged) {
        snprintf(line, sizeof(line), "usb: mouse module=%d load=%#x init=%#x open=%#x merged=%u user=%d%s",
                 (int)module, (unsigned)status, (unsigned)init_rc, (unsigned)handle, (unsigned)param[0],
                 (int)usb->user, status && module >= 0 ? " preloaded" : "");
        say_line(usb->log, line);
    }
    usb->mouse_logged = handle < 0;
}

void pw_usb_input_ps5_open(PwUsbInputPs5 *usb, int32_t user, void (*log)(const char *line))
{
    memset(usb, 0, sizeof(*usb));
    usb->user = user;
    usb->mouse = -1;
    for (int i = 0; i < PW_USB_INPUT_KEYBOARDS; i++) usb->keyboard[i] = usb->keyboard_reads[i].connected = -1;
    usb->log = log;
    /* The preload ran before /data existed, so those say() lines are in title-early.log. Repeat them once the
     * session log is the sink. */
    if (preload_logged)
        for (int i = 0; i < 2; i++) say_line(log, preload_line[i]);
    open_keyboard(usb);
    open_mouse(usb);
}

static void dump(PwUsbInputPs5 *usb, const char *what, const uint8_t *bytes, size_t length)
{
    char line[32 + 2 * PW_USB_KEYBOARD_STATE];
    size_t at = (size_t)snprintf(line, sizeof(line), "usb: %s ", what);
    for (size_t i = 0; i < length && at + 3 < sizeof(line); i++)
        at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x", bytes[i]);
    say_line(usb->log, line);
}

/* One line each time a keyboard handle's read result changes: its first read, an error, unplugged, read again. */
static void note_read(PwUsbInputPs5 *usb, int index, PwUsbReads *reads, int rc, int connected)
{
    char line[96];
    if (rc != reads->rc || connected != reads->connected) {
        snprintf(line, sizeof(line), "usb: keyboard %d read rc=%#x connected=%d after %u reads", index, (unsigned)rc,
                 connected, reads->count);
        say_line(usb->log, line);
    }
    reads->rc = rc;
    reads->connected = connected;
    reads->count++;
}

unsigned pw_usb_input_ps5_poll(PwUsbInputPs5 *usb, unsigned tick, PwWineInput *out, unsigned capacity)
{
    size_t count = 0;

    if (tick && tick % PW_USB_INPUT_RETRY_TICKS == 0) {
        if (!usb->keyboards) open_keyboard(usb);
        if (usb->mouse < 0) open_mouse(usb);
    }
    if (usb->keyboards) {
        PwUsbKeyboard now = {0};
        for (int i = 0; i < PW_USB_INPUT_KEYBOARDS; i++) {
            uint8_t state[PW_USB_KEYBOARD_STATE] = {0};
            PwUsbKeyboard one;
            PwUsbReads *reads = &usb->keyboard_reads[i];
            int rc, connected;
            if (usb->keyboard[i] < 0) continue;
            rc = usb->keyboard_read_state(usb->keyboard[i], state);
            connected = !rc && pw_usb_keyboard_decode(state, &one);
            note_read(usb, i, reads, rc, connected);
            if (!connected) continue;
            if (reads->dumps < PW_USB_INPUT_DUMPS && (one.keys[0] || one.modifiers)) {
                char what[24];
                snprintf(what, sizeof(what), "keyboard %d state", i);
                dump(usb, what, state, sizeof(state));
                reads->dumps++;
            }
            pw_usb_keyboard_merge(&now, &one);
        }
        size_t key_events = pw_usb_keyboard_events(&usb->keys, &now, out + count, capacity - count);
        usb->keys_sent += key_events;
        count += key_events;
        usb->keys = now;
    }
    if (usb->mouse >= 0 && count < capacity) {
        uint8_t records[PW_USB_INPUT_MOUSE_READ * PW_USB_MOUSE_RECORD];
        int got = usb->mouse_read(usb->mouse, records, PW_USB_INPUT_MOUSE_READ);
        if (got > 0) {
            PwUsbMouse mouse = {0, 0, 0, 0, usb->buttons, usb->mouse_time};
            if (got > PW_USB_INPUT_MOUSE_READ) got = PW_USB_INPUT_MOUSE_READ;
            for (int i = 0; i < got && usb->mouse_dumps < PW_USB_INPUT_DUMPS; i++, usb->mouse_dumps++)
                dump(usb, "mouse record", records + (size_t)i * PW_USB_MOUSE_RECORD, PW_USB_MOUSE_RECORD);
            pw_usb_mouse_decode(records, got, &mouse);
            size_t mouse_events = pw_usb_mouse_events(usb->buttons, &mouse, out + count, capacity - count);
            usb->mouse_sent += mouse_events;
            count += mouse_events;
            usb->buttons = mouse.buttons;
            usb->mouse_time = mouse.newest;
        }
    }
    return (unsigned)count;
}
