#!/bin/bash

JOBS_NUM=4
IO_DEPTH=32
RUNS=25
BRD_SIZE=2
DAST="sl"
TYPE="lf"

LOGS_PATH="logs"
PLOTS_PATH="./plots"
RESULTS_FILE="logs/fio_results.dat"
LAT_RESULTS_FILE="logs/fio_lat_results.dat"

HISTOGRAM_PLOTS_SCRIPT="fio_distr_plots.py"
AVG_PLOTS_SCRIPT="avg_plots.py"
LATENCY_PLOTS_SCRIPT="lat_plots.py"

BS_LIST=("4K" "8K" "16K" "32K")
# RBS_LIST=("4K" "8K" "16K") 
# Can be used to benchmark read operations (bio splits)
# Not used in benchmarking bc its kinda more related to optional functionality

LATENCY_WRBS_LIST=("4K" "8K" "16K" "32K") ## SNIA recommends 0.5K also, need some convertion
RW_MIXES=("0-100" "65-35" "100-0") ## Write to read ops ratio

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

prepare_env() {
    echo -e "\nCleaning the logs directory"
    make clean > /dev/null
    mkdir -p $LOGS_PATH $PLOTS_PATH/histograms/{write,read,raw/{write,read}} \
        $PLOTS_PATH/avg/{write,read,raw/{write,read} \ 
		$PLOTS_PATH/latency/raw}
}

reinit_lsvbd() {
    make -C ../src exit DBI=1 > /dev/null
    
	sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
    
	modprobe -r brd
    modprobe brd rd_nr=1 rd_size=$((BRD_SIZE * 1048576))
    
	make -C ../src init_no_recompile DS=${DAST} TY=${TYPE} > /dev/null
}

workload_independent_preconditioning() {
    local wbs=$1
    fio --name=prep --rw=write --bs=${wbs}K --numjobs=1 --iodepth=1 --size=${BRD_SIZE}G \
        --filename=/dev/lsvbd1 --direct=1 --output="$LOGS_PATH/preconditioning.log"
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
			bw=$(echo "$bw_gb * 1000" | bc)
		fi
	else
		local bw=$(grep -oP 'READ: bw=[0-9]+MiB/s \(\K[0-9]+' "$log_file" | head -1)
		if [[ -z "$bw" ]]; then
			bw_gb="$(grep -oP 'READ: bw=.*\(([0-9]+\.[0-9]+)GB/s\)' "$log_file" | grep -oP '[0-9]+\.[0-9]+' | tail -n1)"
			bw=$(echo "$bw_gb * 1000" | bc)
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

run_tests() {
    local device=$1 is_raw=$2 mode log_file fs_flag extra_args
    fs_flag=$([[ $is_raw -eq 1 ]] && echo "FS=ram0" || echo "")
	
	echo -e "Starting Read and Write operations Benchmark on $device...\n"

    for mode in "write" "read"; do
        echo -e "\nRunning $mode tests on $device\n"
        for bs in "${BS_LIST[@]}"; do
            workload_independent_preconditioning "$bs"
            for i in $(seq 1 $RUNS); do
                echo "Run $i of $RUNS..."
                log_file="$LOGS_PATH/fio_${mode:0:1}_run_${i}.log"
                extra_args=$([[ $mode == "read" ]] && echo "RBS=$bs" || echo "")

                make fio_perf_${mode:0:1}_opt $fs_flag ID=$IO_DEPTH NJ=$JOBS_NUM IN=$i $extra_args > "$log_file"
                extract_all_metrics "$log_file" "$i" "$bs" "$([[ $mode == "write" ]] && echo "0" || echo "$bs")" "$mode"
            done
            reinit_lsvbd
        done

        echo "Data collected in $RESULTS_FILE"
        python3 "$AVG_PLOTS_SCRIPT" $([[ $is_raw -eq 1 ]] && echo "--raw")
        python3 "$HISTOGRAM_PLOTS_SCRIPT" $([[ $is_raw -eq 1 ]] && echo "--raw")
        make clean_logs > /dev/null
    done
}

run_latency_tests() {
    local device=$1 is_raw=$2 bs rw_mix log_file

    echo -e "Starting SNIA-complied Latency Benchmark on $device...\n"
    for rw_mix in "${RW_MIXES[@]}"; do
        for bs in "${LATENCY_WRBS_LIST[@]}"; do
            echo -e "\nPerforming a block device warm-up..."
            workload_independent_preconditioning "$bs"

            for i in $(seq 1 $RUNS); do
                echo "Run $i of $RUNS..."
                log_file="$LOGS_PATH/latency_${bs}_${rw_mix}"
                fio --name=latency_test --rw=randrw --rwmixread=${rw_mix%-*} --rwmixwrite=${rw_mix#*-} \
                    --bs=${bs} --numjobs=1 --iodepth=1 --time_based --runtime=30 --direct=1 --hipri=1 \
                    --write_lat_log=$log_file --ioengine=io_uring --registerfiles=1 \
					--filename=/dev/$device > /dev/null
                extract_latency_metrics "$i" "$log_file" "$bs" "$rw_mix"
            done

            reinit_lsvbd
        done
    done

    python3 "$LATENCY_PLOTS_SCRIPT" $([[ $is_raw -eq 1 ]] && echo "--raw")
    make clean_logs > /dev/null
}

# Parse options
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --io_depth) IO_DEPTH="$2"; shift ;;
        --jobs_num) JOBS_NUM="$2"; shift ;;
        --brd_size) BRD_SIZE="$2"; shift ;;
        -h|--help) echo "Usage: $0 [--io_depth number] [--jobs_num number]"; exit 1 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
    shift
done

prepare_env

# Run tests for LSVBD
run_tests "lsvbd1" 0
run_latency_tests "lsvbd1" 0

# Run tests for RAMDISK (raw mode)
run_tests "ram0" 1
run_latency_tests "ram0" 1

echo "Histograms, AVG plots, and statistics saved in $PLOTS_PATH"

echo -e "\nCleaning the logs directory"
make clean > /dev/null
