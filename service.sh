#!/system/bin/sh
# touch_filter watchdog service
# Author: DevCat3

LOG="/data/local/tmp/touch_filter.log"

# Wait for boot
while [ "$(getprop sys.boot_completed)" != "1" ]; do sleep 2; done
sleep 8

# Write default config if not exists
[ -f /data/local/tmp/touch_filter.conf ] || cat > /data/local/tmp/touch_filter.conf << 'CONF'
MAX_JUMP=300
MAX_PENDING_FRAMES=8
CONF

# Watchdog loop — restarts filter if it dies for any reason
(
    while true; do
        pkill -f touch_filter 2>/dev/null
        sleep 1
        /system/bin/touch_filter
        echo "[$(date '+%H:%M:%S')] touch_filter exited (code $?), restarting in 3s..." >> "$LOG"
        sleep 3
    done
) &
