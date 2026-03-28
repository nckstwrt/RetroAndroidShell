[ -f /sdcard/root_install.tar.gz ] && tar -xzf /sdcard/root_install.tar.gz -C /data/local/tmp && rm -f /sdcard/root_install.tar.gz
chmod +x /data/local/tmp/socat
chmod +x /data/local/tmp/bftpd
chmod +x /data/local/tmp/reset_prop
svc power stayon true
dumpsys deviceidle disable
dumpsys battery set usb 1
PATH=$PATH:/data/local/tmp nohup /data/local/tmp/socat -T 86400 TCP-LISTEN:4444,reuseaddr,fork,keepalive EXEC:'/system/bin/sh -i',pty,stderr,setsid,ctty,echo=0,sigint,sane </dev/null >/dev/null 2>&1 &
nohup /data/local/tmp/bftpd -D -c /data/local/tmp/bftpd.conf &
(sleep 1 && /data/local/tmp/reset_prop ro.secure 0 && setprop service.adb.tcp.port 5555 && start adbd) & stop adbd
