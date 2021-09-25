#!/system/bin/sh
# Copyright (c) 2009-2012, The Linux Foundation. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#     * Neither the name of The Linux Foundation nor
#       the names of its contributors may be used to endorse or promote
#       products derived from this software without specific prior written
#       permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
# WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
# OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
# ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# Removed Generalized BULLSHIT and kept our device specific props

#export PATH=${PATH}:/sbin:/system/bin:/system/xbin
#chmod 755 /sys
chmod 644 /sys/fs/selinux/enforce
echo 0 > /sys/fs/selinux/enforce
echo "[MACHIN3X] mx.sh Started" | tee /dev/kmsg

if [ -f "/root/sqlite3" ]
then
	chown 0:0 "/root/sqlite3"
	chmod 755 "/root/sqlite3"
elif [ -f "/sbin/sqlite3" ]
then
	chown 0:0 "/sbin/sqlite3"
	chmod 755 "/sbin/sqlite3"
fi
if [ -f "/root/zip" ]
then
	chown 0:0 "/root/zip"
	chmod 755 "/root/zip"
elif [ -f "/sbin/zip" ]
then
	chown 0:0 "/sbin/zip"
	chmod 755 "/sbin/zip"
fi

#if [ -f "/sbin/resetprop" ] || [ -L "/sbin/resetprop" ]
#then
#    rm "/sbin/resetprop"
#    ln -s "/sbin/magisk" "/sbin/resetprop"
#else
#    ln -s "/sbin/magisk" "/sbin/resetprop"
#fi
#
#if [ -f "/sbin/magiskpolicy" ] || [ -L "/sbin/magiskpolicy" ]
#then
#    rm "/sbin/magiskpolicy"
#    ln -s "/sbin/magiskinit" "/sbin/magiskpolicy"
#else
#    ln -s "/sbin/magiskinit" "/sbin/magiskpolicy"
#fi
#
#if [ -f "/sbin/su" ] || [ -L "/sbin/su" ]
#then
#    rm "/sbin/su"
#    ln -s "/sbin/magisk" "/sbin/su"
#else
#    ln -s "/sbin/magisk" "/sbin/su"
#fi
#
#if [ -f "/sbin/supolicy" ] || [ -L "/sbin/supolicy" ]
#then
#    rm "/sbin/supolicy"
#    ln -s "/sbin/magiskinit" "/sbin/supolicy"
#else
#    ln -s "/sbin/magiskinit" "/sbin/supolicy"
#fi

#if [ -f "/sbin/magiskhide" ] || [ -L "/sbin/magiskhide" ]
#then
#    rm "/sbin/magiskhide"
#    ln -s "/sbin/magisk" "/sbin/magiskhide"
#else
#    ln -s "/sbin/magisk" "/sbin/magiskhide"
#fi

#magiskpolicy --live "permissive audio_data_file audio_prop default_android_service init default_prop platform_app property_socket system_app system_data_file system_file system_prop system_server tmpfs untrusted_app s_untrusted_app"
#magiskpolicy --live "allow s_untrusted_app default_prop property_service {set}"
#magiskpolicy --live "allow s_untrusted_app * property_service {set}"

# Init.d
chmod -R 755 "/system/etc/init.d"
chown -R 0:2000 "/system/etc/init.d"
#chmod 755 "/sys"

# Set correct r/w permissions for LMK parameters
chmod 664 "/sys/module/lowmemorykiller/parameters/cost"
chmod 664 "/sys/module/lowmemorykiller/parameters/adj"
chmod 664 "/sys/module/lowmemorykiller/parameters/minfree"

if [ ! -d "/data/synapse" ]
then
	mkdir "/data/synapse"
fi

if [ ! -d "/data/synapse/actions" ]
then
	mkdir "/data/synapse/actions"
fi

chmod 755 /data/synapse
chown -R 0:0 /data/synapse
chmod 644 /data/synapse/config.json*
chmod -R 755 /data/synapse/actions

if [ ! -d "/data/synapse/stemp" ]
then
    mkdir "/data/synapse/stemp"
fi

chmod 755 /data/synapse/stemp
chown -R 0:0 /data/synapse/stemp
chmod 644 /data/synapse/stemp/*

if [ -f "/data/synapse/config.json" ]
then
    rm "/data/synapse/config.json"
fi

chmod 755 "/sbin/powond"
chown 0:2000 "/sbin/powond"
chmod 755 "/sbin/recond"
chown 0:2000 "/sbin/recond"
chmod 755 "/sbin/sleepcond"
chown 0:2000 "/sbin/sleepcond"

rm /data/synapse/stemp/sleeplate.lock &> /dev/null
rm /data/synapse/stemp/powerofflate.lock &> /dev/null
rm /data/synapse/stemp/rebootlate.lock &> /dev/null

echo "0" > /data/synapse/stemp/sleepclock
echo "0" > /data/synapse/stemp/rebootclock
echo "0" > /data/synapse/stemp/poweroffclock
chmod 644 "/sys/block/mmcblk0/queue/scheduler"
chmod 644 "/sys/block/mmcblk0rpmb/queue/scheduler"
chmod 644 "/sys/block/mmcblk1/queue/scheduler"

for MYBLOCK in mmcblk0 mmcblk0rpmb mmcblk1
do
    echo 0 > "/sys/block/$MYBLOCK/queue/add_random"
if [ -f "/sys/block/$MYBLOCK/queue/iosched/slice_idle" ]
then
    echo 0 > "/sys/block/$MYBLOCK/queue/iosched/slice_idle"
fi
done

echo 0 > /sys/devices/platform/kcal_ctrl.0/kcal_enable
#echo 0 > /sys/devices/virtual/graphics/fb0/csc_cfg
#chown 0:0 /sys/devices/virtual/graphics/fb0/csc_cfg
#chmod 400 /sys/devices/virtual/graphics/fb0/csc_cfg
#echo cfq > /sys/block/mmcblk0/queue/scheduler
#echo cfq > /sys/block/mmcblk1/queue/scheduler

#echo 0 > /proc/sys/net/ipv4/tcp_slow_start_after_idle

#echo 1 > /proc/sys/net/ipv4/tcp_no_metrics_save
#echo 'f' > /sys/class/net/wlan0/queues/tx-0/xps_cpus
#echo 0 > /sys/class/net/wlan0/queues/tx-0/tx_timeout
#echo 'f' > /sys/class/net/wlan0/queues/rx-0/rps_cpus
#echo 7516192768 > /sys/class/net/wlan0/queues/tx-0/byte_queue_limits/limit_max
#echo 0 > /sys/class/net/wlan0/queues/tx-0/byte_queue_limits/hold_time

#SYSUIPID="$(pidof com.android.systemui)"
#if [ -z "$(echo $SYSUIPID)" ]
#then
#    echo "[MACHIN3X] SystemUI not running so not killing" | tee /dev/kmsg
#else
#    if kill -9 "$SYSUIPID"
#    then
#        echo "[MACHIN3X] SystemUI Killed Early" | tee /dev/kmsg
#    else
#        echo "[MACHIN3X] Error: SystemUI Not Killed" | tee /dev/kmsg
#    fi
#fi

#echo 0 > /sys/devices/virtual/lcd/panel/temperature

echo '60' > /proc/sys/vm/dirty_writeback_centisecs
echo '90' > /proc/sys/vm/dirty_expire_centisecs

echo "[MACHIN3X] mx.sh Complete" | tee /dev/kmsg
echo "[MACHIN3X] Running Init.d" | tee /dev/kmsg

/system/xbin/busybox run-parts /system/etc/init.d &
exit 0

