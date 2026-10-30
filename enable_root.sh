setenforce 0
for SRC in /sdcard/root_install.tar.gz /sdcard/Download/root_install.tar.gz /storage/emulated/0/root_install.tar.gz /storage/emulated/0/Download/root_install.tar.gz; do
	if [ -f "$SRC" ]; then
		tar -xzf "$SRC" -C /data/local/tmp # && rm -f "$SRC"
		break
	fi
done
chmod +x /data/local/tmp/socat
chmod +x /data/local/tmp/bftpd
chmod +x /data/local/tmp/reset_prop
chmod +x /data/local/tmp/nano
chmod +x /data/local/tmp/chown
chmod +x /data/local/tmp/sudaemon
chmod +x /data/local/tmp/su
chmod +x /data/local/tmp/init_su.sh
svc power stayon true
dumpsys deviceidle disable
dumpsys battery set usb 1
PATH=$PATH:/data/local/tmp nohup /data/local/tmp/socat -T 86400 TCP-LISTEN:4444,reuseaddr,fork,keepalive EXEC:'/system/bin/sh -i',pty,stderr,setsid,ctty,echo=0,sigint,sane </dev/null >/dev/null 2>&1 &
nohup /data/local/tmp/bftpd -D -c /data/local/tmp/bftpd.conf &
# Set up root infrastructure first: sudaemon + /system/bin overlay with su
/system/bin/sh -c "/data/local/tmp/init_su.sh"
# Now that su is available, force properties for root + wireless adb
su -c "/data/local/tmp/reset_prop ro.secure 0"
setprop service.adb.root 1
setprop service.adb.tcp.port 5555
# Restart adbd to pick up the new properties.
# Detached so it survives adbd going down.
nohup sh -c '
stop adbd
i=0
while [ "$(getprop init.svc.adbd)" = "running" ] && [ $i -lt 10 ]; do
	sleep 1
	i=$((i+1))
done
i=0
while [ $i -lt 10 ]; do
	start adbd 2>/dev/null
	sleep 1
	[ "$(getprop init.svc.adbd)" = "running" ] && break
	pidof adbd >/dev/null 2>&1 && break
	i=$((i+1))
done
' </dev/null >/dev/null 2>&1 &
