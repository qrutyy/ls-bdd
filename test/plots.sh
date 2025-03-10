#!/bin/bash

JOBS_NUM=4
IO_DEPTH=16
LOGS_PATH="logs"
PLOTS_PATH="./plots"
RESULTS_FILE="fio_results.dat"
RUNS=20
HISTOGRAM_SCRIPT="fio_distr.py"

# Function to prioritize all the fio processes (including forks in case of numjobs > 1)
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
make clean_logs

if ! command -v fio2gnuplot &> /dev/null; then
    echo "Error: 'fio2gnuplot' is not installed. Please install it and try again."
    exit 1
fi

### BASIC BW/IOPS - TIME TESTS ###

echo -e "\nRunning fio..."
mkdir -p "$LOGS_PATH" "$PLOTS_PATH"
make fio_perf_wr_opt ID=$IO_DEPTH NJ=$JOBS_NUM 
prioritise_fio

echo -e "\nLogs list:"
ls -lah "$LOGS_PATH"

echo -e "\nGenerating the plots"
mkdir -p "$PLOTS_PATH/lat" "$PLOTS_PATH/iops" "$PLOTS_PATH/bw"

cd "$LOGS_PATH" || exit
fio2gnuplot -g -t "Read Latency" -o "$PLOTS_PATH/lat/read_lat" -p "read_*lat.log"  -d "$PLOTS_PATH/lat"
fio2gnuplot -g -t "Write Latency" -o "$PLOTS_PATH/lat/write_lat" -p "write_*lat.log"  -d "$PLOTS_PATH/lat"
fio2gnuplot -g -t "Read IOPS" -o "$PLOTS_PATH/iops/read_iops" -p "read_iops.log"  -d "$PLOTS_PATH/iops"
fio2gnuplot -g -t "Write IOPS" -o "$PLOTS_PATH/iops/write_iops" -p "write_iops.log"  -d "$PLOTS_PATH/iops"
fio2gnuplot -g -t "Read Bandwidth" -o "$PLOTS_PATH/bw/read_bw" -p "read_bw.log"  -d "$PLOTS_PATH/bw"
fio2gnuplot -g -t "Write Bandwidth" -o "$PLOTS_PATH/bw/write_bw" -p "write_bw.log"  -d "$PLOTS_PATH/bw"

echo -e "\nPlots generated in $PLOTS_PATH"

cd ..

### DISTRIBUTION TESTS ###

echo "# Run Bandwidth IOPS Latency" > "$RESULTS_FILE"

# Function to extract metrics from fio output
extract_metrics() {
    local log_file=$1
    local run_id=$2
    
	BW=$(grep -oP 'WRITE: bw=[0-9]+MiB/s \(\K[0-9]+' "$log_file" | head -1)
    IOPS=$(grep -oP 'IOPS=\K[0-9]+' "$log_file" | head -1) 
    LAT=$(grep -oP 'lat \([^\)]*\), avg=\K[0-9.]+' "$log_file" | head -1) 

    echo "$run_id $BW $IOPS $LAT" >> "$RESULTS_FILE"
}

mkdir -p $LOGS_PATH 

for i in $(seq 1 $RUNS); do
    echo "Run $i of $RUNS..."
	 
    LOG_FILE="$LOGS_PATH/fio_run_${i}.log"
    
    make fio_perf_w_opt ID=$IO_DEPTH NJ=$JOBS_NUM > "$LOG_FILE"
    
    extract_metrics "$LOG_FILE" "$i"
done

echo "Data collected in $RESULTS_FILE"

python3 "$HISTOGRAM_SCRIPT"

echo "Histograms and statistics saved in $PLOTS_PATH"

