# Pixel 6 Non-OEM Screen Touch Fix

Fix for touchscreen "warmup" bug after replacing the Pixel 6 (oriole) screen
with a non-OEM panel. After replacement, the first touch after a few seconds
idle is missed, and long press detection is unreliable.

## Status

| Issue | Status |
|-------|--------|
| Cold tap warmup (first touch after idle missed) | ✅ Fixed |
| Long press broken | 🔄 Partially working (see below) |
| Dead zones at bottom rows | ✅ Fixed (production autotune) |

## Quick Start

Requires root (Magisk).

### 1. Build the daemon

```bash
aarch64-linux-gnu-gcc -static -O2 -pthread -o fts_daemon fts_daemon.c
```

### 2. Deploy and run

```bash
adb push fts_daemon /data/local/tmp/fts_daemon
adb shell "su -c '/data/local/tmp/fts_daemon &'"
```

### 3. Apply boot-time baseline fixes (one-time)

```bash
# Sensitivity boost
adb shell "su -c 'echo 1 > /sys/bus/spi/drivers/fts/spi11.0/glove_mode'"

# Recalibrate per-node baseline for non-OEM panel geometry
adb shell "su -c 'echo \"01 00\" > /sys/bus/spi/drivers/fts/spi11.0/stm_fts_cmd'"
sleep 3
adb shell "su -c 'echo \"02 00\" > /sys/bus/spi/drivers/fts/spi11.0/stm_fts_cmd'"
sleep 2

# Disable grip/palm suppression (left-edge touch rejection)
adb shell "su -c 'resetprop vendor.twoshay.grip_enabled 0'"
adb shell "su -c 'resetprop vendor.twoshay.adaptive_touch_sensitivity 0'"
```

For persistence, add to Magisk module `service.sh` (see `service.sh`).

## How It Works

### Root Cause

The FTS IC (STMicroelectronics ftm5) enters **LPA (Low Power Active)** mode
after ~117ms of no touch. In LPA, the AOC coprocessor (Cortex-M55) owns the
SPI bus and scans at low rate. The first touch wakes the IC but is missed.

`SCAN_MODE_LOCKED + LOCKED_ACTIVE` (`18 03 00` via `/proc/fts/driver_test`)
prevents the IC from entering LPA, fixing the warmup issue.

### Long Press Side Effect

In LOCKED_ACTIVE mode, the IC firmware detects "mutual frame flatness" at
~308ms of sustained touch (finger absorbed into baseline) and forces a
recalibration. This sends `EVT_ID_LEAVE_POINT` → spurious `BTN_TOUCH UP`
even though the finger is still on screen.

`fts_daemon` detects this: on any `BTN_TOUCH UP` it immediately re-sends
`18 03 00`. If the finger is still present, the IC re-detects it and sends
a new `EVT_ID_ENTER_POINT` → `BTN_TOUCH DOWN`. Long press fires at ~800ms
of continuous hold (vs normal ~500ms).

### Daemon Behavior

```
[idle]     keepalive: 18 03 00 every 200ms  →  IC stays active, no LPA
[touch DOWN]   stop keepalive
[touch UP]     send 18 03 00 immediately
               if finger still on screen → IC re-detects → new BTN_DOWN
```

## Known Limitations

- Long press requires ~800ms hold instead of ~500ms
- Subsequent long presses occasionally inconsistent (active research)
- Daemon must be running — not yet integrated into Magisk module auto-start

## Key Commands Reference

All via `/proc/fts/driver_test`:

| Command | Effect |
|---------|--------|
| `echo "A0 01" > /proc/fts/driver_test` | Force touch active (AP owns SPI bus) |
| `echo "18 03 00" > /proc/fts/driver_test` | LOCKED_ACTIVE — prevents LPA ✅ |
| `echo "18 00 00" > /proc/fts/driver_test` | ACTIVE mode — LPA allowed ❌ |
| `echo "81 00" > /proc/fts/driver_test` | Disable background baseline adaptation |

## Next Steps / Open Problems

1. **Long press consistency** — daemon re-arm approach works but timing is
   fragile. Looking into kernel-level fix (kprobe on `fts_status_event_handler`).

2. **Kernel module fix** — `CONFIG_KPROBES=y` on this kernel. Plan:
   hook `EVT_TYPE_STATUS_FORCE_CAL` event handler, call `setScanMode(LOCKED_ACTIVE)`
   when `touch_id != 0` to prevent tracking loss without mode switch races.

3. **Persistent daemon** — integrate `fts_daemon` into Magisk module service.sh.

## Technical Details

See [FINDINGS.md](FINDINGS.md) for full research notes, driver internals,
all attempted approaches, and kernel module implementation plan.

## Device Info

| Item | Value |
|------|-------|
| Device | Pixel 6 (oriole) |
| SoC | Google Tensor GS101 |
| Touch IC | STMicro ftm5 (fts5cu56a) |
| Driver | `ftm5.ko` — `/vendor/lib/modules/ftm5.ko` |
| Kernel | 6.1.145-android14-11 |
| Android | 14 |
