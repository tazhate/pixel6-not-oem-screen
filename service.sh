#!/system/bin/sh
sleep 5

# Enable glove mode (hardware sensitivity boost)
chmod 666 /sys/bus/spi/drivers/fts/spi11.0/glove_mode
echo 1 > /sys/bus/spi/drivers/fts/spi11.0/glove_mode

# Production full autotune: recalibrates per-node baseline for non-OEM panel
# 01 00 = MS (mutual sense) production autotune
# 02 00 = SS (self sense) production autotune
chmod 666 /sys/bus/spi/drivers/fts/spi11.0/stm_fts_cmd
echo '01 00' > /sys/bus/spi/drivers/fts/spi11.0/stm_fts_cmd
sleep 3
echo '02 00' > /sys/bus/spi/drivers/fts/spi11.0/stm_fts_cmd
sleep 2

# Disable twoshay grip/palm (left-edge touch rejection)
resetprop vendor.twoshay.grip_enabled 0

# Disable adaptive touch sensitivity (prevents sensor fusion motion suppression)
resetprop vendor.twoshay.adaptive_touch_sensitivity 0

# Kill any old touch daemons
pkill -9 fts_daemon 2>/dev/null
pkill -9 fts_filter 2>/dev/null
sleep 1

# Load kernel kprobe module: in-kernel force-cal suppression
# Intercepts fts_leave/status/enter handlers to prevent spurious BTN_TOUCH UP.
# Works alongside fts_filter (complementary layers).
KPROBE="/data/local/tmp/ftm5_kprobe.ko"
if [ -f "$KPROBE" ]; then
    # Unload old instance if present
    rmmod ftm5_kprobe 2>/dev/null
    insmod "$KPROBE" 2>/dev/null
    if [ $? -eq 0 ]; then
        log -t "pixel6_touch_fix" "ftm5_kprobe loaded"
    else
        log -t "pixel6_touch_fix" "ftm5_kprobe insmod failed, trying force-load"
        insmod -f "$KPROBE" 2>/dev/null && \
            log -t "pixel6_touch_fix" "ftm5_kprobe force-loaded" || \
            log -t "pixel6_touch_fix" "ftm5_kprobe failed to load"
    fi
else
    log -t "pixel6_touch_fix" "ftm5_kprobe.ko not found, skipping"
fi

# Start fts_filter: EVIOCGRAB + uinput touch event filter (userspace fallback)
# Grabs real touchscreen, filters force-cal glitches, forwards to uinput.
# With kprobe loaded, fts_filter provides additional safety net + keepalive.
FILTER="/data/local/tmp/fts_filter"
if [ -x "$FILTER" ]; then
    nohup "$FILTER" > /data/local/tmp/fts_filter.log 2>&1 &
    log -t "pixel6_touch_fix" "fts_filter started (pid=$!)"
else
    log -t "pixel6_touch_fix" "ERROR: $FILTER not found, falling back to keepalive"
    # Fallback: old stm_fts_cmd keepalive
    (
      SYSFS="/sys/bus/spi/drivers/fts/spi11.0/stm_fts_cmd"
      while true; do
        sleep 0.05
        echo '13 00' > "$SYSFS" 2>/dev/null
      done
    ) &
fi
