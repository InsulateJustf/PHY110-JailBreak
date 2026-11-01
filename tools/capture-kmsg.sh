#!/system/bin/sh
# root-only: dump the kernel log to disk so it survives a panic/reboot
rm -f /data/local/tmp/kmsg.log
setsid cat /dev/kmsg > /data/local/tmp/kmsg.log 2>&1 </dev/null &
echo KMSG_CAPTURE_STARTED
