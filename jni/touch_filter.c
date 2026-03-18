/*
 * touch_filter.c  –  Himax HX83xxx ghost slot filter  v6
 * Device : gta4lve (Unisoc T618)
 * Author : DevCat3
 *
 * ── Why v1-v5 didn't fully fix the problem ──────────────────────────────
 * All previous versions tried to filter events *as they passed through*.
 * Some ghost data leaked because:
 *   • BTN_TOUCH was forwarded raw
 *   • Slot state transitions happened mid-stream
 *   • Ghost slots with stale X/Y from a previous finger were not caught
 *
 * ── v6 approach: state-reconstruction ──────────────────────────────────
 * 1. Parse the entire SYN_REPORT frame into a clean per-slot state table.
 * 2. Validate every slot via a strict state machine (IDLE→PENDING→ACTIVE).
 *    Ghost slots never reach ACTIVE.
 * 3. Reconstruct and emit fresh events from the validated state — raw
 *    events from the hardware NEVER touch the output device directly.
 * 4. Secondary defence: velocity/jump filter catches IC position glitches
 *    on already-ACTIVE slots.
 * 5. Pending-slot timeout: if a slot has a tracking_id but never provides
 *    X+Y within MAX_PENDING_FRAMES, force it to IDLE (ghost cleanup).
 *
 * ── Config file (optional) ──────────────────────────────────────────────
 * /data/local/tmp/touch_filter.conf
 *   MAX_JUMP=300          max pixel delta per frame before clamping
 *   MAX_PENDING_FRAMES=8  frames to wait for position before killing slot
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>

/* ── compat defines ─────────────────────────────────────────────────── */
#ifndef INPUT_PROP_DIRECT
#define INPUT_PROP_DIRECT 0x01
#endif
#ifndef UI_SET_PROPBIT
#define UI_SET_PROPBIT _IOW(UINPUT_IOCTL_BASE, 110, int)
#endif

/* ── tunables ────────────────────────────────────────────────────────── */
#define MAX_SLOTS            10
#define DEFAULT_MAX_JUMP     300   /* abs(dx)+abs(dy) threshold           */
#define DEFAULT_MAX_PENDING  8     /* frames before killing a ghost slot   */
#define INPUT_DEV            "/dev/input/event4"
#define LOG_PATH             "/data/local/tmp/touch_filter.log"
#define CONF_PATH            "/data/local/tmp/touch_filter.conf"
#define STATS_EVERY          500   /* log stats every N frames            */

/* ── slot state machine ─────────────────────────────────────────────── */
typedef enum {
    SLOT_IDLE    = 0,  /* no finger                                      */
    SLOT_PENDING = 1,  /* tracking_id set, waiting for first X+Y         */
    SLOT_ACTIVE  = 2,  /* position known, fully valid                    */
} SlotState;

typedef struct {
    SlotState state;
    SlotState prev_state;      /* state at start of current frame        */

    int  tracking_id;          /* -1 = none                              */

    /* last *emitted* position */
    int  x, y;

    /* this frame's incoming data */
    int  new_x,  new_y;
    int  got_x,  got_y;
    int  pressure, touch_major, width_major;
    int  has_pressure;

    /* velocity (delta between last two emitted positions) */
    int  vx, vy;

    /* counters */
    int  active_frames;        /* frames spent in SLOT_ACTIVE            */
    int  pending_frames;       /* frames spent in SLOT_PENDING           */
} Slot;

/* ── globals ─────────────────────────────────────────────────────────── */
static Slot  slots[MAX_SLOTS];
static int   cur_slot         = 0;
static FILE *logf             = NULL;
static int   cfg_max_jump     = DEFAULT_MAX_JUMP;
static int   cfg_max_pending  = DEFAULT_MAX_PENDING;

/* stats */
static long stat_total   = 0;
static long stat_ghosts  = 0;   /* ghost slots cancelled               */
static long stat_jumps   = 0;   /* position jumps clamped              */

/* ── helpers ─────────────────────────────────────────────────────────── */
static void log_msg(const char *msg) {
    if (!logf) return;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    fprintf(logf, "[%02d:%02d:%02d] %s\n",
            tm->tm_hour, tm->tm_min, tm->tm_sec, msg);
    fflush(logf);
}

static void load_config(void) {
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "MAX_JUMP=%d",          &v) == 1 && v > 0) cfg_max_jump    = v;
        if (sscanf(line, "MAX_PENDING_FRAMES=%d", &v) == 1 && v > 0) cfg_max_pending = v;
    }
    fclose(f);
    char buf[96];
    snprintf(buf, sizeof(buf), "Config: MAX_JUMP=%d MAX_PENDING=%d",
             cfg_max_jump, cfg_max_pending);
    log_msg(buf);
}

static inline int iabs(int x) { return x < 0 ? -x : x; }

static void emit(int fd, __u16 type, __u16 code, __s32 val) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.code  = code;
    ev.value = val;
    write(fd, &ev, sizeof(ev));
}

/* ── uinput setup ────────────────────────────────────────────────────── */
static int uinput_setup(int src_fd) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { log_msg("ERROR: open /dev/uinput failed"); return -1; }

    /* mark as direct touchscreen — prevents phantom mouse cursor */
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

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
        uidev.absmin[code]  = abs.minimum; \
        uidev.absmax[code]  = abs.maximum; \
        uidev.absfuzz[code] = abs.fuzz;    \
        uidev.absflat[code] = abs.flat;    \
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
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        log_msg("ERROR: UI_DEV_CREATE failed");
        close(fd);
        return -1;
    }
    return fd;
}

/* ═══════════════════════════════════════════════════════════════════════
 * process_frame — called on every SYN_REPORT
 *
 * Phase 1 : state transitions  (parse frame data → update slot states)
 * Phase 2 : emission           (reconstruct clean events from state)
 * ═══════════════════════════════════════════════════════════════════════ */
static void process_frame(int dst) {
    stat_total++;

    /* ── Phase 1: state transitions ─────────────────────────────────── */
    for (int i = 0; i < MAX_SLOTS; i++) {
        Slot *s = &slots[i];
        s->prev_state = s->state;  /* snapshot before transitions        */

        /* ① Finger lifted */
        if (s->tracking_id == -1) {
            s->state          = SLOT_IDLE;
            s->active_frames  = 0;
            s->pending_frames = 0;
            s->vx = s->vy    = 0;
            continue;
        }

        /* ② Got both X and Y this frame */
        if (s->got_x && s->got_y) {
            if (s->state == SLOT_IDLE || s->state == SLOT_PENDING) {
                /* new finger fully initialized → ACTIVE */
                s->state          = SLOT_ACTIVE;
                s->x              = s->new_x;
                s->y              = s->new_y;
                s->vx             = 0;
                s->vy             = 0;
                s->active_frames  = 1;
                s->pending_frames = 0;
            } else {
                /* existing finger moved */
                int ddx = iabs(s->new_x - s->x);
                int ddy = iabs(s->new_y - s->y);
                if (s->active_frames > 3 && (ddx + ddy) > cfg_max_jump) {
                    /*
                     * Position jumped too far — IC glitch.
                     * Clamp to velocity-predicted position instead.
                     */
                    s->x += s->vx;
                    s->y += s->vy;
                    stat_jumps++;
                } else {
                    s->vx = s->new_x - s->x;
                    s->vy = s->new_y - s->y;
                    s->x  = s->new_x;
                    s->y  = s->new_y;
                }
                s->active_frames++;
            }
            continue;
        }

        /* ③ No position update this frame */
        if (s->state == SLOT_ACTIVE) {
            /* normal: existing finger just updated pressure */
            s->active_frames++;
            continue;
        }

        if (s->state == SLOT_PENDING) {
            s->pending_frames++;
            if (s->pending_frames > cfg_max_pending) {
                /*
                 * Ghost slot: has tracking_id, has pressure,
                 * but NEVER provided a position. Kill it.
                 */
                s->state          = SLOT_IDLE;
                s->tracking_id    = -1;
                s->pending_frames = 0;
                stat_ghosts++;
            }
            /* else: still waiting — don't emit anything yet */
        }
        /* SLOT_IDLE with pressure but no tracking_id → ignore (ghost) */
    }

    /* ── Phase 2: emit reconstructed events ─────────────────────────── */

    /* Compute BTN_TOUCH before and after */
    int prev_touch = 0, next_touch = 0;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slots[i].prev_state == SLOT_ACTIVE) prev_touch = 1;
        if (slots[i].state      == SLOT_ACTIVE) next_touch = 1;
    }

    /* BTN_TOUCH DOWN */
    if (!prev_touch && next_touch)
        emit(dst, EV_KEY, BTN_TOUCH, 1);

    for (int i = 0; i < MAX_SLOTS; i++) {
        Slot *s = &slots[i];

        /* Slot going IDLE: send lift event */
        if (s->prev_state == SLOT_ACTIVE && s->state == SLOT_IDLE) {
            emit(dst, EV_ABS, ABS_MT_SLOT,        i);
            emit(dst, EV_ABS, ABS_MT_TOUCH_MAJOR, 0);
            emit(dst, EV_ABS, ABS_MT_WIDTH_MAJOR,  0);
            emit(dst, EV_ABS, ABS_MT_PRESSURE,     0);
            emit(dst, EV_ABS, ABS_MT_TRACKING_ID,  -1);
            continue;
        }

        /* Skip non-ACTIVE slots entirely */
        if (s->state != SLOT_ACTIVE) continue;

        emit(dst, EV_ABS, ABS_MT_SLOT, i);

        /* tracking_id only when first becoming active */
        if (s->prev_state != SLOT_ACTIVE)
            emit(dst, EV_ABS, ABS_MT_TRACKING_ID, s->tracking_id);

        /* position — emit only axes that were updated this frame */
        if (s->got_x) emit(dst, EV_ABS, ABS_MT_POSITION_X, s->x);
        if (s->got_y) emit(dst, EV_ABS, ABS_MT_POSITION_Y, s->y);

        /* pressure / size */
        if (s->has_pressure) {
            emit(dst, EV_ABS, ABS_MT_TOUCH_MAJOR, s->touch_major);
            emit(dst, EV_ABS, ABS_MT_WIDTH_MAJOR,  s->width_major);
            emit(dst, EV_ABS, ABS_MT_PRESSURE,     s->pressure);
        }
    }

    /* BTN_TOUCH UP */
    if (prev_touch && !next_touch)
        emit(dst, EV_KEY, BTN_TOUCH, 0);

    emit(dst, EV_SYN, SYN_REPORT, 0);

    /* ── reset per-frame accumulators ───────────────────────────────── */
    for (int i = 0; i < MAX_SLOTS; i++) {
        slots[i].got_x        = 0;
        slots[i].got_y        = 0;
        slots[i].has_pressure = 0;
        slots[i].pressure     = 0;
        slots[i].touch_major  = 0;
        slots[i].width_major  = 0;
    }

    /* periodic stats */
    if (stat_total % STATS_EVERY == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "Stats: frames=%ld  ghosts=%ld  jumps=%ld",
                 stat_total, stat_ghosts, stat_jumps);
        log_msg(buf);
    }
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(void) {
    logf = fopen(LOG_PATH, "a");
    log_msg("=== touch_filter v6 started (gta4lve / Himax) ===");
    log_msg("Mode: state-reconstruction (ghost-safe)");

    load_config();

    int src = open(INPUT_DEV, O_RDWR);
    if (src < 0) { log_msg("ERROR: cannot open " INPUT_DEV); return 1; }

    int dst = uinput_setup(src);
    if (dst < 0) return 1;

    usleep(300000);  /* wait for uinput device to appear */

    if (ioctl(src, EVIOCGRAB, 1) < 0)
        log_msg("WARN: EVIOCGRAB failed");
    else
        log_msg("EVIOCGRAB OK");

    log_msg("Filter running.");

    /* initialise slot table */
    memset(slots, 0, sizeof(slots));
    for (int i = 0; i < MAX_SLOTS; i++) {
        slots[i].tracking_id = -1;
        slots[i].state       = SLOT_IDLE;
        slots[i].prev_state  = SLOT_IDLE;
    }

    struct input_event ev;
    while (read(src, &ev, sizeof(ev)) == sizeof(ev)) {

        if (ev.type == EV_ABS) {
            switch (ev.code) {

            case ABS_MT_SLOT:
                cur_slot = (ev.value >= 0 && ev.value < MAX_SLOTS)
                           ? ev.value : 0;
                break;

            case ABS_MT_TRACKING_ID:
                slots[cur_slot].tracking_id = ev.value;
                if (ev.value != -1 && slots[cur_slot].state == SLOT_IDLE)
                    slots[cur_slot].state = SLOT_PENDING;
                break;

            case ABS_MT_POSITION_X:
                slots[cur_slot].new_x = ev.value;
                slots[cur_slot].got_x = 1;
                break;

            case ABS_MT_POSITION_Y:
                slots[cur_slot].new_y = ev.value;
                slots[cur_slot].got_y = 1;
                break;

            case ABS_MT_TOUCH_MAJOR:
                slots[cur_slot].touch_major = ev.value;
                if (ev.value > 0) slots[cur_slot].has_pressure = 1;
                break;

            case ABS_MT_WIDTH_MAJOR:
                slots[cur_slot].width_major = ev.value;
                break;

            case ABS_MT_PRESSURE:
                slots[cur_slot].pressure = ev.value;
                if (ev.value > 0) slots[cur_slot].has_pressure = 1;
                break;
            }
        }

        /* EV_KEY BTN_TOUCH is ignored — we reconstruct it from slot state */

        if (ev.type == EV_SYN && ev.code == SYN_REPORT)
            process_frame(dst);
    }

    ioctl(src, EVIOCGRAB, 0);
    ioctl(dst, UI_DEV_DESTROY);
    close(src);
    close(dst);
    if (logf) fclose(logf);
    return 0;
}
