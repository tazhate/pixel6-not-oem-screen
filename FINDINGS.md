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
| **EVIOCGRAB + uinput filter (fts_filter)** | **Best fix: suppresses force-cal glitches, ~4s continuous hold** |

## EVIOCGRAB + uinput Filter (fts_filter) — Current Best Fix

### Approach

Exclusively grab `/dev/input/event3` via EVIOCGRAB, filter force-cal glitches
in userspace, forward clean events to a virtual uinput device.

### Key Discoveries During Development

**1. Re-arm timing is critical:**
The `18 03 00` command must be sent IMMEDIATELY on the `BTN_TOUCH 0` event,
before `SYN_REPORT`. Waiting for the full frame to complete misses the IC's
re-detection window.

**2. Keepalive interferes with re-detection:**
The keepalive thread (200ms ping) MUST be paused during the SUPPRESS window.
If keepalive sends `18 03 00` during the ~356ms re-detection period, it resets
the IC before it can re-detect the finger. Fix: keep `touching=1` during
SUPPRESS so keepalive stays paused.

**3. IC re-detection timing is variable:**
Measured via `grab_test.c` (EVIOCGRAB + re-arm, no uinput):
- First re-detection after force-cal: ~282-356ms
- Subsequent re-detections: 5-146ms (usually 34-95ms)
- Outliers: up to 915ms (!) — 500ms timeout was too short

**4. IC baseline degradation limits hold duration:**
Each force-cal re-arm cycle absorbs more of the finger's signal into the IC
baseline. After ~12-15 cycles (~4 seconds of continuous hold):
- Reported pressure drops from 60-80 to 16 (near detection threshold)
- Re-detection slows from 50-200ms to 400-900ms
- Eventually re-detection fails completely
- IC may enter a bad state requiring seconds to recover

**5. `A0 01` is NOT an IC command:**
`CMD_FORCE_TOUCH_ACTIVE` in the procfs handler maps to `fts_set_bus_ref()`:
```c
case CMD_FORCE_TOUCH_ACTIVE:
    fts_set_bus_ref(info, FTS_BUS_REF_FORCE_ACTIVE, cmd[1]);
    __pm_stay_awake(info->wakesrc);
```
It keeps the SPI bus with the AP (prevents SLPI takeover) and holds a wakelock.
It does NOT send any command to the IC and does NOT affect force-cal or
re-detection timing.

**6. `setScanMode(LOCKED_ACTIVE)` sends `A0 03 00` over SPI:**
```c
// ftsCore.c:339
u8 cmd1[7] = {0xFA, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00};  // preamble
u8 cmd[3]  = {0xA0, 0x03, 0x00};  // FTS_CMD_SCAN_MODE + LOCKED + ACTIVE
```

**7. Force-cal is entirely firmware-autonomous:**
The driver has zero suppression logic. `fts_status_event_handler()` for
`EVT_TYPE_STATUS_FORCE_CAL` is purely logging — no state is saved, no action
taken. The LEAVE_POINT (and resulting BTN_TOUCH UP) is processed unconditionally.

**8. Force-cal trigger types (from ftsSoftware.h):**
| Code | Trigger |
|------|---------|
| 0x01 | Sense on force cal (sense off→on transition) |
| 0x02 | Host command force cal |
| 0x10-0x11 | Mutual frame drop / pure raw |
| 0x20-0x23 | Self detect/touch negative/flatness |
| 0x30-0x35 | Invalid/flatness calibration (0x35 = mutual frame flatness) |

**9. Startup race condition:**
When fts_filter starts immediately after killing a previous instance,
`/proc/fts/driver_test` may still be held open. Both `A0 01` and `18 03 00`
fail with EBUSY. Fix: retry loop with 200ms backoff.

**10. twoshay does NOT use event3:**
twoshay reads from `/dev/touch_offload` (heatmap frames), not
`/dev/input/event3`. EVIOCGRAB on event3 does not affect twoshay.

### Parameters Tuned

| Parameter | Final Value | Reason |
|-----------|-------------|--------|
| SUPPRESS_TIMEOUT_MS | 600 | Catches re-detections up to 600ms (grab_test showed up to 915ms but 600 is a good balance) |
| MIN_HOLD_MS | 280 | Force-cal happens at ~308ms. Taps <280ms are real lifts, bypass suppression. Prevents typing delays. |
| MAX_REARMS | 12 | After 12 re-arm cycles, IC baseline is too degraded. Accept UP to prevent hang. |
| KEEPALIVE_US | 200000 | 200ms << 117ms LPA threshold. Keeps IC active between touches. |

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

## Current Best Fix: fts_filter

See `fts_filter.c`. EVIOCGRAB + uinput approach (described above).

## Legacy Fix: fts_daemon

See `fts_daemon.c`. Simple re-arm approach (no EVIOCGRAB, glitches visible to Android):

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

## Kernel Kprobe Module (ftm5_kprobe) — Implemented

In-kernel force-cal suppression via three kprobes. Prevents spurious BTN_TOUCH UP
at the driver level — Android never sees the lift event.

### Architecture

```
IC event batch (single SPI read, same context):
  LEAVE_POINT → fts_leave_pointer_event_handler()  ← Kprobe 1: save touch_id
                  ↓ handler clears touch_id
  FORCE_CAL   → fts_status_event_handler()          ← Kprobe 2: restore touch_id
                  ↓ touch_id != 0 (restored)
  After loop: touch_id != 0 → NO BTN_TOUCH UP       ← Android never sees lift
```

### Three Kprobes

1. **`fts_leave_pointer_event_handler`** (pre+post handler):
   - pre: save `touch_id`, `palm_touch_mask`, `grip_touch_mask` from `fts_ts_info`
   - post: verify touch_id was actually cleared (cancel if not — e.g. invalid touchType)

2. **`fts_status_event_handler`** (pre handler):
   - Check `event[1] == 0x05` (force-cal status event)
   - If `touch_id == 0` and `saved_touch_id != 0` and `rearm_count < MAX_REARMS`:
     restore all three bitmasks, schedule `setScanMode(LOCKED_ACTIVE)` via workqueue

3. **`fts_enter_pointer_event_handler`** (pre handler):
   - Reset `rearm_count` to 0 on new touch-down (when `touch_id` was 0)

### Struct Offsets (verified from ftm5.ko disassembly)

| Field | Offset | Type |
|-------|--------|------|
| `touch_id` | 0x11B0 | `unsigned long` (touch slot bitmask) |
| `palm_touch_mask` | 0x11B8 | `unsigned long` |
| `grip_touch_mask` | 0x11C0 | `unsigned long` |

### setScanMode Resolution

Kernel modules can't import symbols from other modules directly. Resolved
`setScanMode` address at init time using the kprobe registration trick:

```c
struct kprobe kp_tmp = { .symbol_name = "setScanMode" };
register_kprobe(&kp_tmp);
fn_setScanMode = (void *)kp_tmp.addr;
unregister_kprobe(&kp_tmp);
```

Workqueue handler calls `fn_setScanMode(saved_info, 0x03, 0x00)` — kprobe
handlers run with preemption disabled so SPI I/O must be deferred to process
context.

### Build Challenges

**1. MODVERSIONS CRC extraction:**
The running kernel enforces MODVERSIONS — each imported symbol must carry the
correct CRC in the module's `__versions` section. CRCs were obtained from:
- `ftm5.ko` `__versions` section (7 symbols: module_layout, _printk, memset, etc.)
- `bcmdhd4389.ko` `__versions` (system_wq)
- Kernel binary `kcrctab` section (register_kprobe, unregister_kprobe)

The kernel's `kcrctab` stores **absolute CRC values** (not PREL32 relative),
despite `CONFIG_HAVE_ARCH_PREL32_RELOCATIONS=y`. Verified by matching known
CRCs from module `__versions` sections.

**2. Struct module size mismatch (0x400 vs 0x440):**
Initial builds crashed at `mod_sysfs_setup+0x208` because `.gnu.linkonce.this_module`
section was 1024 bytes instead of the expected 1088 bytes. The 64-byte difference
was caused by `CONFIG_DEBUG_INFO_BTF_MODULES=y` being missing — it adds 16 bytes
(`btf_data_size` + `btf_data` pointer) to `struct module`, which rounds up to
+64 bytes due to `____cacheline_aligned` (64-byte boundary).

Fix: manually inject `CONFIG_DEBUG_INFO_BTF_MODULES=y` into the kernel headers'
`autoconf.h` and `auto.conf`.

**3. Toolchain:**
`CONFIG_CFI_CLANG=y` requires clang (not gcc). Build uses:
- `clang-18` as compiler
- `ld.lld-18` as linker
- Full LLVM-18 toolchain (ar, nm, strip, objcopy, objdump)

**4. File transfer (FBE/mount namespace):**
Files pushed via `adb push` are not visible in Magisk su context due to
File-Based Encryption mount namespaces. Fix: pipe transfer:
```bash
cat module.ko | adb shell "su -c 'cat > /data/local/tmp/module.ko'"
```

### Module.symvers (all CRCs)

```
0xea759d7f  module_layout     vmlinux  EXPORT_SYMBOL
0x92997ed8  _printk           vmlinux  EXPORT_SYMBOL
0xdcb764ad  memset            vmlinux  EXPORT_SYMBOL
0x4829a47e  memcpy            vmlinux  EXPORT_SYMBOL
0xc2c193d2  __stack_chk_fail  vmlinux  EXPORT_SYMBOL
0xd969d6f4  cancel_work_sync  vmlinux  EXPORT_SYMBOL_GPL
0x732ac580  queue_work_on     vmlinux  EXPORT_SYMBOL
0x56470118  __warn_printk     vmlinux  EXPORT_SYMBOL
0x2d3385d3  system_wq         vmlinux  EXPORT_SYMBOL
0x0472cf3b  register_kprobe   vmlinux  EXPORT_SYMBOL_GPL
0xeb78b1ed  unregister_kprobe vmlinux  EXPORT_SYMBOL_GPL
```

### Deployment

```bash
# Build
cd kprobe && make

# Deploy
cat ftm5_kprobe.ko | adb shell "su -c 'cat > /data/local/tmp/ftm5_kprobe.ko'"
adb shell "su -c 'insmod /data/local/tmp/ftm5_kprobe.ko'"
adb shell "su -c 'dmesg'" | grep ftm5_kprobe

# Verify: all 3 kprobes registered
# ftm5_kprobe: kp_leave registered at <addr>
# ftm5_kprobe: kp_status registered at <addr>
# ftm5_kprobe: kp_enter registered at <addr>
```

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
- Old stm_fts_cmd keepalive (13 00 every 50ms) — superseded by fts_filter
- TODO: replace old keepalive with fts_filter for persistent fix

## Files

| File | Description |
|------|-------------|
| `kprobe/ftm5_kprobe.c` | Kernel kprobe module — in-kernel force-cal suppression |
| `kprobe/Makefile` | Cross-compile build (clang-18, ARM64) |
| `kprobe/test_module.c` | Minimal test module for verifying build/load pipeline |
| `fts_filter.c` | EVIOCGRAB + uinput touch event filter (userspace fix) |
| `fts_daemon.c` | Legacy daemon — anti-LPA keepalive + long press re-arm |
| `service.sh` | Magisk module boot script (loads kprobe + starts fts_filter) |
| `touchflow_original.pb` | Original `/vendor/etc/touchflow.pb` (147 bytes) |
| `twoshay_config_no_touchflow.json` | twoshay pipeline without TouchflowAlgorithm |
| `twoshay_config_bare.json` | Minimal pipeline (Segmentation+Reporting only) |
