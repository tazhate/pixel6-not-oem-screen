# Pixel 6 Non-OEM Screen Touch Fix

Fix for touchscreen bugs after replacing the Pixel 6 (oriole) screen with a
non-OEM panel. After replacement, the first touch after idle is missed, long
press and key repeat (hold backspace) are broken.

## Status

| Issue | Status |
|-------|--------|
| Cold tap warmup (first touch after idle missed) | Fixed |
| Long press broken at 308ms | Fixed (fts_filter + kprobe suppress force-cal) |
| Hold key repeat (backspace etc.) | Fixed (kprobe: in-kernel suppression) |
| Dead zones at bottom rows | Fixed (production autotune) |
| Fast typing artifacts | Fixed (MIN_HOLD_MS gate) |

Two complementary solutions:
- **`fts_filter`** (userspace) — EVIOCGRAB + uinput filter, ~4s continuous hold limit
- **`ftm5_kprobe`** (kernel) — in-kernel force-cal suppression via kprobes, eliminates BTN_UP at source

## Quick Start

Requires root (Magisk).

### 1. Build the filter

```bash
aarch64-linux-gnu-gcc -static -O2 -pthread -o fts_filter fts_filter.c
```

### 2. Deploy and run

```bash
adb push fts_filter /data/local/tmp/fts_filter
adb shell "su -c 'nohup /data/local/tmp/fts_filter > /data/local/tmp/fts_filter.log 2>&1 &'"
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

## Architecture

```
IC (ftm5) → kernel driver → /dev/input/event3 → [fts_filter EVIOCGRAB]
                                                       ↓ (filter)
                                                  /dev/uinput → Android
```

`fts_filter` exclusively grabs the real touchscreen via EVIOCGRAB, filters
force-calibration glitches, and forwards clean events to a virtual uinput
device that Android uses as its touchscreen.

## How It Works

### Root Cause

The FTS IC (STMicroelectronics ftm5) enters **LPA (Low Power Active)** mode
after ~117ms of no touch. In LPA, the AOC coprocessor (Cortex-M55) owns the
SPI bus and scans at low rate. The first touch wakes the IC but is missed.

`SCAN_MODE_LOCKED + LOCKED_ACTIVE` (`18 03 00` via `/proc/fts/driver_test`)
prevents the IC from entering LPA, fixing the warmup issue.

### Force-Cal Side Effect

In LOCKED_ACTIVE mode, the IC firmware detects "mutual frame flatness" at
~308ms of sustained touch (finger absorbed into baseline) and forces a
recalibration. This sends `EVT_ID_LEAVE_POINT` -> spurious `BTN_TOUCH UP`
even though the finger is still on screen.

### fts_filter Solution

**State machine** with two states:

**ST_FORWARD** (normal):
- All events forwarded to uinput as-is
- On BTN_TOUCH UP with hold >= 280ms: send re-arm (`18 03 00`) immediately,
  enter ST_SUPPRESS

**ST_SUPPRESS** (waiting for force-cal re-detection):
- Buffer the UP frame, don't forward to Android
- Wait up to 600ms for IC to re-detect the finger
- If BTN_TOUCH DOWN arrives: suppress the glitch (Android never sees UP+DOWN)
- If timeout: forward buffered UP (genuine finger lift)
- Max 12 re-arm cycles per continuous touch (IC degrades after ~15 cycles)

**Additional features:**
- Keepalive thread sends `18 03 00` every 200ms during idle (prevents LPA)
- Keepalive paused during active touch and SUPPRESS (avoids IC interference)
- Quick taps (< 280ms) bypass suppression entirely (no delay)
- Startup retry loop for IC commands (handles proc file busy race)

### Key Parameters

| Parameter | Value | Purpose |
|-----------|-------|---------|
| SUPPRESS_TIMEOUT_MS | 600 | Max wait for re-detection after force-cal |
| MIN_HOLD_MS | 280 | Touches shorter than this bypass suppression |
| MAX_REARMS | 12 | Max re-arm cycles before accepting UP |
| KEEPALIVE_US | 200000 | Keepalive interval (200ms) |

## Kernel Kprobe Module (ftm5_kprobe)

In-kernel solution that intercepts force-cal events before they reach Android.
Unlike fts_filter (userspace), this prevents the spurious BTN_TOUCH UP at the
kernel driver level — Android never sees it.

### How It Works

Three kprobes on ftm5 driver functions:

1. **`fts_leave_pointer_event_handler`** (pre+post) — saves `touch_id` bitmask
   before the handler clears it
2. **`fts_status_event_handler`** (pre) — on force-cal event (`event[1]==0x05`),
   restores saved `touch_id` so the event loop sees `touch_id != 0` → no BTN_UP
3. **`fts_enter_pointer_event_handler`** (pre) — resets rearm counter on new
   touch-down

After restoring `touch_id`, schedules `setScanMode(LOCKED_ACTIVE)` via workqueue
to re-arm the IC for the next cycle.

### Build

Requires `clang-18`, `lld-18`, `aarch64-linux-gnu-*` cross toolchain, and
kernel headers at `../ftm5-patch/kernel-headers/common/`.

```bash
cd kprobe
make        # cross-compile for ARM64
```

### Deploy

```bash
# Push to device (pipe method for FBE/mount namespace compatibility)
cat kprobe/ftm5_kprobe.ko | adb shell "su -c 'cat > /data/local/tmp/ftm5_kprobe.ko'"

# Load
adb shell "su -c 'insmod /data/local/tmp/ftm5_kprobe.ko'"

# Verify
adb shell "su -c 'dmesg'" | grep ftm5_kprobe
```

### Key Struct Offsets (from ftm5.ko disassembly)

| Field | Offset | Type |
|-------|--------|------|
| `touch_id` | 0x11B0 | `unsigned long` (touch slot bitmask) |
| `palm_touch_mask` | 0x11B8 | `unsigned long` |
| `grip_touch_mask` | 0x11C0 | `unsigned long` |

## Known Limitations

- **fts_filter**: Continuous hold limited to ~4 seconds (12 force-cal cycles).
  Genuine finger lift delayed up to 600ms after long hold (SUPPRESS timeout).
- **ftm5_kprobe**: Same 12-cycle limit (MAX_REARMS) as safety guard against IC
  baseline degradation. Can be tuned higher if IC proves stable.
- `CMD_FORCE_TOUCH_ACTIVE` (`A0 01`) is a bus/power management command, NOT
  an IC scan mode command. It keeps the SPI bus with AP but does not affect
  force-cal or re-detection.

## Key Commands Reference

All via `/proc/fts/driver_test`:

| Command | Effect |
|---------|--------|
| `A0 01` | CMD_FORCE_TOUCH_ACTIVE: keeps SPI bus with AP, holds wakelock |
| `18 03 00` | LOCKED_ACTIVE — prevents LPA, causes force-cal at 308ms |
| `18 00 00` | ACTIVE mode — LPA allowed |
| `81 00` | Disable background baseline adaptation (does NOT fix force-cal) |

## Files

| File | Description |
|------|-------------|
| `fts_filter.c` | EVIOCGRAB + uinput touch event filter (userspace fix) |
| `kprobe/ftm5_kprobe.c` | Kernel kprobe module (in-kernel force-cal suppression) |
| `kprobe/Makefile` | Kprobe module build system (clang-18, ARM64 cross-compile) |
| `kprobe/test_module.c` | Minimal test module for verifying build/load pipeline |
| `fts_daemon.c` | Legacy daemon (no EVIOCGRAB, visible UP+DOWN glitches) |
| `service.sh` | Magisk module boot script (loads kprobe + starts fts_filter) |
| `FINDINGS.md` | Full research notes and driver analysis |

## Device Info

| Item | Value |
|------|-------|
| Device | Pixel 6 (oriole) |
| SoC | Google Tensor GS101 |
| Touch IC | STMicro ftm5 (fts5cu56a) |
| Driver | `ftm5.ko` — `/vendor/lib/modules/ftm5.ko` |
| Kernel | 6.1.145-android14-11 |
| Android | 14 |
