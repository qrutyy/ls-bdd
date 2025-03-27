#!/bin/bash

JOBS_NUM=4
IO_DEPTH=32
RUNS=5
BRD_SIZE=2
DAST="sl"
TYPE="lf"

LOGS_PATH="logs"
PLOTS_PATH="./plots"
RESULTS_FILE="logs/fio_results.dat"
LAT_RESULTS_FILE="logs/fio_lat_results.dat"

HISTOGRAM_PLOTS_SCRIPT="distr_plots.py"
AVG_PLOTS_SCRIPT="avg_plots.py"
LATENCY_PLOTS_SCRIPT="lat_plots.py"

IOPS_BS_LIST=("4K" "8K" "16K" "32K" "64K" "128K")
# RBS_LIST=("4K" "8K" "16K") 
# Can be used to benchmark read operations (bio splits)
# Not used in benchmarking bc its kinda more related to optional functionality
IOPS_RW_MIXES=("100-0" "65-35" "0-100") # SNIA recommends more mixes (like 95-5, 50-50, ...)

LAT_BS_LIST=("2K" "4K" "8K") ## SNIA recommends 0.5K also, need some convertion, replaced it with 2K
LAT_RW_MIXES=("0-100" "65-35" "100-0") # Write to read ops ratio

TP_BS_LIST=("128K" "1024K") 
TP_RW_MIXES=("0-100" "100-0")

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
    mkdir -p $LOGS_PATH $PLOTS_PATH/histograms/{iops,tp,raw/{tp,iops}} \
        $PLOTS_PATH/avg/{tp,iops,raw/{tp,iops}} \ 
		$PLOTS_PATH/latency/raw
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

extract_iops_metrics() {
    local log_file=$1
    local run_id=$2
    local bs=$3
    local mix=$4

	local iops=$(grep -oP 'IOPS=\K[0-9]+(\.[0-9]+)?k?' "$log_file" | sed 's/k//g' | awk '{s+=$1} END {print s}')
	echo "DEBUG: Extracted IOPS='$iops'"

	echo "$run_id $wbs $rbs $bw $iops 0 0 0 $mode" >> "$RESULTS_FILE"
}

extract_tp_metrics() {
    local log_file=$1
    local run_id=$2
    local bs=$3
	local mix=$4

	if [[ "$mix" == "0-100" ]]; then	
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

    echo "DEBUG: Extracted BW='$bw'"

    echo "$run_id $bs $mix $bw 0 tp" >> "$RESULTS_FILE"
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

    echo "$run_id $bs $avg_slat $avg_clat $avg_lat $max_slat $max_clat $max_lat $p95_slat $p95_clat $p95_lat $rw_mix lat" >> "$LAT_RESULTS_FILE"
}

run_tp_tests() {
    local device=$1 is_raw=$2 rw_mix log_file fs_flag extra_args
    fs_flag=$([[ $is_raw -eq 1 ]] && echo "FS=nullb0" || echo "")
	
	echo -e "---Starting Throughput operations Benchmark on $device...---\n"

	for rw_mix in "${TP_RW_MIXES[@]}"; do
        echo -e "Running $rw_mix tests on $device\n"
		rwmix_read="${rw_mix%-*}"
		rwmix_write="${rw_mix#*-}"

        for bs in "${TP_BS_LIST[@]}"; do
			if [ rw_mix != "0-100" ]; then
                workload_independent_preconditioning "$bs"
            fi

            for i in $(seq 1 $RUNS); do
                echo "Run $i of $RUNS..."
                log_file="$LOGS_PATH/fio_${rw_mix}_run_${i}.log"
                extra_args=$([[ $rw_mix == "100-0" ]] && echo "RBS=$bs" || echo "")

				make fio_perf_mix $fs_flag RWMIX_READ=$rwmix_read RWMIX_WRITE=$rwmix_write BS=$bs ID=$IO_DEPTH NJ=$JOBS_NUM $extra_args > "$log_file"

                extract_tp_metrics "$log_file" "$i" "$bs" "$rw_mix"
            done
            reinit_lsvbd
        done

        echo "Data collected in $RESULTS_FILE"
        python3 "$AVG_PLOTS_SCRIPT" $([[ $is_raw -eq 1 ]] && echo "--raw") --tp
        python3 "$HISTOGRAM_PLOTS_SCRIPT" $([[ $is_raw -eq 1 ]] && echo "--raw")
        make clean_logs > /dev/null
    done
}

run_iops_tests() {
    local device=$1 is_raw=$2 rw_mix log_file fs_flag extra_args
    fs_flag=$([[ $is_raw -eq 1 ]] && echo "FS=nullb0" || echo "")
	
	echo -e "---Starting IOPS Benchmark on $device...\n---"

	for rw_mix in "${IOPS_RW_MIXES[@]}"; do
        echo -e "Running $rw_mix tests on $device\n"
		rwmix_read="${rw_mix%-*}"
		rwmix_write="${rw_mix#*-}"

        for bs in "${IOPS_BS_LIST[@]}"; do
    		if [ rw_mix != "0-100" ]; then
                workload_independent_preconditioning "$bs"
            fi
        
			for i in $(seq 1 $RUNS); do
                echo "Run $i of $RUNS..."
                log_file="$LOGS_PATH/fio_${rw_mix:0:1}_run_${i}.log"
                extra_args=$([[ $rw_mix == "100-0" ]] && echo "RBS=$bs" || echo "")
			
				make fio_perf_mix $fs_flag RWMIX_READ=$rwmix_read RWMIX_WRITE=$rwmix_write BS=$bs ID=$IO_DEPTH NJ=$JOBS_NUM $extra_args > "$log_file"

				extract_iops_metrics "$log_file" "$i" "$bs" "$rw_mix"
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

    echo -e "---Starting SNIA-complied Latency Benchmark on $device...---\n"
    for rw_mix in "${LAT_RW_MIXES[@]}"; do
        for bs in "${LAT_BS_LIST[@]}"; do
            echo -e "Performing a block device warm-up..."
			if [ rw_mix != "0-100" ]; then
                workload_independent_preconditioning "$bs"
            fi

            for i in $(seq 1 $RUNS); do
                echo "Run $i of $RUNS..."
                log_file="$LOGS_PATH/latency_${bs}_${rw_mix}"
                fio --name=latency_test --rw=randrw --rwmixread=${rw_mix%-*} --rwmixwrite=${rw_mix#*-} \
                    --bs=${bs} --numjobs=1 --iodepth=1 --time_based --runtime=10 --direct=1 \
                    --write_lat_log=$log_file --ioengine=io_uring --registerfiles=1 --hipri=0 \
					--cpus_allowed=0-6 --fixedbufs=1 --filename=/dev/$device > /dev/null
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
#run_tp_tests "lsvbd1" 0
run_iops_tests "lsvbd1" 0
run_latency_tests "lsvbd1" 0

# Run tests for RAMDISK (raw mode)
run_tp_tests "nullb0" 1
run_iops_tests "nullb0" 1
run_latency_tests "nullb0" 1

echo "Histograms, AVG plots, and statistics saved in $PLOTS_PATH"

echo -e "\nCleaning the logs directory"
make clean > /dev/null
