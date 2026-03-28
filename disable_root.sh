svc power stayon false
dumpsys deviceidle enable
killall -9 bftpd
killall -9 socat
(sleep 1 && /data/local/tmp/reset_prop ro.secure 1 && start adbd) & stop adbd
