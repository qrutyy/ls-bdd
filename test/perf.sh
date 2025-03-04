#!/bin/bash

BD_NAME="$1"
FLAMEGRAPH_PATH="./FlameGraph"
BLOCK_DEVICE_PATH="/sys/block/${BD_NAME:-vdb}/queue/scheduler"

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

echo -e "\nChecking symbols for lsvbd.ko..."
sudo cat /proc/kallsyms | grep lsbdd

echo -e "\nChecking current native block device scheduler && setting it to 'none'"
sudo cat $BLOCK_DEVICE_PATH
sudo echo "none" > $BLOCK_DEVICE_PATH

echo -e "\nLower kernel restrictions"
sudo sysctl kernel.kptr_restrict=0
sudo sysctl kernel.perf_event_paranoid=1

echo -e "\nAdding probes for lsvbd.ko functions..."
sudo perf probe -x ../src/lsbdd.ko --add '*(*)' || echo "Warning: Some probes may already exist."

echo -e "\nStarting fio write workload..."
sudo perf record -g -F 99 -a -o perf_write.data -- make fio_perf_w ID=16 NJ=4

echo -e "\nStarting fio read workload..."
sudo perf record -g -F 99 -a -o perf_read.data -- make fio_perf_r ID=16 NJ=4

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
