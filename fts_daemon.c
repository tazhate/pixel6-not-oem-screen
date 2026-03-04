/*
 * fts_daemon — Pixel 6 non-OEM touch fix daemon
 *
 * Fixes two problems caused by the FTS IC (STMicro ftm5) in LOCKED_ACTIVE mode:
 *
 * Problem 1: LPA warmup
 *   After ~117ms of no touch the IC enters Low Power Active (LPA) mode.
 *   First touch after idle is missed. Fix: keepalive ping every 200ms
 *   during idle to keep IC in LOCKED_ACTIVE.
 *
 * Problem 2: Long press broken at ~308ms
 *   In LOCKED_ACTIVE mode the IC firmware detects "mutual frame flatness"
 *   (ongoing touch absorbed into baseline) and forces a recalibration at
 *   ~308ms. This sends EVT_ID_LEAVE_POINT → BTN_TOUCH UP even though finger
 *   is still on screen.
 *   Fix: on BTN_TOUCH UP, immediately re-arm LOCKED_ACTIVE. If finger is
 *   still present, IC re-detects it and sends a new ENTER_POINT → BTN_DOWN.
 *   App sees: DOWN → UP(spurious at 308ms) → DOWN → UP(real) → long press
 *   fires after ~800ms of continuous hold.
 *
 * Build (static, aarch64):
 *   aarch64-linux-gnu-gcc -static -O2 -pthread -o fts_daemon fts_daemon.c
 *
 * Deploy:
 *   adb push fts_daemon /data/local/tmp/fts_daemon
 *   adb shell "su -c '/data/local/tmp/fts_daemon &'"
 *
 * For persistence add to Magisk module service.sh (see service.sh).
 */

#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <linux/input.h>
#include <stdatomic.h>

#define PROC   "/proc/fts/driver_test"
#define INPUT  "/dev/input/event3"

static atomic_int touching = 0;

static void send_cmd(const char *cmd, int len) {
    int fd = open(PROC, O_WRONLY);
    if (fd < 0) return;
    write(fd, cmd, len);
    close(fd);
}

/*
 * Keepalive thread: sends "18 03 00" (LOCKED_ACTIVE) every 200ms during idle.
 * Stops sending during active touch to avoid disturbing IC state.
 * 200ms << 117ms LPA threshold, so IC never enters LPA between touches.
 */
static void *keepalive_thread(void *arg) {
    (void)arg;
    while (1) {
        if (!atomic_load(&touching))
            send_cmd("18 03 00\n", 9);
        usleep(200000); /* 200ms */
    }
    return NULL;
}

int main(void) {
    int fd = open(INPUT, O_RDONLY);
    if (fd < 0) return 1;

    /* Init: force touch active + locked active scan mode */
    send_cmd("A0 01\n", 6);
    send_cmd("18 03 00\n", 9);

    pthread_t tid;
    pthread_create(&tid, NULL, keepalive_thread, NULL);

    struct input_event ev;
    while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type != EV_KEY || ev.code != BTN_TOUCH)
            continue;

        if (ev.value == 1) {
            /* Touch DOWN: pause keepalive to avoid disturbing IC */
            atomic_store(&touching, 1);
        } else {
            /* Touch UP: resume keepalive and immediately re-arm.
             * If finger is still on screen (spurious UP from force-cal),
             * IC re-detects it and sends new ENTER_POINT. */
            atomic_store(&touching, 0);
            send_cmd("18 03 00\n", 9);
        }
    }
    return 0;
}
