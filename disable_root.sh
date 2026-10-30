svc power stayon false
dumpsys deviceidle enable
dumpsys battery reset
killall -9 bftpd
killall -9 socat
killall -9 sudaemon
umount -l /system/bin
umount -l /data/local/tmp/ovl_tmp
# Restore secure mode and restart adbd. Detached + retried so that a failure
# in reset_prop can never leave the device with adbd down (and no adb access).
nohup sh -c '
stop adbd
i=0
while [ "$(getprop init.svc.adbd)" = "running" ] && [ $i -lt 10 ]; do
sleep 1
i=$((i+1))
done
/data/local/tmp/reset_prop ro.secure 1
i=0
while [ $i -lt 10 ]; do
start adbd 2>/dev/null
setprop ctl.start adbd 2>/dev/null
sleep 1
[ "$(getprop init.svc.adbd)" = "running" ] && break
pidof adbd >/dev/null 2>&1 && break
i=$((i+1))
done
' </dev/null >/dev/null 2>&1 &
