#!/system/bin/sh

[ ! -d /system/etc/init.d ] && mkdir /system/etc/init.d;
chmod -R 755 /system/etc/init.d;
chown -R 0:2000 /system/etc/init.d;
rm -rf /data/magisk_backup*
chmod 755 /data/synapse
chown -R 0:0 /data/synapse
chmod 644 /data/synapse/config.*
chmod -R 755 /data/synapse/actions
if [ -f "/data/synapse/config.json" ]
then
    rm /data/synapse/config.json
fi
