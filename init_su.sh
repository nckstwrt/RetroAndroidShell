#!/system/bin/sh

chmod +x sudaemon
chmod +x su

MY_SU=/data/local/tmp/su
DAEMON=/data/local/tmp/sudaemon
WORK=/data/local/tmp/overlay_work
UPPER=/data/local/tmp/overlay_upper

# Start daemon
if ! pidof sudaemon > /dev/null 2>&1; then
    $DAEMON > /dev/null 2>&1 &
fi

# Prepare overlay directories
mkdir -p $WORK $UPPER
chmod 777 $WORK
chmod 777 $UPPER

# Put our su in the upper layer
cp $MY_SU $UPPER/su
chmod 755 $UPPER/su

# Mount overlay: upper layer wins over lower (/system/bin)
# Anything in upper/ shadows the same name in lower/
mount -t overlay overlay -o lowerdir=/system/bin,upperdir=$UPPER,workdir=$WORK /system/bin
