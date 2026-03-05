#!/system/bin/sh
sleep 5

MODDIR="${0%/*}"
TAG="pixel6_touch_fix"

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

# Load kernel kprobe module: in-kernel force-cal suppression (primary fix)
# Patches event[1] touchType in fts_leave_pointer_event_handler to prevent
# spurious BTN_TOUCH UP during force-cal. Unlike fts_filter (userspace),
# this works at kernel level — no EVIOCGRAB, no scroll interference.
#
# Search order: Magisk module dir → /data/local/tmp
KPROBE=""
for p in "$MODDIR/ftm5_kprobe.ko" "/data/local/tmp/ftm5_kprobe.ko"; do
    [ -f "$p" ] && KPROBE="$p" && break
done

if [ -n "$KPROBE" ]; then
    # Unload old instance if present
    rmmod ftm5_kprobe 2>/dev/null

    KVER=$(uname -r)
    log -t "$TAG" "kernel: $KVER, loading: $KPROBE"

    insmod "$KPROBE" 2>/dev/null
    if [ $? -eq 0 ]; then
        log -t "$TAG" "ftm5_kprobe loaded"
    else
        log -t "$TAG" "ftm5_kprobe insmod failed, trying force-load"
        insmod -f "$KPROBE" 2>/dev/null && \
            log -t "$TAG" "ftm5_kprobe force-loaded" || \
            log -t "$TAG" "ftm5_kprobe FAILED (kernel=$KVER), falling back to fts_filter"
        # Fallback to fts_filter only if kprobe fails
        KPROBE=""
    fi
else
    log -t "$TAG" "ftm5_kprobe.ko not found"
fi

# fts_filter: userspace fallback (only if kprobe unavailable/failed)
# NOTE: fts_filter uses EVIOCGRAB which interferes with fast scrolling.
# Only use as fallback when kprobe module cannot be loaded.
if [ -z "$KPROBE" ]; then
    FILTER=""
    for p in "$MODDIR/fts_filter" "/data/local/tmp/fts_filter"; do
        [ -x "$p" ] && FILTER="$p" && break
    done

    if [ -n "$FILTER" ]; then
        nohup "$FILTER" > /data/local/tmp/fts_filter.log 2>&1 &
        log -t "$TAG" "fts_filter started as fallback (pid=$!)"
    else
        log -t "$TAG" "ERROR: no fix available (no kprobe, no fts_filter)"
    fi
fi
