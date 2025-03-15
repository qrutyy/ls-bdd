#!/bin/bash

JOBS_NUM=4
IO_DEPTH=16
RUNS=5

LOGS_PATH="logs"
PLOTS_PATH="./plots"
RESULTS_FILE="logs/fio_results.dat"
HISTOGRAM_PLOTS_SCRIPT="fio_distr_plots.py"
AVG_PLOTS_SCRIPT="avg_plots.py"

WBS_LIST=("4K" "8K" "16K")  
RBS_LIST=("4K" "8K" "16K") 


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

extract_metrics() {
    local log_file=$1
    local run_id=$2
    local bs=$3
    local mode=$4

    local bw=$(grep -oP 'WRITE: bw=[0-9]+MiB/s \(\K[0-9]+' "$log_file" | head -1)
    
	# Extract IOPS from the main log file and remove 'k' if present
    local iops=$(grep -oP 'IOPS=\K[0-9]+(\.[0-9]+)?k?' "$log_file" | sed 's/k//g' | awk '{s+=$1} END {print s}')
	# Function to calculate average latency from a given log file
    calc_avg_latency() {
		local file=$1
		local result=$(awk -F',' '{sum+=$2; count++} END {if(count>0) print sum/count; else print 0}' "$file")
		echo "$result"
	}

    local slat_file="logs/write${run_id}_slat.log"
    local clat_file="logs/write${run_id}_clat.log"
    local lat_file="logs/write${run_id}_lat.log"

    local avg_slat=$(calc_avg_latency "$slat_file")
    local avg_clat=$(calc_avg_latency "$clat_file")
    local avg_lat=$(calc_avg_latency "$lat_file")
	echo $avg_lat 
	echo $avg_clat
	echo $avg_slat
	echo $iops
    echo "$run_id $bs $bw $iops $avg_slat $avg_clat $avg_lat $mode" >> "$RESULTS_FILE"
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
make clean

if ! command -v fio2gnuplot &> /dev/null; then
    echo "Error: 'fio2gnuplot' is not installed. Please install it and try again."
    exit 1
fi

### DISTRIBUTION + SNIA BENCHMARK ###

echo "# RUN BS BW IOPS SLAT CLAT LAT MODE" > "$RESULTS_FILE"
mkdir -p $LOGS_PATH $PLOTS_PATH/histograms $PLOTS_PATH/avg

for bs in "${WBS_LIST[@]}"; do  
	for i in $(seq 1 $RUNS); do
		echo "Run $i of $RUNS..."
	 
		LOG_FILE="$LOGS_PATH/fio_run_${i}.log"
    
		make fio_perf_w_opt ID=$IO_DEPTH NJ=$JOBS_NUM IN=$i > "$LOG_FILE"
    
		extract_metrics "$LOG_FILE" "$i" "$bs" "write"
	done
done

echo "Data collected in $RESULTS_FILE"

python3 "$AVG_PLOTS_SCRIPT"

python3 "$HISTOGRAM_PLOTS_SCRIPT" 

## read bench with reinit - todo

echo "Histograms and statistics saved in $PLOTS_PATH"

