/*
 * touch_filter.c - Himax HX83xxx ghost slot filter
 * Device: gta4lve (Unisoc T618)
 *
 * Problem: SLOT 3 (and others) send TOUCH_MAJOR/PRESSURE updates
 *          with no X/Y — causing stale position to be used by games.
 * Fix:     Drop any SYN_REPORT frame where a slot updated pressure
 *          but sent no position in that frame.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>

#define MAX_SLOTS       10
#define INPUT_DEV       "/dev/input/event4"
#define LOG_PATH        "/data/local/tmp/touch_filter.log"

typedef struct {
    int  tracking_id;
    int  x, y;
    int  has_pos;      /* got X or Y this frame */
    int  has_pressure; /* got pressure/major this frame */
    int  active;
} Slot;

static Slot slots[MAX_SLOTS];
static int  cur_slot = 0;
static FILE *logf    = NULL;

/* pending events buffer for current frame */
#define MAX_PENDING 256
static struct input_event pending[MAX_PENDING];
static int pending_n = 0;

static void log_msg(const char *msg) {
    if (logf) { fputs(msg, logf); fputc('\n', logf); fflush(logf); }
}

static int uinput_setup(int src_fd) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open uinput"); return -1; }

    /* copy capabilities from source device */
    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    strncpy(uidev.name, "himax-touchscreen-filtered", UINPUT_MAX_NAME_SIZE - 1);
    uidev.id.bustype = BUS_VIRTUAL;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0x5678;
    uidev.id.version = 1;

    /* abs limits — read from real device */
    struct input_absinfo abs;
    ioctl(src_fd, EVIOCGABS(ABS_MT_POSITION_X), &abs);
    uidev.absmin[ABS_MT_POSITION_X]  = abs.minimum;
    uidev.absmax[ABS_MT_POSITION_X]  = abs.maximum;
    ioctl(src_fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs);
    uidev.absmin[ABS_MT_POSITION_Y]  = abs.minimum;
    uidev.absmax[ABS_MT_POSITION_Y]  = abs.maximum;
    ioctl(src_fd, EVIOCGABS(ABS_MT_SLOT), &abs);
    uidev.absmin[ABS_MT_SLOT]        = abs.minimum;
    uidev.absmax[ABS_MT_SLOT]        = abs.maximum;
    ioctl(src_fd, EVIOCGABS(ABS_MT_TRACKING_ID), &abs);
    uidev.absmin[ABS_MT_TRACKING_ID] = abs.minimum;
    uidev.absmax[ABS_MT_TRACKING_ID] = abs.maximum;
    ioctl(src_fd, EVIOCGABS(ABS_MT_TOUCH_MAJOR), &abs);
    uidev.absmax[ABS_MT_TOUCH_MAJOR] = abs.maximum;
    ioctl(src_fd, EVIOCGABS(ABS_MT_WIDTH_MAJOR), &abs);
    uidev.absmax[ABS_MT_WIDTH_MAJOR] = abs.maximum;
    ioctl(src_fd, EVIOCGABS(ABS_MT_PRESSURE), &abs);
    uidev.absmax[ABS_MT_PRESSURE]    = abs.maximum;

    ioctl(fd, UI_SET_EVBIT,  EV_ABS);
    ioctl(fd, UI_SET_EVBIT,  EV_SYN);
    ioctl(fd, UI_SET_EVBIT,  EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_WIDTH_MAJOR);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_PRESSURE);

    write(fd, &uidev, sizeof(uidev));
    ioctl(fd, UI_DEV_CREATE);
    return fd;
}

static void emit(int fd, __u16 type, __u16 code, __s32 val) {
    struct input_event ev = {0};
    ev.type  = type;
    ev.code  = code;
    ev.value = val;
    write(fd, &ev, sizeof(ev));
}

int main(void) {
    logf = fopen(LOG_PATH, "a");
    log_msg("=== touch_filter started (gta4lve Himax) ===");

    int src = open(INPUT_DEV, O_RDONLY);
    if (src < 0) { log_msg("ERROR: cannot open event4"); return 1; }

    /* grab the device — nobody else gets raw events */
    if (ioctl(src, EVIOCGRAB, 1) < 0) {
        log_msg("WARN: EVIOCGRAB failed — running without exclusive grab");
    }

    int dst = uinput_setup(src);
    if (dst < 0) { log_msg("ERROR: uinput setup failed"); return 1; }

    /* small delay for uinput device to appear */
    usleep(200000);
    log_msg("Filter running. Listening on event4...");

    memset(slots, 0, sizeof(slots));
    for (int i = 0; i < MAX_SLOTS; i++) slots[i].tracking_id = -1;

    struct input_event ev;
    while (read(src, &ev, sizeof(ev)) == sizeof(ev)) {

        if (ev.type == EV_ABS) {
            switch (ev.code) {
            case ABS_MT_SLOT:
                cur_slot = ev.value;
                if (cur_slot < 0 || cur_slot >= MAX_SLOTS) cur_slot = 0;
                break;
            case ABS_MT_TRACKING_ID:
                slots[cur_slot].tracking_id = ev.value;
                if (ev.value == -1) slots[cur_slot].active = 0;
                else                slots[cur_slot].active = 1;
                break;
            case ABS_MT_POSITION_X:
                slots[cur_slot].x       = ev.value;
                slots[cur_slot].has_pos = 1;
                break;
            case ABS_MT_POSITION_Y:
                slots[cur_slot].y       = ev.value;
                slots[cur_slot].has_pos = 1;
                break;
            case ABS_MT_TOUCH_MAJOR:
            case ABS_MT_WIDTH_MAJOR:
            case ABS_MT_PRESSURE:
                slots[cur_slot].has_pressure = 1;
                break;
            }
        }

        /* buffer event */
        if (pending_n < MAX_PENDING)
            pending[pending_n++] = ev;

        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            /*
             * Decision: does any slot have pressure-only update (no pos)?
             * If yes → ghost slot detected → drop entire frame.
             */
            int ghost = 0;
            for (int i = 0; i < MAX_SLOTS; i++) {
                if (slots[i].has_pressure && !slots[i].has_pos && slots[i].active) {
                    ghost = 1;
                    break;
                }
            }

            if (!ghost) {
                /* clean frame — forward as-is */
                for (int i = 0; i < pending_n; i++)
                    write(dst, &pending[i], sizeof(pending[i]));
            }
            /* else: drop silently */

            /* reset per-frame flags */
            for (int i = 0; i < MAX_SLOTS; i++) {
                slots[i].has_pos      = 0;
                slots[i].has_pressure = 0;
            }
            pending_n = 0;
        }
    }

    ioctl(src, EVIOCGRAB, 0);
    ioctl(dst, UI_DEV_DESTROY);
    close(src);
    close(dst);
    if (logf) fclose(logf);
    return 0;
}
