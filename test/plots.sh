#!/bin/bash

JOBS_NUM=4
IO_DEPTH=16
LOGS_PATH="logs/"
PLOTS_PATH="../plots"

# Function to prioritize all the fio processes (including forks in case of numjobs > 1)
prioritise_fio() {
    echo -e "\nPrioritizing fio process..."
    for pid in $(pidof fio); do
        echo -e "\nchrt $pid"
        chrt -r 99 "$pid"
    done
}

echo -e "\nCleaning the logs directory"
make clean_logs

if ! command -v fio2gnuplot &> /dev/null; then
    echo "Error: 'fio2gnuplot' is not installed. Please install it and try again."
    exit 1
fi

echo -e "\nRunning fio..."
mkdir logs 
make fio_perf_wr_opt ID=$IO_DEPTH NJ=$JOBS_NUM 
prioritise_fio

echo -e "\nLogs list:"
ls -lah logs

echo -e "\nGenerating the plots"
mkdir -p $PLOTS_PATH/lat $PLOTS_PATH/iops $PLOTS_PATH/bw

cd logs
fio2gnuplot -g -t "Read Latency" -o "$PLOTS_PATH/lat/read_lat" -p "read_*lat*.log"  -d "$PLOTS_PATH/lat"
fio2gnuplot -g -t "Write Latency" -o "$PLOTS_PATH/lat/write_lat" -p "write_*lat*.log"  -d "$PLOTS_PATH/lat"
fio2gnuplot -g -t "Read IOPS" -o "$PLOTS_PATH/iops/read_iops" -p "read_iops.log"  -d "$PLOTS_PATH/iops"
fio2gnuplot -g -t "Write IOPS" -o "$PLOTS_PATH/iops/write_iops" -p "write_iops.log"  -d "$PLOTS_PATH/iops"
fio2gnuplot -g -t "Read Bandwidth" -o "$PLOTS_PATH/bw/read_bw" -p "read_bw.log"  -d "$PLOTS_PATH/bw"
fio2gnuplot -g -t "Write Bandwidth" -o "$PLOTS_PATH/bw/write_bw" -p "write_bw.log"  -d "$PLOTS_PATH/bw"
echo -e "\nPlots generated in $PLOTS_PATH"

