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

# Start fts_filter: EVIOCGRAB + uinput touch event filter
# Grabs real touchscreen, filters force-cal glitches, forwards to uinput.
# Replaces old stm_fts_cmd keepalive and fts_daemon.
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
