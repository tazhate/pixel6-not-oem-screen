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

# Disable twoshay grippalm (left-edge touch rejection)
resetprop vendor.twoshay.grip_enabled 0

# Disable adaptive touch sensitivity (prevents sensor fusion motion suppression)
resetprop vendor.twoshay.adaptive_touch_sensitivity 0

# FTS LPA keepalive daemon: prevents IC from entering Low Power Active mode
# LPA onset = ~7 frames at 60Hz = ~117ms. Must ping faster than that.
# 0.05s interval = 20 pings/sec — keeps FTS in active scan mode continuously.
# Battery impact: ~2-3mA extra while screen on.
(
  SYSFS="/sys/bus/spi/drivers/fts/spi11.0/stm_fts_cmd"
  while true; do
    sleep 0.05
    echo '13 00' > "$SYSFS" 2>/dev/null
  done
) &
