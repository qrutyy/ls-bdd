#!/bin/bash

source ./configurable_params.sh

readonly LOGS_PATH="logs"
readonly PLOTS_PATH="./plots"
readonly RESULTS_FILE="$LOGS_PATH/fio_results.dat"
readonly LAT_RESULTS_FILE="$LOGS_PATH/fio_lat_results.dat"
readonly CONC_IOPS_PLOTS_SCRIPT="iops_conc_plots.py"
readonly CONC_GENERAL_DIFF_PLOT="general_conc_plots.py"

usage() {
    echo "Usage: $0 [--io_depth number] [--jobs_num number]"
    exit 1
}

# Function to prioritize all the fio processes (including forks in case of numjobs > 1)
# UPD: not used, bc fio already prioritizes the IO operations
prioritise_fio() {
    echo -e "\nPrioritizing fio process..."
    for pid in $(pidof fio); do
        echo -e "\nchrt $pid"
        chrt -r 99 "$pid"
    done
}

prepare_env() {
    echo -e "\nCleaning the logs directory"
    make clean > /dev/null

	mkdir -p "$LOGS_PATH" \
    "$PLOTS_PATH"/distribution/{vbd,raw}/{rewrite,non_rewrite}/{tp,iops} \
    "$PLOTS_PATH"/avg/{vbd,raw}/{rewrite,non_rewrite}/{bw,tp_conc,iops,iops_conc} \
    "$PLOTS_PATH"/latency/{rewrite,non_rewrite}/{raw,vbd} 
}

# Reinits the lsbdd module
reinit_lsvbd() {
	local ds=$1

	if [ "$ds" == "" ]; then
		ds=$PL_CUR_DS
	fi 

	make -C ../src exit DBI=1 > /dev/null

	sync; echo 3 | sudo tee /proc/sys/vm/drop_caches

	make -C ../src init_no_recompile DS="$ds" TY="$PL_TYPE" BD="$BD_NAME" > /dev/null
}

# Performs warm-up with workload as big as the block device.
workload_independent_preconditioning() {
    local wbs=$1 wf_size_per_job

	wf_size_per_job=$((BD_SIZE / 10)) 

	echo -e "\nRunning warm-up with size $wf_size_per_job for each job"

	fio --name=prep --rw=write --bs="$wbs"K --numjobs=10 --iodepth=32 --ioengine=io_uring --size="$wf_size_per_job"G \
        --filename=/dev/$VBD_NAME --direct=1 --output="$LOGS_PATH/preconditioning.log"
}

<<docs
Extracts throughput metrics from the log file and writes to the general log used for plotting.
Uses the last line of the FIO's stdout (so gets the all in all throughput, not thread-specific). 
Converts to GB/s if needed.  

@param run_id - number of the run (repeat id) 
@param log_file - path to FIO log_file being the FIO's log gathered with --write_lat_log option
@param bs - used block size
@param rw_mix - current Read/Write mix used (see fio docs - rwmixread/rwmixwrite)
docs
extract_tp_metrics() {
    local log_file=$1
    local run_id=$2
    local bs=$3
    local mix=$4
	local iodepth=$5
	local numjobs=$6
    local bw=""

    local op_type="WRITE"
    if [[ "$mix" != "0-100" ]]; then
        op_type="READ"
    fi

    summary_line=$(grep -A 1 "Run status group 0 (all jobs):" "$log_file" | grep "^\s*${op_type}: bw=")

    if [[ -n "$summary_line" ]]; then
        bw_gb=$(echo "$summary_line" | grep -oP '\(\K[0-9]+(\.[0-9]+)?(?=GB/s\))' | head -1)

        if [[ -n "$bw_gb" ]]; then
            bw=$(awk -v val="$bw_gb" 'BEGIN { printf "%.0f", val }')
        else
            bw_mb=$(echo "$summary_line" | grep -oP '\(\K[0-9]+(\.[0-9]+)?(?=MB/s\))' | head -1)
            if [[ -n "$bw_mb" ]]; then
                bw=$(awk -v val="$bw_mb" 'BEGIN { printf "%.0f", val / 1000 }')
            fi
        fi
    fi

    if [[ -z "$bw" ]]; then
        echo "ERROR: Could not extract BW (MB/s or GB/s) for run_id=$run_id, bs=$bs, mix=$mix from $log_file"
		bw="0" # Default (error) value  
    fi
	if [ "$numjobs" == "" ] && [ "$iodepth" == "" ]; then
		numjobs="0"
		iodepth="0"
	fi
    echo "DEBUG: Extracted BW='$bw' (target GB/s) for op_type=$op_type"
    echo "$run_id $bs $mix $bw 0 TP $iodepth $numjobs" >> "$RESULTS_FILE"
}

<<docs
Extracts IOPS metrics from the log file and writes to the general log used for plotting.
Uses the last line of the FIO's stdout (so gets the all in all IOPS, not thread-specific). 
Converts to thousand IOPS if needed.  

@param run_id - number of the run (repeat id) 
@param log_file - path to FIO log_file being the FIO's log gathered with --write_lat_log option
@param bs - used block size
@param rw_mix - current Read/Write mix used (see fio docs - rwmixread/rwmixwrite)
@param iodepth 
@param numjobs
docs
extract_iops_metrics() {
    local log_file=$1
    local run_id=$2
	local ds=$3
    local bs=$4
    local rw_mix=$5
	local rw_type=$6
	local iodepth=$7
	local numjobs=$8

    local iops

    iops=$(grep -oP 'IOPS=\K[0-9]+(\.[0-9]+)?[kKmM]?' "$log_file" | \
        awk '{
            val=$1
            gsub(/K/, "k", val)
            gsub(/M/, "m", val)

            if (val ~ /k$/) {
                sub(/k/, "", val)
                s += val * 1000
            } else if (val ~ /m$/) {
                sub(/m/, "", val)
                s += val * 1000000
            } else {
                s += val
            }
        } END { printf "%.0f", s }')

	if [ "$numjobs" == "" ] && [ "$iodepth" == "" ]; then
		numjobs="0"
		iodepth="0"
	fi
    echo "DEBUG: Extracted IOPS='$iops', NJ=$numjobs, ID=$iodepth"
    echo "$run_id $ds $bs $rw_mix 0 $iops IOPS $rw_type $iodepth $numjobs" >> "$RESULTS_FILE"
}

<<docs
Extracts current latency metrics fro mthe log file and writes to the general log_file used for plotting.
Includes:
- average latency
- maximum latency
- 99 percentile of latency

@param run_id - number of the run (repeat id) 
@param log_file - path to FIO log_file being the FIO's log gathered with --write_lat_log option
@param bs - used block size
@param rw_mix - current Read/Write mix used (see fio docs - rwmixread/rwmixwrite)
docs
extract_latency_metrics() {
    local run_id=$1
    local log_file=$2
    local bs=$3
    local rw_mix=$4
	local iodepth=$5 
	local numjobs=$6

    calc_avg_latency() {
        local file=$1
        local result
		result=$(awk -F',' '{sum+=$2; count++} END {if(count>0) print sum/count/1000; else print 0}' "$file")
        echo "$result"
    }

    calc_max_latency() {
        local file=$1
        local result
		result=$(awk -F',' 'BEGIN {max=0} {if($2>max) max=$2} END {print max/1000}' "$file")
        echo "$result"
    }

	calc_99p_latency() {
		local file=$1
		local result
		result=$(awk -F',' '{print $2}' "$file" | sort -n | awk '
			BEGIN { count = 0 }
			{ values[++count] = $1 }
			END {
				if (count > 0) {
					idx = int(count * 0.99 + 0.5)
					if (idx > count) idx = count
					if (idx < 1) idx = 1
					print values[idx]/1000
				}
			}')
		echo "$result"
	}

	local slat_file="${log_file}_slat.1.log"
	local clat_file="${log_file}_clat.1.log"
	local lat_file="${log_file}_lat.1.log"

    local avg_slat
	avg_slat=$(calc_avg_latency "$slat_file")
    local avg_clat
	avg_clat=$(calc_avg_latency "$clat_file")
    local avg_lat
	avg_lat=$(calc_avg_latency "$lat_file")

    local max_slat
	max_slat=$(calc_max_latency "$slat_file")
    local max_clat
	max_clat=$(calc_max_latency "$clat_file")
    local max_lat
	max_lat=$(calc_max_latency "$lat_file")

    local p99_slat
	p99_slat=$(calc_99p_latency "$slat_file")
    local p99_clat
	p99_clat=$(calc_99p_latency "$clat_file")
    local p99_lat
	p99_lat=$(calc_99p_latency "$lat_file")

	if [ "$numjobs" == "" ] && [ "$iodepth" == "" ]; then
		numjobs="0"
		iodepth="0"
	fi

	echo -e "DEBUG: extracted \nId:$run_id \nBS:$bs \nAVG_SLAT:$avg_slat AVG_CLAT:$avg_clat AVG_LAT:$avg_lat \nMAX_SLAT:$max_slat MAX_CLAT:$max_clat MAX_LAT:$max_lat \nP99_LAT:$p99_slat P99_CLAT:$p99_clat P99_LAT:$p99_lat \nRW_MIX:$rw_mix\n"

	echo "$run_id $ds $bs $rw_mix $rw_type $avg_slat $avg_clat $avg_lat $max_slat $max_clat $max_lat $p99_slat $p99_clat $p99_lat $iodepth $numjobs" >> "$LAT_RESULTS_FILE"
}

<<docs
Runs IOPS tests based on SNIA specification. Uses fio_iops_mix cfg from ./Makefile. 

@param device - target device (f.e. /dev/lsvbd1) (just log needed)
@param rewrite_mode - shows if warm-up is needed and if the read tests are included
	1 - mode is on (enables warm-up and read tests)
	0 - mode is off
@param conc_mode - enables the conccurent performance evaluation 
@param ds - data structure (sl/ht/...)
docs
run_iops_for_each_nj_id() {
    local mode=$1 raw_bs_list=$2 log_file fs_flag iodepth bs_list rw_mix 
	local IFS=','

	read -r -a bs_list <<< "$raw_bs_list"

	if [ "$mode" == "write" ]; then
		rw_mix="0-100"
	elif [ "$mode" == "read" ]; then
		rw_mix="100-0"
	else 
		echo -e "Failed to parse mode argument (only write/read are supported)\n"
		return
	fi 

	echo -e "---Starting IOPS Benchmark on $BD_NAME...---\n"
	
	rw_mix_read="${rw_mix%-*}"
	rw_mix_write="${rw_mix#*-}"
	
	echo "$rw_mix_read"
	echo "$rw_mix_write"
	
	for rw_type in "${PL_RW_TYPES[@]}"; do
		for bs in "${bs_list[@]}"; do
			for ds in "${PL_AVAILABLE_DS[@]}"; do 
				reinit_lsvbd "$ds" # to make sure that ds is selected right

				if [ "$rw_mix_read" != "0" ]; then 
					workload_independent_preconditioning "$bs"
				fi

				for nj in "${PL_IOPS_CONC_NJ_LIST[@]}"; do 
					iodepth=$(echo "$nj * 4" | bc)

					for ((i=1;i<=PL_RUNS+1;i++)); do
						echo "Running with bs = $bs, iodepth = $iodepth and nj = $nj, ds = $ds, rw_type = $rw_type rw_mix = $rw_mix ..."

						log_file="$LOGS_PATH/fio_${rw_mix}_run_${i}.log"
						
						make fio_perf_mix FS=$VBD_NAME RW_TYPE="$rw_type" RWMIX_READ="$rw_mix_read" RWMIX_WRITE="$rw_mix_write" BS="$bs" ID="$iodepth" NJ="$nj" > "$log_file" 2>&1

						extract_iops_metrics "$log_file" "$i" "$ds" "$bs" "$rw_mix" "$rw_type" "$iodepth" "$nj"

						reinit_lsvbd "$ds" 
					done
				done
			done

			echo "Data collected in $RESULTS_FILE"
			python3 "$CONC_IOPS_PLOTS_SCRIPT"  
		done
	done
	make clean_logs > /dev/null
}

<<docs
Runs IOPS tests based on SNIA specification. Uses fio_iops_mix cfg from ./Makefile. 

@param device - target device (f.e. /dev/lsvbd1) (just log needed)
@param rewrite_mode - shows if warm-up is needed and if the read tests are included
	1 - mode is on (enables warm-up and read tests)
	0 - mode is off
@param conc_mode - enables the conccurent performance evaluation 
@param ds - data structure (sl/ht/...)
docs
run_general_conc_cases() {
    local raw_bs_list=$1 nj=$2 id=$3 metric=$4 log_file bs_list rw_mix 
	local IFS=','

	read -r -a bs_list <<< "$raw_bs_list"

	echo -e "---Starting General (IOPS + LAT) Benchmark on $BD_NAME...---\n"
	

	for ds in "${PL_AVAILABLE_DS[@]}"; do 
		reinit_lsvbd "$ds" 
		for rw_mix in "${PL_RW_MIXES[@]}"; do 
			rw_mix_read="${rw_mix%-*}"
			rw_mix_write="${rw_mix#*-}"

			for rw_type in "${PL_RW_TYPES[@]}"; do
				for bs in "${bs_list[@]}"; do

					if [ "$rw_mix_read" != "0" ]; then 
						workload_independent_preconditioning "$bs"
					fi

					for ((i=1;i<=PL_RUNS+1;i++)); do
						echo "Running with bs = $bs, iodepth = $id and nj = $nj, ds = $ds, rw_mix = $rw_mix, rw_type = $rw_type ..."

						log_file="$LOGS_PATH/fio_${rw_mix}_run_${i}.log"

						if [ "$metric" == "IOPS" ]; then
							make fio_perf_mix FS=$VBD_NAME RW_TYPE="$rw_type" RWMIX_READ="$rw_mix_read" RWMIX_WRITE="$rw_mix_write" BS="$bs" ID="$id" NJ="$nj" > "$log_file"

							extract_iops_metrics "$log_file" "$i" "$ds" "$bs" "$rw_mix" "$rw_type" "$id" "$nj"

						elif [ "$metric" == "LAT" ]; then 
							make fio_lat_mix FS=$VBD_NAME RW_TYPE="$rw_type" RWMIX_READ="$rw_mix_read" RWMIX_WRITE="$rw_mix_write" BS="$bs" ID="$id" NJ="1" > "$log_file"

							extract_latency_metrics "$log_file" "$i" "$ds" "$bs" "$rw_mix" "$rw_type" "$id" "1"
						else 
							echo -e "unknown metric"
						fi
	
						reinit_lsvbd "$ds" 
					done
				done
			done
		done 
	done 

	echo "Data collected in $RESULTS_FILE"
	python3 "$CONC_GENERAL_DIFF_PLOT" "$metric"
#	make clean_logs > /dev/null
}

# Parse options
while [[ "$#" -gt 0 ]]; do
    case $1 in
        -h|--help) echo "Usage: $0 [--bd_size size of underlyiing block device] [--bd_name name of underlying block device]"; exit 1 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
    shift
done

prepare_env

for cfg_entry in "${PL_GENERAL_CONC_CFG[@]}"; do
	read -r operation block_size <<< "$cfg_entry"

	run_iops_for_each_nj_id "$operation" "$block_size"
done

for cfg_entry in "${PL_GENERAL_CONC_CFG[@]}"; do
	read -r block_size nj id metric <<< "$cfg_entry"

	run_general_conc_cases "$block_size" "$nj" "$id" "$metric"
done

#echo -e "\nCleaning the logs directory"
#make clean > /dev/null
