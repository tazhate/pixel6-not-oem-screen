# Pixel 6 (oriole) — Non-OEM Screen Touch Fix Research

## Problem
After non-OEM screen replacement, touchscreen has "warmup" behavior:
- First touch after a few seconds pause is missed/ignored
- Long press required to "warm up", then short taps work
- After another pause — repeats

## Architecture

```
FTS IC (STMicro ftm5, SPI)
  → ftm5 kernel driver
    → /dev/input/event3  (raw events, getevent reads here)
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

## Root Cause
**FTS IC enters LPA (Low Power Active) mode after ~117ms of no touch** (CX_LPA_GAP=7 frames at ~60Hz).
In LPA, the AOC (Always-On Companion coprocessor, Cortex-M55) owns the SPI bus and scans at low rate.
First touch after idle wakes the IC but is missed.

Key interfaces:
- `/proc/fts/driver_test` — internal driver commands
- `/sys/bus/spi/drivers/fts/spi11.0/stm_fts_cmd` — production test commands
- `/vendor/etc/twoshay_config.json` — twoshay pipeline config
- `/vendor/etc/touchflow.pb` — touchflow algorithm protobuf config

## Key Commands Found

### /proc/fts/driver_test
| Command | Meaning |
|---------|---------|
| `A0 01` | CMD_FORCE_TOUCH_ACTIVE: ON — takes SPI bus from AOC, acquires wakelock |
| `A0 00` | CMD_FORCE_TOUCH_ACTIVE: OFF |
| `18 03 00` | CMD_SETSCANMODE: SCAN_MODE_LOCKED + LOCKED_ACTIVE — forces IC active, prevents LP. **Breaks long press!** |
| `18 00 FF` | CMD_SETSCANMODE: SCAN_MODE_ACTIVE + all features — candidate fix |
| `18 00 00` | CMD_SETSCANMODE: SCAN_MODE_ACTIVE |

### /sys/bus/spi/drivers/fts/spi11.0/stm_fts_cmd
| Command | Meaning |
|---------|---------|
| `01 00` | MS production autotune — recalibrates mutual sense baseline |
| `02 00` | SS production autotune — recalibrates self sense baseline |
| `13 00` | Get mutual sense frame — wakes IC momentarily |

### /sys/bus/spi/drivers/fts/spi11.0/
| Node | Meaning |
|------|---------|
| `glove_mode` = 1 | Hardware sensitivity boost |

## What Was Tried

| Fix | Result |
|-----|--------|
| glove_mode=1 | Slight improvement |
| Production autotune (01 00, 02 00) | Fixed dead zones at bottom rows |
| Keepalive '13 00' every 1.5s | No effect on warmup |
| Keepalive '13 00' every 50ms | No effect on warmup |
| resetprop vendor.twoshay.grip_enabled 0 | Minor improvement |
| resetprop vendor.twoshay.adaptive_touch_sensitivity 0 | Minor improvement |
| A0 01 (FTS_BUS_REF_FORCE_ACTIVE: ON) | Confirmed working in dmesg, but warmup remains |
| Modify touchflow.pb byte 23 (field 2) from 0x0a→0x01 | No effect (wrong field or not the cause) |
| Remove TouchflowAlgorithm from pipeline | Warmup persists |
| Remove GripSuppression + TouchflowAlgorithm | Warmup persists |
| **A0 01 + 18 03 00 (LOCKED_ACTIVE)** | **Cold short taps WORK! But long press broken** |
| 18 00 FF (SCAN_MODE_ACTIVE) | Testing... |

## Current Working State (as of session)
- Magisk module: `/data/adb/modules/pixel6_touch_fix/service.sh`
- Applied at boot: glove_mode=1, production autotune, grip_enabled=0, adaptive_touch_sensitivity=0
- Keepalive: `echo '13 00' > stm_fts_cmd` every 50ms (insufficient but running)
- Manual (not persistent): `A0 01` + `18 03 00` via /proc/fts/driver_test

## What Actually Fixed Warmup
`echo '18 03 00' > /proc/fts/driver_test` after `echo 'A0 01' > /proc/fts/driver_test`

The `18 03 00` (CMD_SETSCANMODE with SCAN_MODE_LOCKED=0x03, LOCKED_ACTIVE=0x00) forces FTS IC
into permanently active scanning mode, preventing LPA transitions.

**Side effect**: Long press detection broken (probably baseline drift issue in locked mode).

## Candidate Full Fix
Try `18 00 FF` (SCAN_MODE_ACTIVE with all features) — may keep IC active without locked mode side effects.
If that works: add `18 00 FF` + `A0 01` to Magisk module service.sh and remove keepalive loop.

## Files
- `service.sh` — current Magisk module boot script
- `touchflow_original.pb` — original /vendor/etc/touchflow.pb (147 bytes)
- `twoshay_config_no_touchflow.json` — twoshay pipeline without TouchflowAlgorithm
- `twoshay_config_bare.json` — minimal pipeline (Segmentation+Reporting only)
