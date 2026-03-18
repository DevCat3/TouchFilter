/*
 * touch_filter.c - Himax HX83xxx ghost slot filter v2
 * Device: gta4lve (Unisoc T618)
 *
 * Root cause (confirmed from getevent log):
 *   Himax sends TOUCH_MAJOR/PRESSURE frames with no X/Y for slots that
 *   were never properly initialized with a position. These "ghost slots"
 *   cause games to read a stale last-known position (often the camera
 *   joystick area) and spin the camera uncontrollably.
 *
 *   NOTE: Himax ALSO sends pressure-only updates for REAL slots after
 *   the initial touch frame (normal behavior). We must NOT drop those.
 *
 * Fix:
 *   Track pos_ever_known per slot (set when first X+Y seen after
 *   tracking_id assigned, cleared on tracking_id = -1).
 *   Ghost = active slot + has_pressure this frame + pos_ever_known == 0
 *   Drop any frame containing a ghost slot.
 *
 * v2 changes vs v1:
 *   - Open with O_RDWR (required for EVIOCGRAB on some kernels)
 *   - EVIOCGRAB called AFTER uinput device is ready
 *   - Ghost detection uses pos_ever_known (persistent) not has_pos (per-frame)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>

#define MAX_SLOTS   10
#define INPUT_DEV   "/dev/input/event4"
#define LOG_PATH    "/data/local/tmp/touch_filter.log"

typedef struct {
    int  tracking_id;
    int  active;
    int  pos_ever_known;  /* got X AND Y at least once since tracking_id set */
    int  has_pressure;    /* got pressure/major this frame */
} Slot;

static Slot slots[MAX_SLOTS];
static int  cur_slot = 0;
static FILE *logf    = NULL;

#define MAX_PENDING 512
static struct input_event pending[MAX_PENDING];
static int pending_n = 0;

static void log_msg(const char *msg) {
    if (logf) { fputs(msg, logf); fputc('\n', logf); fflush(logf); }
}

static int uinput_setup(int src_fd) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open uinput"); return -1; }

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    strncpy(uidev.name, "himax-touchscreen-filtered", UINPUT_MAX_NAME_SIZE - 1);
    uidev.id.bustype = BUS_VIRTUAL;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0x5678;
    uidev.id.version = 1;

    struct input_absinfo abs;
#define COPY_ABS(code) \
    if (ioctl(src_fd, EVIOCGABS(code), &abs) == 0) { \
        uidev.absmin[code] = abs.minimum; \
        uidev.absmax[code] = abs.maximum; \
        uidev.absfuzz[code] = abs.fuzz; \
        uidev.absflat[code] = abs.flat; \
    }
    COPY_ABS(ABS_MT_SLOT)
    COPY_ABS(ABS_MT_TRACKING_ID)
    COPY_ABS(ABS_MT_POSITION_X)
    COPY_ABS(ABS_MT_POSITION_Y)
    COPY_ABS(ABS_MT_TOUCH_MAJOR)
    COPY_ABS(ABS_MT_WIDTH_MAJOR)
    COPY_ABS(ABS_MT_PRESSURE)
#undef COPY_ABS

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

int main(void) {
    logf = fopen(LOG_PATH, "a");
    log_msg("=== touch_filter v2 started (gta4lve Himax) ===");

    /* O_RDWR required for EVIOCGRAB on some kernels */
    int src = open(INPUT_DEV, O_RDWR);
    if (src < 0) { log_msg("ERROR: cannot open event4"); return 1; }

    int dst = uinput_setup(src);
    if (dst < 0) { log_msg("ERROR: uinput setup failed"); return 1; }

    /* Wait for uinput device to appear before grabbing */
    usleep(300000);

    if (ioctl(src, EVIOCGRAB, 1) < 0)
        log_msg("WARN: EVIOCGRAB failed");
    else
        log_msg("EVIOCGRAB OK");

    log_msg("Filter running.");

    memset(slots, 0, sizeof(slots));
    for (int i = 0; i < MAX_SLOTS; i++) slots[i].tracking_id = -1;

    /* per-frame X/Y seen flags to build pos_ever_known */
    int frame_x[MAX_SLOTS];
    int frame_y[MAX_SLOTS];
    memset(frame_x, 0, sizeof(frame_x));
    memset(frame_y, 0, sizeof(frame_y));

    struct input_event ev;
    while (read(src, &ev, sizeof(ev)) == sizeof(ev)) {

        if (ev.type == EV_ABS) {
            switch (ev.code) {
            case ABS_MT_SLOT:
                cur_slot = (ev.value >= 0 && ev.value < MAX_SLOTS) ? ev.value : 0;
                break;
            case ABS_MT_TRACKING_ID:
                slots[cur_slot].tracking_id = ev.value;
                if (ev.value == -1) {
                    slots[cur_slot].active        = 0;
                    slots[cur_slot].pos_ever_known = 0;
                } else {
                    slots[cur_slot].active = 1;
                    /* pos_ever_known stays 0 until we see X AND Y */
                }
                break;
            case ABS_MT_POSITION_X:
                frame_x[cur_slot] = 1;
                if (frame_y[cur_slot])
                    slots[cur_slot].pos_ever_known = 1;
                break;
            case ABS_MT_POSITION_Y:
                frame_y[cur_slot] = 1;
                if (frame_x[cur_slot])
                    slots[cur_slot].pos_ever_known = 1;
                break;
            case ABS_MT_TOUCH_MAJOR:
            case ABS_MT_WIDTH_MAJOR:
            case ABS_MT_PRESSURE:
                slots[cur_slot].has_pressure = 1;
                break;
            }
        }

        if (pending_n < MAX_PENDING)
            pending[pending_n++] = ev;

        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            int ghost = 0;
            for (int i = 0; i < MAX_SLOTS; i++) {
                if (slots[i].active &&
                    slots[i].has_pressure &&
                    !slots[i].pos_ever_known) {
                    ghost = 1;
                    break;
                }
            }

            if (!ghost) {
                for (int i = 0; i < pending_n; i++)
                    write(dst, &pending[i], sizeof(pending[i]));
            }

            /* reset per-frame flags */
            for (int i = 0; i < MAX_SLOTS; i++)
                slots[i].has_pressure = 0;
            memset(frame_x, 0, sizeof(frame_x));
            memset(frame_y, 0, sizeof(frame_y));
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
