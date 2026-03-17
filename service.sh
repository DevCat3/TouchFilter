#!/system/bin/sh
# Wait for boot
while [ "$(getprop sys.boot_completed)" != "1" ]; do sleep 2; done
sleep 5

# Kill old instance
pkill -f touch_filter 2>/dev/null
sleep 1

# Start filter daemon in background
nohup /system/bin/touch_filter >> /data/local/tmp/touch_filter.log 2>&1 &
echo $! > /data/local/tmp/touch_filter.pid
