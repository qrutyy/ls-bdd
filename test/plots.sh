#!/bin/bash

JOBS_NUM=4
IO_DEPTH=16
RUNS=4
BRD_SIZE=2

LOGS_PATH="logs"
PLOTS_PATH="./plots"
RESULTS_FILE="logs/fio_results.dat"
LAT_RESULTS_FILE="logs/fio_lat_results.dat"

HISTOGRAM_PLOTS_SCRIPT="fio_distr_plots.py"
AVG_PLOTS_SCRIPT="avg_plots.py"
LATENCY_PLOTS_SCRIPT="lat_plots.py"

WBS_LIST=("4K" "8K")
RBS_LIST=("4K" "8K" "16K")

LATENCY_WRBS_LIST=("8K" "4K") ## SNIA recommends 0.5K also, need some convertion
RW_MIXES=("100-0" "65-35" "0-100") ## Write to read ops ratio

# Function to prioritize all the fio processes (including forks in case of numjobs > 1)
# UPD: mb no need in it
prioritise_fio() {
    echo -e "\nPrioritizing fio process..."
    for pid in $(pidof fio); do
        echo -e "\nchrt $pid"
        chrt -r 99 "$pid"
    done
}

usage() {
    echo "Usage: $0 [--io_depth number] [--jobs_num number]"
    exit 1
}

extract_all_metrics() {
    local log_file=$1
    local run_id=$2
    local wbs=$3
	local rbs=$4
    local mode=$5
	
	if [[ "$rbs" == "0" ]]; then	
	    local bw=$(grep -oP 'WRITE: bw=[0-9]+MiB/s \(\K[0-9]+' "$log_file" | head -1)
		if [[ -z "$bw" ]]; then
			bw_gb="$(grep -oP 'WRITE: bw=.*\(([0-9]+\.[0-9]+)GB/s\)' "$log_file" | grep -oP '[0-9]+\.[0-9]+' | tail -n1)"
			echo $bw_gb
			bw=$(echo "$bw_gb * 1000" | bc)
			echo $bw
		fi
	else
		local bw=$(grep -oP 'READ: bw=[0-9]+MiB/s \(\K[0-9]+' "$log_file" | head -1)
		if [[ -z "$bw" ]]; then
			bw_gb="$(grep -oP 'READ: bw=.*\(([0-9]+\.[0-9]+)GB/s\)' "$log_file" | grep -oP '[0-9]+\.[0-9]+' | tail -n1)"
			echo $bw_gb
			bw=$(echo "$bw_gb * 1000" | bc)
			echo $bw
		fi
	fi

	# Extract IOPS from the main log file and remove 'k' if present
    local iops=$(grep -oP 'IOPS=\K[0-9]+(\.[0-9]+)?k?' "$log_file" | sed 's/k//g' | awk '{s+=$1} END {print s}')
	echo "DEBUG: Extracted IOPS='$iops' BW='$bw'"

	echo "$run_id $wbs $rbs $bw $iops 0 0 0 $mode" >> "$RESULTS_FILE"
}

extract_latency_metrics() {
    local run_id=$1
    local log_file=$2
    local bs=$3
    local rw_mix=$4

    calc_avg_latency() {
        local file=$1
        local result=$(awk -F',' '{sum+=$2; count++} END {if(count>0) print sum/count; else print 0}' "$file")
        echo "$result"
    }

    calc_max_latency() {
        local file=$1
        local result=$(awk -F',' 'BEGIN {max=0} {if($2>max) max=$2} END {print max}' "$file")
        echo "$result"
    }

    calc_95p_latency() {
        local file=$1
        local result=$(awk -F',' '{print $2}' "$file" | sort -n | awk 'NR > 0 { all[NR] = $1 } END { if (NR > 0) print all[int(NR*0.95)] }')
        echo "$result"
    }

	local slat_file="${log_file}_slat.1.log"
	local clat_file="${log_file}_clat.1.log"
	local lat_file="${log_file}_lat.1.log"

    local avg_slat=$(calc_avg_latency "$slat_file")
    local avg_clat=$(calc_avg_latency "$clat_file")
    local avg_lat=$(calc_avg_latency "$lat_file")

    local max_slat=$(calc_max_latency "$slat_file")
    local max_clat=$(calc_max_latency "$clat_file")
    local max_lat=$(calc_max_latency "$lat_file")

    local p95_slat=$(calc_95p_latency "$slat_file")
    local p95_clat=$(calc_95p_latency "$clat_file")
    local p95_lat=$(calc_95p_latency "$lat_file")

    echo "$run_id $bs $avg_slat $avg_clat $avg_lat $max_slat $max_clat $max_lat $p95_slat $p95_clat $p95_lat $rw_mix" >> "$LAT_RESULTS_FILE"
}

run_latency_test() {
    local bs=$1
    local rw_mix=$2
    local log_file="$LOGS_PATH/latency_${bs}_${rw_mix}"
    
    echo "Running latency test: Block Size=$bs, RW Mix=$rw_mix..."
	rw_mix_read=$(echo "$rw_mix" | cut -d'-' -f1)
	rw_mix_write=$(echo "$rw_mix" | cut -d'-' -f2)
	
	fio --name=latency_test --rw=randrw --rwmixread=${rw_mix_read} --rwmixwrite=${rw_mix_write} --bs=${bs} --numjobs=1 --iodepth=1 --time_based --runtime=3 --direct=1 --write_lat_log=$log_file --ioengine=io_uring --filename=/dev/lsvbd1
}

workload_independent_preconditioning() {
	local wbs=$1
    echo "Running workload independent pre-conditioning..."
    fio --name=prep --rw=write --bs=${wbs}K --numjobs=1 --iodepth=1 --size=2G --filename=/dev/lsvbd1 --direct=1 --output="$LOGS_PATH/preconditioning.log"
}

# add data-structure
reinit_lsvbd() {
	make -C ../src exit DBI=1 > /dev/null

	echo -e "\nPage cache and Dentry flushing"
	sync; echo 3 | sudo tee /proc/sys/vm/drop_caches 

	modprobe brd rd_nr=1 rd_size=$((BRD_SIZE * 1048576))
	
	make -C ../src init_no_recompile DS=sl TY=lf  > /dev/null
	
}

# Parse options using getopts
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --io_depth)
            IO_DEPTH="$2"
            shift 
            ;;
        --jobs_num)
            JOBS_NUM="$2"
            shift 
            ;;
		--brd_size)
			BRD_SIZE="$2"
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

echo -e "\nCleaning the logs directory"
make clean > /dev/null

mkdir -p $LOGS_PATH $PLOTS_PATH/histograms $PLOTS_PATH/histograms/write $PLOTS_PATH/histograms/read $PLOTS_PATH/avg $PLOTS_PATH/avg/write $PLOTS_PATH/avg/read

### DISTRIBUTION + SNIA BENCHMARK ###

## WRITE TESTS ##
'
echo -e "\nRunning write tests\n"

for bs in "${WBS_LIST[@]}"; do 

	workload_independent_preconditioning "128"
	echo -e "Before work\n"
	free -m 
	for i in $(seq 1 $RUNS); do
		echo "Run $i of $RUNS..."

		LOG_FILE="$LOGS_PATH/fio_w_run_${i}.log"
		# LOG_FILE is used for simpler iops and bw results parsing
		# Latency is parsed from fio generated log files

		make fio_perf_w_opt ID=$IO_DEPTH NJ=$JOBS_NUM IN=$i > "$LOG_FILE"
		extract_all_metrics "$LOG_FILE" "$i" "$bs" "0" "write"
	done
	echo -e "\n after work"
	free -m
	reinit_lsvbd
done

echo "Data collected in $RESULTS_FILE"

python3 "$AVG_PLOTS_SCRIPT"
python3 "$HISTOGRAM_PLOTS_SCRIPT" 
make clean_logs > /dev/null

## READ TESTS ##

for wbs in "${WBS_LIST[@]}"; do
	for rbs in "${RBS_LIST[@]}"; do
		echo -e "\n\nRunning read test for wbs=$wbs rbs=$rbs..."
		
		workload_independent_preconditioning "$wbs"

		for i in $(seq 1 $RUNS); do


			echo "Run $i of $RUNS..."

			LOG_FILE="$LOGS_PATH/fio_r_run_${i}.log"

			make fio_perf_r_opt RBS=$rbs ID=$IO_DEPTH NJ=$JOBS_NUM IN=$i > "$LOG_FILE"
			extract_all_metrics "$LOG_FILE" "$i" "$wbs" "$rbs" "read"
		done
	
		reinit_lsvbd
	done
done 

echo "Data collected in $RESULTS_FILE"

python3 "$AVG_PLOTS_SCRIPT"
python3 "$HISTOGRAM_PLOTS_SCRIPT" 
make clean_logs > /dev/null
'
### LATENCY SNIA BENCHMARK ### TOFIX

echo "Starting SNIA Latency Benchmark..."

for rw_mix in "${RW_MIXES[@]}"; do
    for bs in "${LATENCY_WRBS_LIST[@]}"; do

		echo -e "\nPerfofm a block device warm up"
		workload_independent_preconditioning "$bs"
		
		for i in $(seq 1 $RUNS); do
			echo "Run $i of $RUNS..."
			run_latency_test "$bs" "$rw_mix"
			extract_latency_metrics "$i" "$LOGS_PATH/latency_${bs}_${rw_mix}" "$bs" "$rw_mix"
		done
		
		reinit_lsvbd
	done
done

python3 "$LATENCY_PLOTS_SCRIPT"
#make clean_logs > /dev/null

echo "Histograms, AVG plots and statistics saved in $PLOTS_PATH"
