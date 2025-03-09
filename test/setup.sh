#!/bin/bash

###								###
###		  SETUP CONFIG PART		###
###								###

IO_DEPTH=16
BD_NAME="vdb"
BD_SYS_PATH="/sys/block/${BD_NAME}/"
SYSCTL_CONF="/etc/sysctl.conf"
VBD_NAME="lsvbd1"

# Parse options using getopts
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --bd_name)
            BD_NAME="$2"
            shift
            ;;
		--io_depth)
			IO_DEPTH="$2"
			shift 
			;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
	shift
done

# Function to display help
usage() {
    echo "Usage: $0 [--bd_name name_without_/dev/] [--io_depth number]"
    exit 1
}

echo -e "\nBlock device info"
sudo lsblk -o NAME,MAJ:MIN,SIZE,TYPE,FSTYPE,SCHED,ROTA,PHY-SEC,TYPE,MIN-IO,OPT-IO | grep $BD_NAME

echo -e "\nChecking symbols for lsvbd.ko..."
sudo cat /proc/kallsyms | grep lsbdd

echo -e "\nChecking current native block device scheduler && setting it to none"
sudo cat $BD_SYS_PATH/queue/scheduler
sudo echo "none" > $BD_SYS_PATH/queue/scheduler

echo -e "\nLower kernel restrictions"
sudo sysctl kernel.kptr_restrict=0
sudo sysctl kernel.perf_event_paranoid=1

echo -e "\nCheck queue depth"
if [ "$(cat $BD_SYS_PATH/queue/nr_requests)" -le "$IO_DEPTH" ]; then
	echo "$IO_DEPTH" > /sys/block/$BD_NAME/queue/nr_requests
fi

echo -e "\nCheck if CONFIG_BLOCK is enabled"
if [ "$(zgrep CONFIG_BLOCK= /boot/config-$(uname -r))" != "CONFIG_BLOCK=y" ]; then
	echo -e "\nError: Recompile the kernel with CONFIG_BLOCK=y"
	exit -1 
fi

echo -e "\nCheck CPU governors"
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

#echo -e "\nDisble iostats"
#echo 0 > /sys/block/$BD_NAME/queue/iostats 
#echo 0 > /sys/block/$VBD_NAME/queue/iostats 

echo -e "\nDisable merges"
echo 2 > /sys/block/$BD_NAME/queue/nomerges
echo 2 > /sys/block/$VBD_NAME/queue/nomerges

### SYSCTL CFG ###
echo -e "\nApplying sysctl cfg"
echo "vm.dirty_ratio = 5" >> $SYSCTL_CONF
echo "vm.dirty_background_ratio = 2" >> $SYSCTL_CONF
echo "vm.swappiness = 0" >> $SYSCTL_CONF
echo "vm.overcommit_memory = 1" >> $SYSCTL_CONF
echo "fs.aio-max-nr = 1048576" >> $SYSCTL_CONF
echo "fs.file-max = 2097152" >> $SYSCTL_CONF
sysctl -p

echo -e "\nIncreasing file descriptors limits..."
echo "* soft nofile 1048576" >> /etc/security/limits.conf
echo "* hard nofile 1048576" >> /etc/security/limits.conf

