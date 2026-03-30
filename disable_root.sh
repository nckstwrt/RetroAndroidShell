setenforce 1
svc power stayon false
dumpsys deviceidle enable
dumpsys battery reset
killall -9 bftpd
killall -9 socat
killall -9 sudaemon
umount -l /system/bin
(sleep 1 && /data/local/tmp/reset_prop ro.secure 1 && start adbd) & stop adbd
