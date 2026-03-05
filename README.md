# Pixel 6 Non-OEM Screen Touch Fix

Fix for touchscreen bugs after replacing the Pixel 6 (oriole) screen with a
non-OEM panel. After replacement, the first touch after idle is missed, long
press and key repeat (hold backspace) are broken.

## Status

| Issue | Status |
|-------|--------|
| Cold tap warmup (first touch after idle missed) | Fixed |
| Long press broken at 308ms | Fixed (kprobe patches event to prevent BTN_UP) |
| Hold key repeat (backspace etc.) | Fixed (kprobe: in-kernel suppression) |
| Fast scrolling "jump back" | Fixed (removed fts_filter EVIOCGRAB interference) |
| Dead zones at bottom rows | Fixed (production autotune) |

**Primary solution: `ftm5_kprobe`** (kernel module) — patches force-cal leave
events at kernel level, no EVIOCGRAB, no scroll interference.

**Legacy fallback: `fts_filter`** (userspace) — EVIOCGRAB + uinput filter.
Causes scroll issues due to over-suppressing real lift events. Only used if
kprobe module cannot be loaded.

## Quick Start

Requires root (Magisk).

### 1. Build the kprobe module

Requires `clang-18`, `lld-18`, `aarch64-linux-gnu-*` cross toolchain, and
kernel headers at `../ftm5-patch/kernel-headers/common/`.

```bash
cd kprobe
make        # cross-compile for ARM64
```

### 2. Deploy

```bash
# Push to device
cat kprobe/ftm5_kprobe.ko | adb shell "su -c 'cat > /data/local/tmp/ftm5_kprobe.ko'"

# Load
adb shell "su -c 'insmod /data/local/tmp/ftm5_kprobe.ko'"

# Verify
adb shell "su -c 'dmesg'" | grep ftm5_kprobe
```

### 3. Apply boot-time baseline fixes (one-time)

```bash
# Sensitivity boost
adb shell "su -c 'echo 1 > /sys/bus/spi/drivers/fts/spi11.0/glove_mode'"

# Recalibrate per-node baseline for non-OEM panel geometry
adb shell "su -c 'echo \"01 00\" > /sys/bus/spi/drivers/fts/spi11.0/stm_fts_cmd'"
sleep 3
adb shell "su -c 'echo \"02 00\" > /sys/bus/spi/drivers/fts/spi11.0/stm_fts_cmd'"
```

For persistence, install as Magisk module with `service.sh`.

## Architecture

```
IC (ftm5) → kernel driver → fts_leave_pointer_event_handler()
                                ↓
                          [kprobe pre_handler]
                          if force-cal pending:
                            event[1] |= 0x08  (poison touchType)
                                ↓
                          handler sees invalid touchType → skips clear_bit
                                ↓
                          touch_id stays non-zero → NO BTN_TOUCH UP
                                ↓
                          /dev/input/event3 → Android (no glitch)
```

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

### IC Event Order (from SPI batch analysis)

```
1. EVT_TYPE_STATUS_FORCE_CAL  (event[1]==0x05)  → status handler
2. EVT_ID_LEAVE_POINT                            → leave handler → clears touch_id
3. (event loop ends, touch_id==0 → BTN_TOUCH UP)
```

### Kprobe Solution (v6 — event patching)

Three kprobes on ftm5 driver functions:

1. **`fts_status_event_handler`** (pre) — detects force-cal (`event[1]==0x05`),
   sets `forcecal_pending` flag with jiffies timestamp
2. **`fts_leave_pointer_event_handler`** (pre) — if force-cal pending, poisons
   `event[1]` by OR-ing `0x08` into the touchType nibble. Handler sees
   `touchType > 7` → skips `__clear_bit(touchId, &info->touch_id)` → no BTN_UP
3. **`fts_enter_pointer_event_handler`** (pre) — resets rearm counter when
   `touch_id==0` (new touch-down after genuine lift)

Key insight from disassembly: the leave handler reads **`event[1]`** (not
`event[0]`). TouchType is in the lower nibble, touchId in the upper nibble.
Valid touchTypes are {0,1,2,4,7} (bitmask `0x97`); values >7 cause skip.

### Why Not fts_filter?

`fts_filter` (userspace EVIOCGRAB approach) **causes scroll issues**: it cannot
distinguish real finger lifts (~294ms swipes) from force-cal phantom lifts
(~308ms). During fast scrolling, it over-suppresses real BTN_UP events, causing
the page to "jump back" when the next swipe starts at a different position.

The kprobe approach is strictly better: it only suppresses leave events that are
actually paired with a preceding force-cal STATUS event.

### Key Parameters

| Parameter | Value | Tunable | Purpose |
|-----------|-------|---------|---------|
| `max_rearms` | 12 | `/sys/module/ftm5_kprobe/parameters/max_rearms` | Max suppress cycles per touch |
| `FORCECAL_MAX_AGE_MS` | 50 | compile-time | Staleness window for force-cal flag |

### Runtime Statistics

Available at `/sys/module/ftm5_kprobe/parameters/`:

| Counter | Description |
|---------|-------------|
| `hits_forcecal` | Force-cal events detected |
| `hits_status` | Total status handler calls |
| `hits_leave` | Total leave handler calls |
| `hits_enter` | Total enter handler calls |
| `total_suppressed` | Leave events successfully suppressed |
| `total_rearms_exhausted` | Times max_rearms limit was hit |
| `total_stale` | Force-cal flags that expired (>50ms) |

### Key Struct Offsets (from ftm5.ko disassembly)

| Field | Offset | Type |
|-------|--------|------|
| `touch_id` | 0x11B0 | `unsigned long` (touch slot bitmask) |
| `palm_touch_mask` | 0x11B8 | `unsigned long` |
| `grip_touch_mask` | 0x11C0 | `unsigned long` |

## Known Limitations

- **max_rearms=12**: Safety guard against IC baseline degradation after ~15
  force-cal cycles. Can be tuned via sysfs if IC proves stable.
- **No in-kernel re-arm**: setScanMode cannot be called from kprobe context
  (CFI violation). IC self-recovers within ~300ms for next cycle.
- `CMD_FORCE_TOUCH_ACTIVE` (`A0 01`) is a bus/power management command, NOT
  an IC scan mode command.

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
| `kprobe/ftm5_kprobe.c` | Kernel kprobe module — primary force-cal suppression |
| `kprobe/Makefile` | Kprobe module build system (clang-18, ARM64 cross-compile) |
| `fts_filter.c` | Legacy EVIOCGRAB + uinput filter (causes scroll issues) |
| `fts_daemon.c` | Legacy daemon (no EVIOCGRAB, visible UP+DOWN glitches) |
| `service.sh` | Magisk module boot script (loads kprobe, falls back to fts_filter) |
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
