#!/bin/bash

export CCACHE_DIR="$HOME/.ccache"
export USE_CCACHE="1"
export CCACHE_NLEVELS="8"
#env KCONFIG_NOTIMESTAMP=true &>/dev/null

warnandfailearly() {

	echo -n "MX ERROR on Line ${BASH_LINENO[0]}"
	echo "!!!"
	local ISTRING
	ISTRING="$1"
	if [ -n "$ISTRING" ]
	then
		printf "%s\n" "$ISTRING"
	fi
	exit 1

}

[ ! -d "/root/mx_toolchains" ] && warnandfailearly "/root/mx_toolchains folder does not exist!"

RDIR="/root/mn3lite"
ZDIR="/root/mn3-lite-zips"
MXPREFIX="machinexlite-Mark"
MXSUFFIX="-hltetmo"

MXRD="$RDIR/mxrd"
RAMDISKFOLDER="$RDIR/mxramdisk"
ZIPFOLDER="$RDIR/mxzip"
BUILDIR="$RDIR/build"
LOGDIR="/root/mn3litelogging/buildlogs"
KDIR="$BUILDIR/arch/arm/boot"
OLDCFGDIR="/root/mn3litelogging/oldconfigs"
NOREBOOTOPFILE="$RDIR/.noreboot"
OLDVERFILE="$RDIR/.oldversion"
OLDVER="$(cat $OLDVERFILE)"
LASTZIPFILE="$RDIR/.lastzip"
[ ! -f "$LASTZIPFILE" ] && echo -n "${MXPREFIX}1${MXSUFFIX}.zip" > "$LASTZIPFILE"
LASTZIP="$(cat $LASTZIPFILE)"
ENDFILE="$RDIR/.endtime"
STARTFILE="$RDIR/.starttime"

MXCONFIG="$RDIR/arch/arm/configs/mxconfig"
MXNEWCFG="$MXCONFIG.new"
DTCPATH="$BUILDIR/scripts/dtc"
DTIMG="$KDIR/dt.img"
MXDT="$MXRD/split_img/boot.img-dt"
NEWZMG="$KDIR/zImage"
FZMG="$NEWZMG-fixup"
MXZMG="$MXRD/split_img/boot.img-kernel"
DTBTOOL="$RDIR/tools/dtbTool"
MKBOOTIMG="/usr/bin/mkbootimg"
NEWRD="$MXRD/ramdisk-new.cpio.gz"

QUICKHOUR="$(date +%l | cut -d " " -f2)"
QUICKMIN="$(date +%S)"
QUICKAMPM="$(date +%p)"
#QUICKDMY="$(date +%d-%m-%Y)"
QUICKYMD="$(date +%Y-%m-%d)"
QUICKTIME="$QUICKHOUR-$QUICKMIN-$QUICKAMPM"
QUICKDATE="$QUICKYMD-$QUICKTIME"
#CORECOUNT="$(grep processor /proc/cpuinfo | wc -l)"
#TOOLCHAIN="/root/mx_toolchains/arm-cortex_a15-linux-gnueabihf_5.3/bin/arm-cortex_a15-linux-gnueabihf-"
#TOOLCHAIN="/root/mx_toolchains/gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-"
#TOOLCHAIN="/root/mx_toolchains/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabi/bin/arm-linux-gnueabi-"
#TOOLCHAIN="/root/mx_toolchains/gcc-linaro-6.5.0-2018.12-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-"
#TOOLCHAIN="/root/mx_toolchains/gcc-arm-9.2-2019.12-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-"
#TOOLCHAIN="/root/mx_toolchains/gcc-arm-8.2-2019.01-x86_64-arm-linux-gnueabihf/bin/arm-linux-gnueabihf-"
TOOLCHAIN="/root/mx_toolchains/arm-cortex_a15-linux-gnueabihf-linaro_4.9.4-2015.06/bin/arm-cortex_a15-linux-gnueabihf-"
export ARCH="arm"
#export SUBARCH="arm"
export CROSS_COMPILE="$TOOLCHAIN"

if [ "$1" != "-b" ] && [ "$1" != "--bsd" ] && [ "$1" != "-c" ] && [ "$1" != "--clean" ]
then
    if [ "$2" = "reboot" ] || [ "$1" = "-ar" ] || [ "$1" = "--allreboot" ]
    then
        AUTOREBOOT="true"
        echo "AUTOREBOOT option has been selected."
        echo "Script will automatically reboot into recovery!"
    else
        AUTOREBOOT="false"
    fi

    if [ "$2" = "noreboot" ] || [ "$1" = "-anr" ] || [ "$1" = "--allnoreboot" ] || [ -f "$NOREBOOTOPFILE" ]
    then
        NOREBOOT="true"
        echo "NOREBOOT option has been selected."
        echo "Script will not reboot after recovery install!"
    else
        NOREBOOT="false"
    fi
fi

if [ "$1" = "-d" ] || [ "$1" = "--debug" ]
then
    CLEANONFAIL="no"
else
    CLEANONFAIL="yes"
fi

if [ ! -d "$LOGDIR" ]
then
    mkdir "$LOGDIR"
fi

if [ ! -d "$OLDCFGDIR" ]
then
    mkdir -p "$OLDCFGDIR"
fi

stop_build_timer() {

    echo "Stopping MXBuild Timer."
    [ -f "$ENDFILE" ] && rm "$ENDFILE"
    printf "%s" "$(date +%s)" > "$ENDFILE"

}

start_build_timer() {

    echo "Starting MXBuild Timer."
    [ -f "$STARTFILE" ] && rm "$STARTFILE"
    printf "%s" "$(date +%s)" > "$STARTFILE"
}

timerprint() {

	local DIFFMINS
	local DIFFSECS
	local ENDTIME
    local STARTTIME
	local DIFFTIME

    if [ ! -f "$ENDFILE" ]
    then
        echo "Looks like you forgot to start the timer. I will do it for you."
        start_build_timer
    fi

	ENDTIME="$(cat $ENDFILE)"
    STARTTIME="$(cat $STARTFILE)"
	DIFFTIME=$(( ENDTIME - STARTTIME ))

    if [ -z "$DIFFTIME" ]
    then
        printf "%s\n" "MXBUILD Completed!"
    else
    	DIFFMINS=$(bc <<< "(${DIFFTIME}%3600)/60")
    	DIFFSECS=$(bc <<< "${DIFFTIME}%60")
    	printf "%s" "Build completed in: "
    	printf "%d" "$DIFFMINS"
    	if [ "$DIFFMINS" = "1" ]
    	then
    		printf "%s" " Minute and "
    	else
    		printf "%s" " Minutes and "
    	fi
    	printf "%d" "$DIFFSECS"
    	if [ "$DIFFSECS" = "1" ]
    	then
    		printf "%s\n" " Second."
    	else
    		printf "%s\n" " Seconds."
    	fi
    	rm $STARTFILE &> /dev/null
    	rm $ENDFILE &> /dev/null
    	printf "%s\n" "MXBUILD is Finished!"
    fi

}

takeouttrash() {

    rm "$RDIR/localversion" &> /dev/null
	rm "$STARTFILE" &> /dev/null
	rm "$ENDFILE" &> /dev/null
	rm "$RDIR/mxtempusb" &> /dev/null

	find . -type f \( -iname \*.rej \
			-o -iname \*.orig \
			-o -iname \*.bkp \
			-o -iname \*.ko \) \
			| parallel rm -fv {};

}

getmxrecent() {

	local NEEDCOMMIT;
	if [ -f "$BUILDIR/.config" ]
	then
		cp "$BUILDIR/.config" "$MXNEWCFG"
		diff "$MXCONFIG" "$MXNEWCFG"
		if [ "$?" -eq "1" ]
		then
			NEEDCOMMIT="true"
		fi
		[ -f "$MXCONFIG" ] && rm "$MXCONFIG"
		mv "$MXNEWCFG" "$MXCONFIG"
		if [ "$NEEDCOMMIT" = "true" ]
		then
			git add "$MXCONFIG"
			git commit -a -m 'mxconfig updated from build'
            git push
            git status
		fi
	fi
}

clean_build() {

	cd "$RDIR" || warnandfailearly "Failed to cd to $RDIR!"
	getmxrecent
	if [ "$1" = "standalone" ]
	then
		echo -ne "Cleaning build.         \r"; \
		make ARCH="arm" CROSS_COMPILE="$TOOLCHAIN" -j16 clean
		echo -ne "Cleaning build..        \r"; \
		make ARCH="arm" CROSS_COMPILE="$TOOLCHAIN" -j16 distclean
		echo -ne "Cleaning build...       \r"; \
		make ARCH="arm" CROSS_COMPILE="$TOOLCHAIN" -j16 mrproper
	else
		echo -ne "Cleaning build.         \r"; \
		make ARCH="arm" CROSS_COMPILE="$TOOLCHAIN" -j16 clean &>/dev/null
		echo -ne "Cleaning build..        \r"; \
		make ARCH="arm" CROSS_COMPILE="$TOOLCHAIN" -j16 distclean &>/dev/null
		echo -ne "Cleaning build...       \r"; \
		make ARCH="arm" CROSS_COMPILE="$TOOLCHAIN" -j16 mrproper &>/dev/null
	fi
	echo -ne "Cleaning build....      \r"; \
	takeouttrash &>/dev/null
	echo -ne "Cleaning build.....     \r"; \
	rm -rf "$BUILDIR" &>/dev/null
	echo -ne "Cleaning build......    \r"; \
	rm "$ZIPFOLDER/boot.img" &>/dev/null
	echo -ne "Cleaning build.......   \r"; \
    [ -f "$MXRD/image-new.img" ] && rm "$MXRD/image-new.img"
    [ -f "$NEWRD" ] && rm "$NEWRD"
	echo -ne "Cleaning build........  \r"; \
#	rm -rf "$RDIR/scripts/mkqcdtbootimg/mkqcdtbootimg" &>/dev/null
	echo -ne "Cleaning build......... \r"; \
	echo -ne "                        \r"; \
	echo -ne "Cleaned.                \r"; \
	echo -e "\n"

}

warnandfail() {

	echo -n "MX ERROR on Line ${BASH_LINENO[0]}"
	echo "!!!"
	local ISTRING
	ISTRING="$1"
	if [ -n "$ISTRING" ]
	then
		printf "%s\n" "$ISTRING"
	fi
    if [ "$CLEANONFAIL" = "yes" ]
    then
    	clean_build
    fi
	exit 1

}

#_quote() {
#
#	echo $1 | sed 's/[]\/()$*.^|[]/\\&/g'
#
#}

# This function looks for a string, and inserts a specified string after it inside a given file
# $1: the line to locate, $2: the line to insert, $3: Config file where to insert
#pc_insert() {
#
#	local PATTERN;
#	local CONTENT;
#	PATTERN=$(_quote "$1")
#	CONTENT=$(_quote "$2")
#	sed -i "/$PATTERN/a$CONTENT" $3
#
#}

# This function looks for a string, and replace it with a different string inside a given file
# $1: the line to locate, $2: the line to replace with, $3: Config file where to insert
#pc_replace() {
#
#	local PATTERN;
#	local CONTENT;
#	PATTERN=$(_quote "$1")
#	CONTENT=$(_quote "$2")
#	sed -i "s/$PATTERN/$CONTENT/" $3
#
#}

# This function will append a given string at the end of a given file
# $1 The line to append at the end, $2: Config file where to append
#pc_append() {
#
#	echo "$1" >> $2
#
#}

# This function will delete a line containing a given string inside a given file
# $1 The line to locate, $2: Config file where to delete
#pc_delete() {
#
#	local PATTERN;
#	PATTERN=$(_quote "$1")
#	sed -i "/$PATTERN/d" $2
#
#}

if [ "$1" != "-nc" ] && [ "$1" != "--newconfig" ]
then
	if [ ! -f "$MXCONFIG" ]
	then
		warnandfail "$MXCONFIG not found in arm configs!"
	fi
fi

if [ ! -d "$RAMDISKFOLDER" ]
then
	warnandfail "$RAMDISKFOLDER not found!"
fi

shortprog() {

	printf "%s\r" "-----------------------------------"; \
	sleep 0.4; \
	printf "%s\r" "----------------- -----------------"; \
	sleep 0.4; \
	printf "%s\r" "---------------     ---------------"; \
	sleep 0.4; \
	printf "%s\r" "----------               ----------"; \
	sleep 0.4; \
	printf "%s\r" "-------                     -------"; \
	sleep 0.4; \
	printf "%s\r" "-----                         -----"; \
	sleep 0.4; \
	printf "%s\r" "---                             ---"; \
	sleep 0.4; \
	printf "%s\r" "-                                 -"; \
	sleep 0.4; \
	printf "%s\r" "---                             ---"; \
	sleep 0.4; \
	printf "%s\r" "-----                         -----"; \
	sleep 0.4; \
	printf "%s\r" "-------                     -------"; \
	sleep 0.4; \
	printf "%s\r" "----------               ----------"; \
	sleep 0.4; \
	printf "%s\r" "---------------     ---------------"; \
	sleep 0.4; \
	printf "%s\r" "----------------- -----------------"; \
	sleep 0.4; \
	printf "%s\r" "-----------------------------------"; \
	sleep 0.4; \
	printf "%s\r" "                                   "; \
	printf "%s\n"
	#echo -ne "\n"

}

test_funcs() {

	echo "This is a test of the emergency broadcast system."
	echo "This is only a test."
	shortprog
	echo "This has been a test of the emergency broadcast system."
	echo "This was only a test"

}

checkrecov() {
    echo "Ensuring System is ready for operations"
    adb start-server &> /dev/null
    if [ "$(echo $(adb get-state))" = "device" ]
    then
        echo "System is Ready"
        if [ "$AUTOREBOOT" = "true" ]
        then
  			echo "Automatically Rebooting into TWRP Recovery"
            adb shell sync
  			adb reboot recovery
        else
    		echo -n "Reboot into Recovery? [y|n]: "
       		read -r RECREBOOT
       		if [ "$RECREBOOT" = "y" ]
       		then
       			echo "Rebooting into TWRP Recovery"
                adb shell sync
      			adb reboot recovery
       		fi
        fi
    elif [ "$(echo $(adb get-state))" = "recovery" ]
    then
        echo "Device is already in Recovery"
    else
        echo "Device is not connected, nothing to do."
    fi
}

handle_existing() {

	if [ -z "$OLDVER" ]
	then
		warnandfail "FATAL ERROR! Failed to read version from .oldversion"
	fi

	if [ ! -f "$ZDIR/${MXPREFIX}${OLDVER}${MXSUFFIX}.zip" ]
	then
		echo "Version Override!"
		echo "Previous version was not completed!"
		echo "Rebuilding old version"
		MX_KERNEL_VERSION="${MXPREFIX}${OLDVER}${MXSUFFIX}"
	elif [ "$LASTZIP" = "${MXPREFIX}${OLDVER}${MXSUFFIX}.zip" ]
	then
		echo "Version Override"
		echo "Previous version completed successfully!"
		echo "Building new version!"
		NEWVER="$(echo $(( OLDVER + 1 )))"
		if [ -z "$NEWVER" ]
		then
			warnandfail "FATAL ERROR! Failed to raise version number by one!"
		fi
		MX_KERNEL_VERSION="${MXPREFIX}${NEWVER}${MXSUFFIX}"
		echo -n "$NEWVER" > "$OLDVERFILE"
	else
		echo -n "Rebuilding (o)ld version? Or building (n)ew version? Please specify [o|n]: "
		read -r WHICHVERSION
		if [ "$WHICHVERSION" = "n" ]
		then
			CURVER="new"
		elif [ "$WHICHVERSION" = "o" ]
		then
			CURVER="old"
		else
			CURVER="invalid"
		fi
		if [ -z "$CURVER" ]
		then
			warnandfail "You MUST choose a version for the kernel"
		elif [ "$CURVER" = "invalid" ]
		then
			warnandfail "versioning failed.  please fix"
		elif [ "$CURVER" = "old" ]
		then
			echo "Rebulding old version has been selected"
			echo "Removing old zip files..."
			MX_KERNEL_VERSION="${MXPREFIX}${OLDVER}${MXSUFFIX}"
			rm -f "$ZDIR/$MX_KERNEL_VERSION.zip"
		elif [ "$CURVER" = "new" ]
		then
			echo "Building new version has been selected"
			NEWVER="$(echo $(( OLDVER + 1 )))"
			if [ -z "$NEWVER" ]
			then
				warnandfail "FATAL ERROR! Failed to raise version number by one!"
			fi
			MX_KERNEL_VERSION="${MXPREFIX}${NEWVER}${MXSUFFIX}"
			echo -n "$NEWVER" > "$OLDVERFILE"
		fi
	fi
	echo "Kernel version is: $MX_KERNEL_VERSION"
	echo "--------------------------------"
    echo "$MX_KERNEL_VERSION" > "$RDIR/localversion"

}

rebuild() {

	echo "Using last version. Mark$OLDVER will be removed."
	MX_KERNEL_VERSION="${MXPREFIX}${OLDVER}${MXSUFFIX}"
    rm "$RDIR/localversion" &> /dev/null
    echo "$MX_KERNEL_VERSION" > "$RDIR/localversion"
	echo "Removing old zip files..."
	rm -f "$ZDIR/$MX_KERNEL_VERSION.zip"
	echo "Kernel version is: $MX_KERNEL_VERSION"
	echo "--------------------------------"

}

build_new_config() {

	echo "Creating kernel config..."
	cd "$RDIR" || warnandfail "Failed to cd to $RDIR!"
	mkdir -p "$BUILDIR" || warnandfail "Failed to make $BUILDIR directory!"
	cat "$RDIR/arch/arm/configs/msm8974_sec_hlte_tmo_defconfig" "$RDIR/arch/arm/configs/msm8974_sec_defconfig" "$RDIR/arch/arm/configs/selinux_defconfig" > "$RDIR/arch/arm/configs/mxconfig"
	cp "$MXCONFIG" "$BUILDIR/.config" || warnandfail "Config Copy Error!"
	make ARCH="arm" CROSS_COMPILE="$TOOLCHAIN" -C "$RDIR" O="$BUILDIR" menuconfig

}

build_menuconfig() {

	echo "Creating kernel config..."
	cd "$RDIR" || warnandfail "Failed to cd to $RDIR!"
	mkdir -p "$BUILDIR" || warnandfail "Failed to make $BUILDIR directory!"
	cp "$MXCONFIG" "$BUILDIR/.config" || warnandfail "Config Copy Error!"
	make ARCH="arm" CROSS_COMPILE="$TOOLCHAIN" -C "$RDIR" O="$BUILDIR" menuconfig

}

build_single_config() {

	echo "Creating kernel config..."
	cd "$RDIR" || warnandfail "Failed to cd to $RDIR!"
	mkdir -p "$BUILDIR" || warnandfail "Failed to make $BUILDIR directory!"
	cp "$MXCONFIG" "$BUILDIR/.config" || warnandfail "Config Copy Error!"
	make ARCH="arm" CROSS_COMPILE="$TOOLCHAIN" -C "$RDIR" O="$BUILDIR" oldconfig || warnandfail "make oldconfig Failed!"

}

build_kernel_config() {

	echo "Creating kernel config..."
	cd "$RDIR" || warnandfail "Failed to cd to $RDIR!"
	mkdir -p "$BUILDIR" || warnandfail "Failed to make $BUILDIR directory!"
	cp "$MXCONFIG" "$BUILDIR/.config" || warnandfail "Config Copy Error!"
	make ARCH="arm" CROSS_COMPILE="$TOOLCHAIN" -C "$RDIR" O="$BUILDIR" oldconfig || warnandfail "make oldconfig Failed!"
	getmxrecent

}

backupconfig() {

    if [ -d "$BUILDIR" ]
    then
    	echo "Backing up .config to $OLDCFGDIR/config.$QUICKDATE"
    	cp "$BUILDIR/.config" "$OLDCFGDIR/config.$QUICKDATE" || warnandfail "Config Copy Error!"
    fi

}

build_single_driver() {

	echo "Building Single Driver..."
	make -Wa ARCH="arm" CROSS_COMPILE="$TOOLCHAIN" -C "$RDIR" -S -s -j16 O="$BUILDIR" "$1"

}

build_kernel() {

    backupconfig
	start_build_timer
	echo "Starting build..."
	make ARCH="arm" CROSS_COMPILE="$TOOLCHAIN" -S -s -j16 -C "$RDIR" O="$BUILDIR" 2>&1 | tee -a "$LOGDIR/$QUICKDATE.Mark${OLDVER}.log"

}

build_kernel_debug() {

    backupconfig
	start_build_timer
	echo "Starting build..."
	make ARCH="arm" CROSS_COMPILE="$TOOLCHAIN" -S -s -j16 -C "$RDIR" O="$BUILDIR" 2>&1 | tee -a "$LOGDIR/$QUICKDATE.Mark${OLDVER}.log"
    stop_build_timer
    timerprint
}

build_ramdisk() {

	echo "Building ramdisk structure..."
	cd "$RDIR" || warnandfail "Failed to cd to $RDIR"
	rm -rf "$BUILDIR/ramdisk" &>/dev/null
	cp -par "$RAMDISKFOLDER" "$BUILDIR/ramdisk" || warnandfail "Failed to create $BUILDIR/ramdisk!"
	cd "$BUILDIR/ramdisk" || warnandfail "Failed to cd to $BUILDIR/ramdisk!"
	mkdir -pm 755 dev proc sys system
	mkdir -pm 771 data
	[ -f "$KDIR/ramdisk.cpio.gz" ] && rm "$KDIR/ramdisk.cpio.gz"
	echo "Building ramdisk img"
	find | fakeroot "$MXRD/bin/linux/x86_64/cpio" -v -o -H newc | gzip -v -9 > "$KDIR/ramdisk.cpio.gz"
	[ ! -f "$KDIR/ramdisk.cpio.gz" ] && warnandfail "NO ramdisk!"
	cd "$RDIR" || warnandfail "Failed to cd to $RDIR"

}

build_boot_img_qcdt() {

	echo "Generating boot.img..."
	rm -f "$ZIPFOLDER/boot.img"
	if [ ! -f "$RDIR/scripts/mkqcdtbootimg/mkqcdtbootimg" ]
	then
		make -C "$RDIR/scripts/mkqcdtbootimg" || warnandfail "Failed to build mkqcdtbootimg!"
	fi

 	if [ ! -f "$RDIR/scripts/mkqcdtbootimg/mkqcdtbootimg" ]
	then
		warnandfail "mkqcdtbootimg does not exist! Exiting with Error!"
	fi   

	$RDIR/scripts/mkqcdtbootimg/mkqcdtbootimg --kernel "$KDIR/zImage" \
		--ramdisk "$KDIR/ramdisk.cpio.gz" \
        --dt_dir "$KDIR" \
		--cmdline "console=null androidboot.hardware=qcom user_debug=23 msm_rtb.filter=0x37 ehci-hcd.park=3" \
		--base "0x00000000" \
		--pagesize "2048" \
		--ramdisk_offset "0x02000000" \
		--tags_offset "0x01e00000" \
		--output "$ZIPFOLDER/boot.img"
	if [ "$?" -eq 0 ]
	then
		echo "mkqcdtbootimg appears to have succeeded in building an image"
	else
		warnandfail "mkqcdtbootimg appears to have failed in building an image!"
	fi
	[ -f "$ZIPFOLDER/boot.img" ] || warnandfail "$ZIPFOLDER/boot.img does not exist!"
	echo -n "SEANDROIDENFORCE" >> "$ZIPFOLDER/boot.img"

}

build_boot_img() {

    cd "$RDIR" || warnandfail "Failed to cd into $RDIR!"

    if [ ! -f "$NEWZMG" ]
    then
        warnandfail "build_boot_img called with no zImage!"
    fi

	[ -f "$ZIPFOLDER/boot.img" ] && rm "$ZIPFOLDER/boot.img"
    [ -f "$MXRD/image-new.img" ] && rm "$MXRD/image-new.img"
    [ -f "$NEWRD" ] && rm "$NEWRD"

    echo "Regenerating .dtb files"
    rm "$KDIR/msm8974-sec-hlte-r05.dtb" &> /dev/null
    rm "$KDIR/msm8974-sec-hlte-r06.dtb" &> /dev/null
    rm "$KDIR/msm8974-sec-hlte-r07.dtb" &> /dev/null
    rm "$KDIR/msm8974-sec-hlte-r09.dtb" &> /dev/null

    local DTB_FILE
    local DTS_FILE
    for DTS_PREFIX in msm8974-sec-hlte-r05 msm8974-sec-hlte-r06 msm8974-sec-hlte-r07 msm8974-sec-hlte-r09
    do
        DTS_FILE="$RDIR/arch/arm/boot/dts/msm8974/$DTS_PREFIX.dts"
        DTB_FILE="$KDIR/$DTS_PREFIX.dtb"
        echo "Creating $DTB_FILE from $DTS_FILE"
        "$BUILDIR/scripts/dtc/dtc" -i "$RDIR/arch/arm/boot/dts/msm8974" -p 1024 -O dtb -o "$DTB_FILE" "$DTS_FILE" 2>&1 | \
                   tee -a "$LOGDIR/$QUICKDATE.Mark${OLDVER}.log" || warnandfail "Failed to build $DTB_FILE!"
    done

	echo "Generating $DTIMG"

    ./tools/skales/dtbTool -v -o "$DTIMG" -s 2048 -p "$DTCPATH" "$KDIR" 2>&1 | \
                   tee -a "$LOGDIR/$QUICKDATE.Mark${OLDVER}.log" || warnandfail "dtbTool failed to build $DTIMG!"

    if [ ! -f "$DTIMG" ]
    then
		warnandfail "dtbTool failed to build $DTIMG!"
	fi

    echo "Packing up boot.img"

    [ -f "$MXDT" ] && rm "$MXDT"
    cp "$DTIMG" "$MXDT" || warnandfail "Failed to copy $DTIMG to $MXDT!"
    chmod 644 "$MXDT"

#	FIXUP="/root/skales/atag-fix/fixup"
#	${CROSS_COMPILE}gcc -c "$FIXUP.S" -o "$FIXUP.o" && \
#	${CROSS_COMPILE}objcopy -O binary "$FIXUP.o" "$FIXUP.bin" && \
#	cat "$FIXUP.bin" "$NEWZMG" > "$FZMG" || warnandfail "Can't build fixup"

    [ -f "$MXZMG" ] && rm "$MXZMG"
    cp "$NEWZMG" "$MXZMG" || warnandfail "Failed to copy $NEWZMG to $MXZMG!"
    chmod 644 "$MXZMG"

    cd "$MXRD" || warnandfail "Failed to cd into $MXRD!"
    ./repackimg.sh --sudo
    cd "$RDIR" || warnandfail "Failed to cd into $RDIR!"

    [ -f "$NEWRD" ] && rm "$NEWRD"

    if [ -f "$MXRD/image-new.img" ]
    then
        mv "$MXRD/image-new.img" "$ZIPFOLDER/boot.img" || warnandfail "Failed to move $MXRD/image-new.img to $ZIPFOLDER/boot.img!"
    else
        warnandfail "$MXRD/image-new.img nonexistant! Repack must have Failed!"
    fi

	[ -f "$ZIPFOLDER/boot.img" ] || warnandfail "$ZIPFOLDER/boot.img does not exist!"
    chmod 644 "$ZIPFOLDER/boot.img"

}

ADBPUSHLOCATION="/sdcard/Download"
MAGISKFILE="Magisk-v23.0.zip"

create_zip() {

	echo "Compressing to TWRP flashable zip file..."
	cd "$ZIPFOLDER" || warnandfail "Failed to cd to $ZIPFOLDER"
	#[ -d "$ZIPFOLDER/system/lib/modules" ] && rm -rf "$ZIPFOLDER/system/lib/modules"
	#for MXMODS in $(find "$BUILDIR/" -iname '*.ko')
	#do
	#	if [ -f "$MXMODS" ]
	#	then
	#		echo "Copying $MXMODS to zip"
	#		cp -pa "$MXMODS" "$ZIPFOLDER/system/lib/modules/" || warnandfail "Failed to copy new modules to zip!"
	#	fi
	#done
	zip -v -r -9 -y - * > "$ZDIR/$MX_KERNEL_VERSION.zip"
	if [ ! -f "$ZDIR/$MX_KERNEL_VERSION.zip" ]
	then
		warnandfail "$ZDIR/$MX_KERNEL_VERSION.zip does not exist!"
	fi

    cd "$RDIR" || warnandfail "Failed to cd to $RDIR"

	echo "Kernel $MX_KERNEL_VERSION.zip finished"
	echo "Filepath: "
	echo "$ZDIR/$MX_KERNEL_VERSION.zip"
    stop_build_timer

}

pushtodevice() {

	if [ -s "$ZDIR/$MX_KERNEL_VERSION.zip" ]
	then
		echo -n "$MX_KERNEL_VERSION.zip" > "$LASTZIPFILE"
		echo "Starting ADB as root."
		adb root &> /dev/null
		echo "Checking if Device is Connected..."
		if [ "$(echo $(adb get-state))" = "device" ]
		then
			echo "Device is Connected via Usb in System Mode!"
			echo "Ensuring System is ready for operations"
			adb "wait-for-device";
			echo "System is Ready"
			adb shell input keyevent KEYCODE_WAKEUP
			#adb shell input touchscreen swipe 930 880 930 380
			echo "Transferring via adb to $ADBPUSHLOCATION/$MX_KERNEL_VERSION.zip"
			adb push "$ZDIR/$MX_KERNEL_VERSION.zip" "$ADBPUSHLOCATION"
			if [ "$?" -eq "0" ]
			then
				echo "Successfully pushed $ZDIR/$MX_KERNEL_VERSION.zip to $ADBPUSHLOCATION over ADB!"
				#echo "Rebooting Device into Recovery"
				#adb reboot recovery
			else
				echo "Failed to push $ZDIR/$MX_KERNEL_VERSION.zip to $ADBPUSHLOCATION over ADB!"
			fi
		elif [ "$(echo $(adb get-state))" = "recovery" ]
		then
			echo "Device is Connected via Usb in Recovery Mode!"
			#adb shell input keyevent KEYCODE_WAKEUP
			#adb shell input touchscreen swipe 930 880 930 380
			echo "Ensuring Recovery is ready for operations"
			adb "wait-for-recovery";
			echo "Recovery is Ready"
			echo "Transferring installer via adb to $ADBPUSHLOCATION/$MX_KERNEL_VERSION.zip"
			adb push "$ZDIR/$MX_KERNEL_VERSION.zip" "$ADBPUSHLOCATION"
			if [ "$?" -eq "0" ]
			then
				echo "Successfully pushed $ZDIR/$MX_KERNEL_VERSION.zip to $ADBPUSHLOCATION/$MX_KERNEL_VERSION.zip over ADB!"
				echo "Installing $ADBPUSHLOCATION/$MX_KERNEL_VERSION.zip via open recovery script"
				adb shell twrp install "$ADBPUSHLOCATION/$MX_KERNEL_VERSION.zip"
                echo "Pushing Magisk to $ADBPUSHLOCATION/$MAGISKFILE"
                adb push "$RDIR/$MAGISKFILE" "$ADBPUSHLOCATION"
				echo "Installing $ADBPUSHLOCATION/$MAGISKFILE via open recovery script"
				adb shell twrp install "$ADBPUSHLOCATION/$MAGISKFILE"
                echo "Removing leftovers"
                adb shell rm "/data/dalvik-cache/arm/dev@tmp@install@common@magisk.apk@classes.dex" &> /dev/null
                adb shell rm "/data/dalvik-cache/arm/data@app@com.topjohnwu.magisk-1@base.apk@classes.dex" &> /dev/null
                adb shell rm "/data/dalvik-cache/profiles/com.topjohnwu.magisk" &> /dev/null
                if [ "$NOREBOOT" = "false" ]
                then
    				echo "Rebooting Device"
                    adb shell sync
        			adb reboot
                else
                    echo "Skipping Reboot due to noreboot flag!"
                fi
			else
				echo "FAILED to push $ZDIR/$MX_KERNEL_VERSION.zip to $ADBPUSHLOCATION/$MX_KERNEL_VERSION.zip over ADB!"
			fi
		else
			echo "Device not Connected.  Skipping adb transfer."
			#echo "Uploading $MX_KERNEL_VERSION.zip to Google Drive Instead."
			#/bin/bash /root/google-drive-upload/upload.sh "$ZDIR/$MX_KERNEL_VERSION.zip"
			#if [ "$?" -eq "0" ]
			#then
			#	echo "$ZDIR/$MX_KERNEL_VERSION.zip upload SUCCESS!"
			#else
			#	echo "$ZDIR/$MX_KERNEL_VERSION.zip upload FAILED!"
			#fi
		fi
	else
		warnandfail "$ZDIR/$MX_KERNEL_VERSION.zip is 0 bytes, something is wrong!"
	fi
	adb kill-server || echo "Failed to kill ADB server!"
	cd "$RDIR" || warnandfail "Failed to cd to $RDIR"
	timerprint
}

#CREATE_TAR()
#{
#	if [ $MAKE_TAR != 1 ]; then return; fi-d|--debug
#
#	echo "Compressing to Odin flashable tar.md5 file..."
#	cd $RDIR/$ZIPFOLDER
#	tar -H ustar -c boot.img > $RDIR/$MX_KERNEL_VERSION.tar
#	cd $RDIR
#	md5sum -t $MX_KERNEL_VERSION.tar >> $MX_KERNEL_VERSION.tar
#	mv $MX_KERNEL_VERSION.tar $MX_KERNEL_VERSION.tar.md5
#	cd $RDIR
#}

#package_ramdisk_and_zip() {
#
#	build_ramdisk && build_boot_img && create_zip
#
#}

package_ramdisk_and_zip() {

	build_ramdisk && build_boot_img_qcdt && create_zip && pushtodevice

}

package_ramdisk_and_zip_nopush() {

	build_ramdisk && build_boot_img_qcdt && create_zip && timerprint

}


build_kernel_and_package() {

	build_kernel
    if [ -f "$NEWZMG" ]
    then
        package_ramdisk_and_zip
    else
        warnandfail "Kernel Build failed!"
    fi

}

build_kernel_and_package_nopush() {

	build_kernel
    if [ -f "$NEWZMG" ]
    then
        package_ramdisk_and_zip_nopush
    else
        warnandfail "Kernel Build failed!"
    fi

}

build_all() {

	clean_build && build_kernel_config && build_kernel_and_package && clean_build

}

build_all_nopush() {

	clean_build && build_kernel_config && build_kernel_and_package_nopush && clean_build

}

build_debug() {

	clean_build && build_kernel_config
    if [ -f "$NEWZMG" ]
    then
        build_kernel_debug
    else
        warnandfail "Kernel Build failed!"
    fi

}

bsdwrapper() {

	[ -z "$1" ] && warnandfail "Build Single Driver: Missing path/to/folder/ or path/to/file.o"
	clean_build && build_single_config && build_single_driver "$1"
	clean_build

}

runtest() {

test_funcs && exit 0

}

show_help() {

cat << EOF
Machinexlite kernel by robcore
Script written by jcadduono, frequentc & robcore

usage: ./mxbuild.sh [OPTION]
Common options:
 -a|--all            Do a complete build (starting at the beginning)
 -anp|--allnopush    Do a complete build (starting at the beginning), no push to device
 -ar|--allreboot     Do a complete build (starting at the beginning), reboot automatically
 -anr|--allnoreboot  Do a complete build (starting at the beginning), do not reboot
 -d|--debug          Similiar to --all but no img, zip or cleanup. Not for production.
 -r|--rebuildme      Same as --all but defaults to rebuilding previous version
 -b|--bsd            Build single driver (path/to/folder/ | path/to/file.o)
 -c|--clean          Remove everything this build script has done
-nc|--newconfig      Concatecate samsung defconfigs & enter menuconfig
 -m|--menuconfig     Setup an environment for and enter menuconfig
 -k|--kernel         Try the build again starting at compiling the kernel
 -o|--kernel-only    Recompile only the kernel, nothing else
 -t|--tests          Testing playground

Extra command line options are possible, with more to be added in the future:
Currently, it is just the one.
Appending "noreboot" as the second option will keep the device from rebooting.
Or, just use the -anr option that is the same as -a to build all,
but skips the reboot.
Conversely, appending "reboot" or using -ar will instruct the script to reboot
automatically.
EOF

	exit 1

}

if [ $# = 0 ] ; then
	show_help
fi

while [[ $# -gt 0 ]]
do
	extrargs="$2"
	case "$1" in
	     -a|--all)
			checkrecov
			handle_existing
			build_all
			break
	    	;;

	     -anc|--allnoclean)
			checkrecov
			handle_existing
			build_all
			break
	    	;;

	     -anp|--allnopush)
			checkrecov
			handle_existing
			build_all_nopush
			break
	    	;;

	     -anr|--allnoreboot)
			handle_existing
			build_all
			break
	    	;;

	     -ar|--allreboot)
			checkrecov
			handle_existing
			build_all
			break
	    	;;

	     -d|--debug)
			handle_existing
			build_debug
			break
	    	;;

	     -r|--rebuildme)
			checkrecov
            handle_existing
			rebuild
			build_all
			break
	    	;;

	     -b|--bsd)
            handle_existing
			bsdwrapper "$extrargs"
			break
	    	;;

	     -c|--clean)
	    	clean_build "standalone"
	    	break
	    	;;

		 -nc|--newconfig)
            handle_existing
			build_new_config
			break
			;;

		 -m|--menuconfig)
            handle_existing
			build_menuconfig
			break
			;;

	     -k|--kernel)
			checkrecov
			handle_existing
	    	build_kernel_and_package
	    	break
	    	;;

	    -o|--kernel-only)
			checkrecov
			handle_existing
	    	build_kernel
	    	break
	    	;;

	     -t|--tests)
			runtest
	    	break
	    	;;

	    *)
	    	show_help
	    	break;
	    	;;
	esac
	shift # past argument or value
done

