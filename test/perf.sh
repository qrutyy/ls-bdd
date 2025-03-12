#!/bin/bash

###											###
###		  PERFORMANCE ANALYSIS PART			###
###											###

FLAMEGRAPH_PATH="./FlameGraph"
VERIFY="false"
JOBS_NUM=1
IO_DEPTH=16

# Function to display help
usage() {
    echo "Usage: $0 [--bd_name name_without_/dev/] [--verify true/false] [--io_depth number]"
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

# Parse options using getopts
while [[ "$#" -gt 0 ]]; do
    case $1 in
        -v|--verify)
            VERIFY="true"
            ;;
		--io_depth)
			IO_DEPTH="$2"
			shift 
			;;
		--jobs_num)
			JOBS_NUM="$2"
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

echo -e "\nAdding probes for lsvbd.ko functions..."
sudo perf probe -x ../src/lsbdd.ko --add '*(*)' || echo "Warning: Some probes may already exist."

if [ "$VERIFY" == "true" ]; then
	echo -e "\nStarting fio read&write verify workload ..."
	# While veryfying - read & write cannot be run independently, due to verify information loss
	sudo perf record -g -F 99 -a -o perf_wr.data -- make fio_perf_wr_opt ID=$IO_DEPTH JOBS_NUM=4 
	prioritise_fio
else
	echo -e "\nStarting fio write workload..."
	sudo perf record -g -F 99 -a -o perf_write.data -- make fio_perf_w_opt ID=$IO_DEPTH JOBS_NUM=4 
	prioritise_fio

	echo -e "\nStarting fio read workload..."
	sudo perf record -g -F 99 -a -o perf_read.data -- make fio_perf_r_opt ID=$IO_DEPTH JOBS_NUM=4 
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

if [ "$VERIFY" == "true" ]; then
	perf script -f -i perf_wr.data | $FLAMEGRAPH_PATH/stackcollapse-perf.pl | grep lsbdd > out_wr.folded
	sudo $FLAMEGRAPH_PATH/flamegraph.pl --width 2500 --height 16 out_wr.folded > lsbdd_fg_spec_wr.svg
else 
	perf script -f -i perf_write.data | $FLAMEGRAPH_PATH/stackcollapse-perf.pl | grep lsbdd > out_write.folded
	sudo $FLAMEGRAPH_PATH/flamegraph.pl --width 2500 --height 16 out_write.folded > lsbdd_fg_spec_write.svg

	perf script -f -i perf_read.data | $FLAMEGRAPH_PATH/stackcollapse-perf.pl | grep lsbdd > out_read.folded
	sudo $FLAMEGRAPH_PATH/flamegraph.pl --width 2500 --height 16 out_read.folded > lsbdd_fg_spec_read.svg
fi

if [ ! -f lsbdd_fg_spec_read.svg ] || [ ! -f lsbdd_fg_spec_write.svg ]; then
 echo "Error: local FlameGraph generation failed."
exit 1
fi

echo -e "\nFlameGraph successfully generated: lsbdd_fg_spec_read.svg and lsbdd_fg_spec_write.svg"
