#!/bin/bash

VBD_NAME="lsvbd1"
BD_NAME="vdb"
VERIFY="false"
FLAMEGRAPH_PATH="./FlameGraph"
BLOCK_DEVICE_PATH="/sys/block/${BD_NAME}/queue/scheduler"
IO_DEPTH=16
SYSCTL_CONF="/etc/sysctl.conf"

# Function to display help
usage() {
    echo "Usage: $0 [--bd_name name_without_/dev/] [--verify true/false]"
    exit 1
}

# Function to prioritise all the fio processes (including forks in case of numjobs > 1)
prioritise_fio()
{
	echo -e "\nPrioritise fio process..."
	for pid in $(pidof fio); do
		echo -e "\nchrt $pid"
		chrt -r 99 "$pid"
	done
}

# Function to validate VERIFY variable
validate_verify_input() {
    if [[ "$VERIFY" != "true" && "$VERIFY" != "false" ]]; then
        echo "ERROR: VERIFY must be either 'true' or 'false'."
        usage
    fi
}

# Parse options using getopts
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --bd_name)
            BD_NAME="$2"
            shift 2
            ;;
        --verify)
            VERIFY="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
done

validate_verify_input

echo "Block device name: $BD_NAME"
echo "Verify option: $VERIFY"

if [ "$(id -u)" -ne 0 ]; then
    echo "This script should be run from a root."
    exit 1
fi

if [ ! command -v perf &> /dev/null ]; then
    echo "Error: 'perf' is not installed. Please install it and try again."
    exit 1
fi

if [[ ! -f "$FLAMEGRAPH_PATH/stackcollapse-perf.pl" || ! -f "$FLAMEGRAPH_PATH/flamegraph.pl" ]]; then
    echo "Error: FlameGraph scripts not found."
	echo "Installing FlameGraph into $FLAMEGRAPH_PATH ."
	git clone https://github.com/brendangregg/FlameGraph.git
    
	# Check if git clone was successful
    if [[ $? -ne 0 ]]; then
        echo "Error: Failed to clone FlameGraph repository."
        exit 1
    fi
    echo "FlameGraph successfully cloned."
fi

###								###
###			CONFIG PART			###
###								###

echo -e "\nBlock device info"
sudo lsblk -o NAME,MAJ:MIN,SIZE,TYPE,FSTYPE,SCHED,ROTA,PHY-SEC,TYPE,MIN-IO,OPT-IO | grep $BD_NAME

echo -e "\nChecking symbols for lsvbd.ko..."
sudo cat /proc/kallsyms | grep lsbdd

echo -e "\nChecking current native block device scheduler && setting it to 'none'"
sudo cat $BLOCK_DEVICE_PATH
sudo echo "none" > $BLOCK_DEVICE_PATH

echo -e "\nLower kernel restrictions"
sudo sysctl kernel.kptr_restrict=0
sudo sysctl kernel.perf_event_paranoid=1

echo -e "\nCheck queue depth"
if [ "$(cat /sys/block/$BD_NAME/queue/nr_requests)" -le "$IO_DEPTH" ]; then
	echo "$IO_DEPTH" > /sys/block/$BD_NAME/queue/nr_requests
fi

echo -e "\nCheck if CONFIG_BLOCK is enabled"
if [ "$(zgrep CONFIG_BLOCK= /boot/config-$(uname -r))" != "CONFIG_BLOCK=y" ]; then
	echo -e "\nError: Recompile the kernel with CONFIG_BLOCK=y"
	exit -1 
fi

echo -e "\nCheck CPU governors"
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
:' 
echo -e "\nDisble iostats"
echo 0 > /sys/block/$BD_NAME/queue/iostats 
echo 0 > /sys/block/$VBD_NAME/queue/iostats 
'
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

###								  ###
###		PERFORMANCE TEST PART	  ###
###								  ###

echo -e "\nAdding probes for lsvbd.ko functions..."
sudo perf probe -x ../src/lsbdd.ko --add '*(*)' || echo "Warning: Some probes may already exist."

if [ "$VERIFY" == "true" ]; then
	echo -e "\nStarting fio read&write verify workload ..."
	# While veryfying - read & write cannot be run independently, due to verify information loss
	sudo perf record -g -F 99 -a -o perf_read.data -- make fio_perf_wr_opt ID=$IO_DEPTH NJ=4 
	prioritise_fio
else
	echo -e "\nStarting fio write workload..."
	sudo perf record -g -F 99 -a -o perf_write.data -- make fio_perf_w_opt ID=$IO_DEPTH NJ=4 
	prioritise_fio

	echo -e "\nStarting fio read workload..."
	sudo perf record -g -F 99 -a -o perf_read.data -- make fio_perf_r_opt ID=$IO_DEPTH NJ=4 
	prioritise_fio
fi

if [ ! -s perf_write.data ] || [ ! -s perf_read.data ]; then
    echo "Error: perf results are empty. Profiling may have failed."
    exit 1
fi

#echo "Filtering traces for lsvbd.ko..."
#sudo perf script | grep lsbdd > lsbdd-trace-spec.txt
#sudo perf script > lsbdd-trace.txt

echo -e "\nGenerating FlameGraph... (lsbdd only)"

perf script -f -i perf_write.data | $FLAMEGRAPH_PATH/stackcollapse-perf.pl | grep lsbdd > out_write.folded
sudo $FLAMEGRAPH_PATH/flamegraph.pl --width 2500 --height 16 out_write.folded > lsbdd_fg_spec_write.svg

perf script -f -i perf_read.data | $FLAMEGRAPH_PATH/stackcollapse-perf.pl | grep lsbdd > out_read.folded
sudo $FLAMEGRAPH_PATH/flamegraph.pl --width 2500 --height 16 out_read.folded > lsbdd_fg_spec_read.svg


if [ ! -f lsbdd_fg_spec_read.svg ] || [ ! -f lsbdd_fg_spec_write.svg ]; then
 echo "Error: local FlameGraph generation failed."
exit 1
fi

#echo "Generating global FlameGraph..."
#sudo perf script -f | ./FlameGraph/stackcollapse-perf.pl > out.folded
#sudo ./FlameGraph/flamegraph.pl out.folded > lsbdd_fg.svg

#if [ ! -f flamegraph.svg ]; then
 #echo "Error: FlameGraph generation failed."
#exit 1
#fi

echo -e "\nFlameGraph successfully generated: lsbdd_fg_spec_read.svg and lsbdd_fg_spec_write.svg"
