/*
 * fts_filter — EVIOCGRAB + uinput touch event filter for Pixel 6 non-OEM screen
 *
 * Grabs the real touchscreen, filters force-cal glitches, forwards to uinput.
 *
 * Key insight: re-arm command (18 03 00) must be sent IMMEDIATELY on
 * BTN_TOUCH 0, not after SYN_REPORT. The IC needs the command within
 * a tight window to re-detect the finger.
 *
 * Build:
 *   aarch64-linux-gnu-gcc -static -O2 -pthread -o fts_filter fts_filter.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdatomic.h>
#include <time.h>

#define INPUT_DEV   "/dev/input/event3"
#define UINPUT_DEV  "/dev/uinput"
#define PROC_CMD    "/proc/fts/driver_test"

#define SUPPRESS_TIMEOUT_MS   600
#define MIN_HOLD_MS           280
#define KEEPALIVE_US          200000
#define MAX_FRAME             64
#define MAX_REARMS            12      /* max re-arms per touch before accepting UP */

#ifndef BUS_SPI
#define BUS_SPI 0x001c
#endif

enum state { ST_FORWARD, ST_SUPPRESS };

static atomic_int touching = 0;
static int g_input_fd  = -1;
static int g_uinput_fd = -1;

static int ic_cmd(const char *cmd, int len) {
    int fd = open(PROC_CMD, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "ic_cmd: open failed: %s\n", strerror(errno));
        return -1;
    }
    int r = write(fd, cmd, len);
    if (r != len)
        fprintf(stderr, "ic_cmd: write %d/%d: %s\n", r, len, strerror(errno));
    close(fd);
    return r;
}

static void *keepalive(void *arg) {
    (void)arg;
    for (;;) {
        if (!atomic_load(&touching))
            ic_cmd("18 03 00\n", 9);
        usleep(KEEPALIVE_US);
    }
}

static void on_signal(int sig) {
    (void)sig;
    if (g_input_fd >= 0) ioctl(g_input_fd, EVIOCGRAB, 0);
    if (g_uinput_fd >= 0) { ioctl(g_uinput_fd, UI_DEV_DESTROY); close(g_uinput_fd); }
    if (g_input_fd >= 0) close(g_input_fd);
    _exit(0);
}

static int create_uinput(void) {
    int fd = open(UINPUT_DEV, O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open " UINPUT_DEV); return -1; }

    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);

    int abs_codes[] = {
        ABS_MT_SLOT, ABS_MT_TOUCH_MAJOR, ABS_MT_TOUCH_MINOR,
        ABS_MT_ORIENTATION, ABS_MT_POSITION_X, ABS_MT_POSITION_Y,
        ABS_MT_TRACKING_ID, ABS_MT_PRESSURE, ABS_MT_DISTANCE
    };
    for (int i = 0; i < (int)(sizeof(abs_codes)/sizeof(abs_codes[0])); i++)
        ioctl(fd, UI_SET_ABSBIT, abs_codes[i]);

    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    struct uinput_user_dev ud;
    memset(&ud, 0, sizeof(ud));
    snprintf(ud.name, UINPUT_MAX_NAME_SIZE, "fts_filtered");
    ud.id.bustype = BUS_SPI;
    ud.id.vendor  = 0x0001;
    ud.id.product = 0x0002;
    ud.id.version = 1;

    ud.absmax[ABS_MT_SLOT]         = 9;
    ud.absmax[ABS_MT_TOUCH_MAJOR]  = 2032;
    ud.absmax[ABS_MT_TOUCH_MINOR]  = 2032;
    ud.absmin[ABS_MT_ORIENTATION]  = -4096;
    ud.absmax[ABS_MT_ORIENTATION]  = 4096;
    ud.absmax[ABS_MT_POSITION_X]   = 1079;
    ud.absmax[ABS_MT_POSITION_Y]   = 2399;
    ud.absmax[ABS_MT_TRACKING_ID]  = 65535;
    ud.absmax[ABS_MT_PRESSURE]     = 127;
    ud.absmax[ABS_MT_DISTANCE]     = 127;

    if (write(fd, &ud, sizeof(ud)) != sizeof(ud)) {
        perror("write uinput_user_dev"); close(fd); return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE"); close(fd); return -1;
    }
    return fd;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int frame_has_btn(const struct input_event *f, int n, int val) {
    for (int i = 0; i < n; i++)
        if (f[i].type == EV_KEY && f[i].code == BTN_TOUCH && f[i].value == val)
            return 1;
    return 0;
}

static int frame_get_pos(const struct input_event *f, int n, int *x, int *y) {
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (f[i].type == EV_ABS) {
            if (f[i].code == ABS_MT_POSITION_X) { *x = f[i].value; found = 1; }
            if (f[i].code == ABS_MT_POSITION_Y) { *y = f[i].value; found = 1; }
        }
    }
    return found;
}

static void track_frame(const struct input_event *f, int n,
                         int *x, int *y, int *pressure) {
    for (int i = 0; i < n; i++) {
        if (f[i].type == EV_ABS) {
            if (f[i].code == ABS_MT_POSITION_X) *x = f[i].value;
            if (f[i].code == ABS_MT_POSITION_Y) *y = f[i].value;
            if (f[i].code == ABS_MT_PRESSURE)   *pressure = f[i].value;
        }
    }
}

static void fwd_frame(int fd, const struct input_event *f, int n) {
    for (int i = 0; i < n; i++)
        write(fd, &f[i], sizeof(f[i]));
}

static void fwd_frame_filtered(int fd, const struct input_event *f, int n) {
    for (int i = 0; i < n; i++) {
        if (f[i].type == EV_KEY && f[i].code == BTN_TOUCH) continue;
        if (f[i].type == EV_ABS && f[i].code == ABS_MT_TRACKING_ID) continue;
        write(fd, &f[i], sizeof(f[i]));
    }
}

int main(void) {
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    setvbuf(stderr, NULL, _IONBF, 0);

    g_input_fd = open(INPUT_DEV, O_RDONLY);
    if (g_input_fd < 0) { perror("open " INPUT_DEV); return 1; }
    if (ioctl(g_input_fd, EVIOCGRAB, 1) < 0) {
        perror("EVIOCGRAB"); close(g_input_fd); return 1;
    }

    g_uinput_fd = create_uinput();
    if (g_uinput_fd < 0) {
        ioctl(g_input_fd, EVIOCGRAB, 0); close(g_input_fd); return 1;
    }

    /* Retry startup commands — proc file may be busy from previous instance */
    for (int i = 0; i < 10; i++) {
        if (ic_cmd("A0 01\n", 6) > 0) break;
        usleep(200000);
    }
    for (int i = 0; i < 10; i++) {
        if (ic_cmd("18 03 00\n", 9) > 0) break;
        usleep(200000);
    }

    pthread_t tid;
    pthread_create(&tid, NULL, keepalive, NULL);

    enum state st = ST_FORWARD;
    struct input_event frame[MAX_FRAME], suppressed[MAX_FRAME];
    int fc = 0, sc = 0;
    long long deadline = 0;
    int glitch_count = 0;
    int rearm_sent = 0;  /* flag: re-arm already sent for this UP */
    int rearms_this_touch = 0;  /* re-arm cycles in current continuous touch */

    int last_x = 0, last_y = 0, last_pressure = 0;
    int saved_x = 0, saved_y = 0;
    long long touch_down_time = 0;

    struct pollfd pfd = { .fd = g_input_fd, .events = POLLIN };
    long long t0 = now_ms();

    fprintf(stderr, "[%06lld] fts_filter v5: started (suppress=%dms, min_hold=%dms)\n",
            now_ms() - t0, SUPPRESS_TIMEOUT_MS, MIN_HOLD_MS);

    for (;;) {
        int tmo = -1;
        if (st == ST_SUPPRESS) {
            long long rem = deadline - now_ms();
            if (rem <= 0) {
                fwd_frame(g_uinput_fd, suppressed, sc);
                sc = 0;
                st = ST_FORWARD;
                atomic_store(&touching, 0);
                fprintf(stderr, "[%06lld] timeout → real UP\n", now_ms() - t0);
                continue;
            }
            tmo = (int)rem;
        }

        int r = poll(&pfd, 1, tmo);

        if (r == 0 && st == ST_SUPPRESS) {
            fwd_frame(g_uinput_fd, suppressed, sc);
            sc = 0;
            st = ST_FORWARD;
            atomic_store(&touching, 0);
            fprintf(stderr, "[%06lld] timeout → real UP\n", now_ms() - t0);
            continue;
        }
        if (r < 0) { if (errno == EINTR) continue; break; }

        struct input_event ev;
        if (read(g_input_fd, &ev, sizeof(ev)) != sizeof(ev)) break;

        /* === CRITICAL: Send re-arm IMMEDIATELY on BTN_TOUCH 0 ===
         * Don't wait for SYN_REPORT — the IC needs the command ASAP
         * to re-detect the finger (same timing as old fts_daemon). */
        if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 0) {
            if (st == ST_FORWARD) {
                long long hold_ms = now_ms() - touch_down_time;
                if (hold_ms >= MIN_HOLD_MS && rearms_this_touch < MAX_REARMS) {
                    int r = ic_cmd("18 03 00\n", 9);
                    rearm_sent = 1;
                    rearms_this_touch++;
                    fprintf(stderr, "[%06lld] BTN_UP → re-arm #%d (r=%d, hold=%lldms)\n",
                            now_ms() - t0, rearms_this_touch, r, hold_ms);
                } else if (rearms_this_touch >= MAX_REARMS) {
                    fprintf(stderr, "[%06lld] BTN_UP → re-arm LIMIT (%d), releasing\n",
                            now_ms() - t0, MAX_REARMS);
                }
            }
        }

        if (fc < MAX_FRAME) frame[fc++] = ev;
        if (ev.type != EV_SYN || ev.code != SYN_REPORT) continue;

        /* --- Frame complete --- */
        int has_up   = frame_has_btn(frame, fc, 0);
        int has_down = frame_has_btn(frame, fc, 1);

        switch (st) {
        case ST_FORWARD:
            if (has_up) {
                long long now = now_ms();
                long long hold_ms = now - touch_down_time;

                if (hold_ms < MIN_HOLD_MS || rearms_this_touch >= MAX_REARMS) {
                    /* Quick tap or re-arm limit reached → forward UP immediately */
                    fwd_frame(g_uinput_fd, frame, fc);
                    atomic_store(&touching, 0);
                    if (rearms_this_touch >= MAX_REARMS)
                        fprintf(stderr, "[%06lld] UP limit reached → fwd (hold=%lldms)\n",
                                now - t0, hold_ms);
                    else
                        fprintf(stderr, "[%06lld] quick tap (hold=%lldms) → fwd\n",
                                now - t0, hold_ms);
                    rearms_this_touch = 0;
                } else {
                    /* Re-arm was already sent on BTN_TOUCH 0 above.
                     * Just enter suppression and buffer the frame. */
                    memcpy(suppressed, frame, fc * sizeof(*frame));
                    sc = fc;
                    saved_x = last_x;
                    saved_y = last_y;
                    deadline = now + SUPPRESS_TIMEOUT_MS;
                    st = ST_SUPPRESS;
                    rearm_sent = 0;
                    fprintf(stderr, "[%06lld] UP hold=%lldms p=%d (%d,%d) → SUPPRESS\n",
                            now - t0, hold_ms, last_pressure, saved_x, saved_y);
                }
            } else {
                fwd_frame(g_uinput_fd, frame, fc);
                track_frame(frame, fc, &last_x, &last_y, &last_pressure);
                if (has_down) {
                    atomic_store(&touching, 1);
                    touch_down_time = now_ms();
                    rearms_this_touch = 0;
                }
            }
            break;

        case ST_SUPPRESS:
            if (has_down) {
                /* Any DOWN during SUPPRESS is force-cal re-detection.
                 * SUPPRESS only entered for holds >= 280ms (force-cal territory).
                 * Always suppress — don't use distance check (IC can re-detect
                 * at wildly different positions, causing false UP+DOWN). */
                long long elapsed = now_ms() - (deadline - SUPPRESS_TIMEOUT_MS);
                fwd_frame_filtered(g_uinput_fd, frame, fc);
                track_frame(frame, fc, &last_x, &last_y, &last_pressure);
                sc = 0;
                st = ST_FORWARD;
                atomic_store(&touching, 1);
                glitch_count++;
                fprintf(stderr, "[%06lld] glitch #%d suppressed %lldms\n",
                        now_ms() - t0, glitch_count, elapsed);
            } else {
                fwd_frame(g_uinput_fd, frame, fc);
                track_frame(frame, fc, &last_x, &last_y, &last_pressure);
            }
            break;
        }

        fc = 0;
    }

    on_signal(0);
    return 0;
}
