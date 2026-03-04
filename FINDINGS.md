# Pixel 6 (oriole) — Non-OEM Screen Touch Fix Research

## Problem

After non-OEM screen replacement, touchscreen has "warmup" behavior:
- First touch after a few seconds pause is missed/ignored
- Long press required to "warm up", then short taps work
- After another pause — repeats
- Long press (text selection etc.) broken in some modes

## Architecture

```
FTS IC (STMicro ftm5, SPI)
  → ftm5 kernel driver (/vendor/lib/modules/ftm5.ko)
    → /dev/input/event3  (raw events)
    → /dev/touch_offload (heatmap frames)
      → twoshay daemon (/vendor/bin/twoshay -s)
        ├── SegmentationAlgorithm
        ├── GripSuppressionAlgorithm
        ├── TouchflowAlgorithm  (uses /vendor/etc/touchflow.pb)
        ├── ReportingAlgorithm
        └── TouchSuezAlgorithm
          → back to FTS driver → /dev/input/event3 (filtered)
            → Android InputReader → Apps
```

**Key fact from getevent analysis**: raw `/dev/input/event3` shows BTN_TOUCH DOWN
*immediately* on touch — the warmup delay is NOT in the driver or twoshay.
It is entirely at the IC hardware/firmware level (LPA mode).

## Root Cause

**FTS IC enters LPA (Low Power Active) mode after ~117ms of no touch.**
`CX_LPA_GAP = 7 frames at ~60Hz ≈ 117ms`

In LPA, the AOC (Always-On Companion coprocessor, Cortex-M55) owns the SPI
bus and scans at low rate. First touch after idle wakes the IC but the event
is already missed by the time AP regains the bus.

`A0 01` (CMD_FORCE_TOUCH_ACTIVE) forces AP to hold the SPI bus but does NOT
prevent the IC firmware from entering LPA internally — warmup persists.

## Key Interfaces

| Interface | Purpose |
|-----------|---------|
| `/proc/fts/driver_test` | Internal driver command interface (raw IC commands) |
| `/sys/bus/spi/drivers/fts/spi11.0/stm_fts_cmd` | Production test commands |
| `/sys/bus/spi/drivers/fts/spi11.0/glove_mode` | Hardware sensitivity boost |
| `/vendor/etc/twoshay_config.json` | twoshay pipeline config |
| `/vendor/etc/touchflow.pb` | Touchflow algorithm protobuf config |

## Commands

### /proc/fts/driver_test

| Command | Meaning |
|---------|---------|
| `A0 01` | CMD_FORCE_TOUCH_ACTIVE: ON — triggers goog_pm_resume → fts_system_reset + fts_mode_handler + tbn_request_bus (AP takes SPI bus, acquires wakelock). Does NOT prevent IC LPA internally. |
| `A0 00` | CMD_FORCE_TOUCH_ACTIVE: OFF |
| `18 03 00` | CMD_SETSCANMODE: SCAN_MODE_LOCKED + LOCKED_ACTIVE — forces IC permanently active, **prevents LPA**. Side effect: long press broken. |
| `18 03 01` | LOCKED_HOVER — breaks all touch |
| `18 03 02` | LOCKED_IDLE — breaks all touch |
| `18 03 10` | LOCKED_LP_DETECT — breaks all touch (self-sense only) |
| `18 03 11` | LOCKED_LP_ACTIVE — breaks all touch |
| `18 00 00` | SCAN_MODE_ACTIVE — normal mode, allows LPA transition |
| `18 00 FF` | SCAN_MODE_ACTIVE + all features — allows LPA, warmup returns |
| `81 00` | CMD_BASELINE_ADAPTATION: OFF — writes ADDR_CONFIG_AUTOCAL=0. Does NOT fix long press (controls slow background autocal, not force-cal) |

### /sys/bus/spi/drivers/fts/spi11.0/stm_fts_cmd

| Command | Meaning |
|---------|---------|
| `01 00` | MS production autotune — recalibrates mutual sense baseline (fixes dead zones) |
| `02 00` | SS production autotune — recalibrates self sense baseline |
| `13 00` | Get mutual sense frame — wakes IC momentarily (insufficient to prevent LPA) |

## What Was Tried

| Fix | Result |
|-----|--------|
| glove_mode=1 | Slight sensitivity improvement |
| Production autotune (01 00, 02 00) | Fixed dead zones at bottom rows |
| Keepalive `13 00` every 1.5s via stm_fts_cmd | No effect on warmup |
| Keepalive `13 00` every 50ms via stm_fts_cmd | No effect on warmup |
| `resetprop vendor.twoshay.grip_enabled 0` | Minor improvement |
| `resetprop vendor.twoshay.adaptive_touch_sensitivity 0` | Minor improvement |
| `A0 01` alone | Confirmed in dmesg, warmup remains |
| Modify touchflow.pb byte 23 (field 2) 0x0a→0x01 | No effect |
| Remove TouchflowAlgorithm from twoshay pipeline | Warmup persists |
| Remove GripSuppression + TouchflowAlgorithm (bare pipeline) | Warmup persists |
| `18 00 FF` (SCAN_MODE_ACTIVE all features) | Warmup returns |
| **`A0 01` + `18 03 00` (LOCKED_ACTIVE)** | **Cold short taps WORK** — IC never enters LPA |
| `18 03 00` + `81 00` (baseline adapt off) | Long press still broken |
| Periodic `18 03 00` every 200ms keepalive | Prevents LPA ✓, does NOT reset 308ms recal timer |
| Daemon: switch to ACTIVE on BTN_DOWN | Stuck touch (IC loses tracking on mode switch mid-touch) |
| Daemon: switch to ACTIVE 100ms after BTN_DOWN | Stuck touch (same issue regardless of delay) |
| `18 03 02` (LOCKED_IDLE) | All touch broken |
| `18 03 10`, `18 03 11` (LP modes) | All touch broken |
| **Daemon: re-arm `18 03 00` on every BTN_UP** | **First long press worked! Inconsistent after.** |

## Long Press Root Cause (Detailed)

In LOCKED_ACTIVE mode, FTS IC firmware detects "mutual frame flatness":
when a sustained touch is held long enough, the mutual capacitance frame
becomes "flat" relative to the baseline (finger absorbed into baseline).

This triggers `EVT_TYPE_STATUS_FORCE_CAL` (sub-code 0x35 = Mutual frame
flatness Force cal) at approximately **308ms** after touch start.

Driver event chain:
1. IC sends `EVT_ID_LEAVE_POINT` → `fts_leave_pointer_event_handler()` → clears `touch_id` bit
2. IC sends `EVT_TYPE_STATUS_FORCE_CAL` → `fts_status_event_handler()` → logs only, does nothing
3. After event loop: `touch_id == 0` → `input_report_key(BTN_TOUCH, 0)` → spurious UP

**Key**: `CMD_BASELINE_ADAPTATION (0x81)` only controls slow background autocal
(`ADDR_CONFIG_AUTOCAL = 0x0040`). The frame-flatness force-cal is a separate
firmware mechanism and cannot be disabled with 0x81.

## Current Best Fix: fts_daemon

See `fts_daemon.c`. Two-part approach:

**Part 1 — Anti-LPA keepalive:**
Send `18 03 00` every 200ms while no touch is active.
200ms ≪ 117ms LPA threshold → IC stays awake → cold taps always register.
Keepalive pauses during active touch to avoid disturbing IC state.

**Part 2 — Long press re-arm:**
On every BTN_TOUCH UP, immediately send `18 03 00`.
If the finger is still on screen (spurious UP from force-cal at 308ms),
the IC re-detects it and sends a new `EVT_ID_ENTER_POINT` → `BTN_TOUCH DOWN`.
App sees: `DOWN → UP(308ms) → DOWN → UP(real)` — long press fires at ~800ms.

**Known limitation**: Long press requires ~800ms hold instead of the normal
~500ms, due to the spurious UP + re-detection cycle. Inconsistency observed
on subsequent presses — needs further testing.

## Kernel Module Approach (Next Step)

The cleanest fix is a kernel-space kprobe module:
- Hook `fts_status_event_handler` at `EVT_TYPE_STATUS_FORCE_CAL`
- If `info->touch_id != 0`: call `setScanMode(LOCKED_ACTIVE)` to re-arm IC
- This happens before LEAVE_POINT is processed → can potentially prevent tracking loss

Prerequisites:
- `CONFIG_KPROBES=y` ✓ (confirmed on running kernel)
- `CONFIG_KALLSYMS_ALL=y` ✓ — all ftm5 symbols visible in `/proc/kallsyms`
- `ftm5.ko` at `/vendor/lib/modules/ftm5.ko` (loads with relaxed vermagic on GKI)
- Need: android14-6.1 kernel headers + android14 clang toolchain to build

Key symbols in `/proc/kallsyms`:
- `fts_leave_pointer_event_handler [ftm5]`
- `fts_status_event_handler [ftm5]`
- `setScanMode [ftm5]`

## ftm5 Driver Facts

| Item | Value |
|------|-------|
| Module | `/vendor/lib/modules/ftm5.ko` |
| Kernel | `6.1.145-android14-11` |
| scmversion | `g9e5b4f7d8dba` |
| Source | `android.googlesource.com/kernel/google-modules/touch/fts_touch` |
| Key structs | `fts_ts_info.touch_id` (unsigned long bitmask, offset ~855 in struct) |
| FORCE_CAL handler | `fts.c:3377` — logs only, returns without action |
| Leave handler | `fts.c:3247` — clears `touch_id` bit, triggers BTN_UP |
| setScanMode | `ftsCore.c:339` — sends `{0xFA,0x20,0,0,0,0,0}` preamble + `{0xA0, mode, settings}` |

## Magisk Module

Current module at `/data/adb/modules/pixel6_touch_fix/service.sh`:
- Runs at boot: glove_mode=1, production autotune (01 00 + 02 00), grip_enabled=0, adaptive_touch_sensitivity=0
- Old stm_fts_cmd keepalive (13 00 every 50ms) — superseded by fts_daemon
- TODO: replace old keepalive with fts_daemon for persistent fix

## Files

| File | Description |
|------|-------------|
| `service.sh` | Current Magisk module boot script |
| `fts_daemon.c` | Userspace daemon — anti-LPA keepalive + long press re-arm |
| `touchflow_original.pb` | Original `/vendor/etc/touchflow.pb` (147 bytes) |
| `twoshay_config_no_touchflow.json` | twoshay pipeline without TouchflowAlgorithm |
| `twoshay_config_bare.json` | Minimal pipeline (Segmentation+Reporting only) |
