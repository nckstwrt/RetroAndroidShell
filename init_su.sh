#!/system/bin/sh
chmod +x sudaemon
chmod +x su

MY_SU=/data/local/tmp/su
DAEMON=/data/local/tmp/sudaemon
OVL_TMP=/data/local/tmp/ovl_tmp
UPPER=$OVL_TMP/upper
WORK=$OVL_TMP/work

# Start daemon
if ! pidof sudaemon > /dev/null 2>&1; then
    $DAEMON > /dev/null 2>&1 &
fi

# Create a tmpfs to host upper and work dirs (overlayfs requires
# upper/work to be on a filesystem it fully supports - tmpfs always works,
# f2fs does not on this kernel)
mkdir -p $OVL_TMP
chmod 777 $OVL_TMP
mount -t tmpfs tmpfs $OVL_TMP -o size=10m

# Create upper and work inside the tmpfs
mkdir -p $UPPER $WORK
chmod 777 $UPPER
chmod 777 $WORK

# Put our su in the upper layer
cp $MY_SU $UPPER/su
chmod 755 $UPPER/su

# Sort nano and chown out too
cp /data/local/tmp/nano $UPPER/nano
cp -r /data/local/tmp/terminfo $UPPER/terminfo
cp /data/local/tmp/chown $UPPER/chown

# Mount overlay: upper layer wins over lower (/system/bin)
# Anything in upper/ shadows the same name in lower/
mount -t overlay overlay \
    -o lowerdir=/system/bin,upperdir=$UPPER,workdir=$WORK \
    /system/bin