#!/bin/bash

JOBS_NUM=8
IO_DEPTH=32
RUNS=25
NBD_SIZE=400
DAST="sl"
TYPE="lf"

LOGS_PATH="logs"
PLOTS_PATH="./plots"
RESULTS_FILE="$LOGS_PATH/fio_results.dat"
LAT_RESULTS_FILE="$LOGS_PATH/fio_lat_results.dat"

HISTOGRAM_PLOTS_SCRIPT="distr_plots.py"
AVG_PLOTS_SCRIPT="tp_iops_plots.py"
LATENCY_PLOTS_SCRIPT="lat_plots.py"

IOPS_BS_LIST=("4K" "8K" "16K" "32K" "64K" "128K") # + 2K by SNIA
# Can be used to benchmark read operations (bio splits)
# Not used in benchmarking bc its kinda more related to optional functionality
IOPS_RW_MIXES=("0-100" "100-0") # SNIA recommends more mixes (like 99-5, 50-50, ...)

LAT_BS_LIST=("4K" "8K") # SNIA recommends 0.5K and 2K also
LAT_RW_MIXES=("0-100" "65-35" "100-0") # READ to WRITE operations ratio

TP_BS_LIST=("128K" "1024K") 
TP_RW_MIXES=("0-100" "100-0")

IOPS_CONC_RW_MIXES="$IOPS_RW_MIXES"
IOPS_CONC_BS_LIST=("4K" "8K")
IOPS_CONC_NJ_LIST=("1" "2" "4" "6" "8") 

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

# Reinits the lsbdd and null_blk modules
reinit_lsvbd() {
	make -C ../src exit DBI=1 > /dev/null

	sync; echo 3 | sudo tee /proc/sys/vm/drop_caches

	modprobe -r null_blk
	modprobe null_blk queue_mode=0 gb=$NBD_SIZE bs=512 irqmode=0 nr_devices=1
	make -C ../src init_no_recompile DS=${DAST} TY=${TYPE} > /dev/null
}

# Performs warm-up with workload as big as the block device.
workload_independent_preconditioning() {
    local wbs=$1

	echo -e "\nRunning warm-up with size 125GB for each job"

	fio --name=prep --rw=write --bs="${wbs}"K --numjobs=8 --iodepth=32 --ioengine=io_uring --size=50G \
        --filename=/dev/lsvbd1 --direct=1 --output="$LOGS_PATH/preconditioning.log"
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
    local bs=$3
    local rw_mix=$4
	local iodepth=$5
	local numjobs=$6

    local iops
    iops=$(grep -oP 'IOPS=\K[0-9]+(\.[0-9]+)?k?' "$log_file" | \
        awk '{
            if ($1 ~ /k$/) {
                sub(/k/, "", $1);
                s += $1;
            } else {
                s += $1 / 1000;
            }
        } END { printf "%.3f", s }')

	if [ "$numjobs" == "" ] && [ "$iodepth" == "" ]; then
		numjobs="0"
		iodepth="0"
	fi
    echo "DEBUG: Extracted IOPS='$iops', NJ=$numjobs, ID=$iodepth"
    echo "$run_id $bs $rw_mix 0 $iops iops $iodepth $numjobs" >> "$RESULTS_FILE"
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
    echo "$run_id $bs $mix $bw 0 tp $iodepth $numjobs" >> "$RESULTS_FILE"
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

    echo "$run_id $bs $avg_slat $avg_clat $avg_lat $max_slat $max_clat $max_lat $p99_slat $p99_clat $p99_lat $rw_mix" $iodepth $numjobs >> "$LAT_RESULTS_FILE"
}

<<docs
Runs latency tests based on SNIA specification. Uses fio based on cfg from ./Makefile

@param device - target device (f.e. /dev/lsvbd1) (just log needed)
@param is_raw - shows if the test is aimed for raw(nullb0)/not raw(lsvbd1) device
	is needed for plot scripts and their legends
@param rewrite_mode - shows if warm-up is needed and if the read tests are included
	1 - mode is on (enables warm-up and read tests)
	0 - mode is off
docs
run_tp_tests() {
    local device=$1 rewrite_mode=$2 mode=$3 rw_mix log_file fs_flag extra_args plot_flag rewrite_flag conc_mode
	fs_flag=$([[ $device == "nullb0" ]] && echo "FS=nullb0" || echo "")
	plot_flag=$([[ $device == "nullb0" ]] && echo "--raw")
	rewrite_flag=$([[ $rewrite_mode -eq 1 ]] && echo "--rewrite")
	conc_mode=$([[ $mode == "conc_mode" ]] && echo "--conc_mode")

	echo -e "\n---Starting Throughput operations Benchmark on $device...---\n"

	for rw_mix in "${TP_RW_MIXES[@]}"; do
        echo -e "Running $rw_mix tests on $device\n"
		rwmix_read="${rw_mix%-*}"
		rwmix_write="${rw_mix#*-}"
	
		# only-read tests (100-0) can't be performed without the warm-up
		if [ "$rewrite_mode" == "0" ] && [ "$rwmix_read" == "100" ]; then 
			continue
		fi

        for bs in "${TP_BS_LIST[@]}"; do
			if [ "$rewrite_mode" == "1" ]; then 
				# running warm-up for all the operations in case of rewrite_mode==1 
                workload_independent_preconditioning "$bs"
            fi
			if [ "$mode" == "conc_mode" ]; then
				for nj in "${IOPS_CONC_NJ_LIST[@]}"; do 
					iodepth=$(echo "$nj * 4" | bc)

					for i in $(seq 1 $RUNS); do
						echo "Run $i of $RUNS..."
						log_file="$LOGS_PATH/fio_${rw_mix}_run_${i}.log"
						extra_args=$([[ $rwmix_read == "100" ]] && echo "RBS=$bs" || echo "")

						make fio_perf_mix "$fs_flag" RWMIX_READ="$rwmix_read" RWMIX_WRITE="$rwmix_write" BS="$bs" ID="$IO_DEPTH" NJ="$JOBS_NUM" "$extra_args" > "$log_file"

						extract_tp_metrics "$log_file" "$i" "$bs" "$rw_mix" "$iodepth" "$numjobs"

						reinit_lsvbd
					done
				done
			else 
				for i in $(seq 1 $RUNS); do
					echo "Run $i of $RUNS..."
					log_file="$LOGS_PATH/fio_${rw_mix}_run_${i}.log"
					extra_args=$([[ $rwmix_read == "100" ]] && echo "RBS=$bs" || echo "")

					make fio_perf_mix "$fs_flag" RWMIX_READ="$rwmix_read" RWMIX_WRITE="$rwmix_write" BS="$bs" ID="$IO_DEPTH" NJ="$JOBS_NUM" "$extra_args" > "$log_file"

					extract_tp_metrics "$log_file" "$i" "$bs" "$rw_mix" "$IO_DEPTH" "$JOBS_NUM"

					reinit_lsvbd
				done
			fi
        done

        echo "Data collected in $RESULTS_FILE"
		echo "$plot_flag"
        python3 "$AVG_PLOTS_SCRIPT" $plot_flag $rewrite_flag $conc_mode --tp
        python3 "$HISTOGRAM_PLOTS_SCRIPT" $plot_flag $rewrite_flag
        make clean_logs > /dev/null
    done
}

<<docs
Runs IOPS tests based on SNIA specification. Uses fio cfg from ./Makefile. 

@param device - target device (f.e. /dev/lsvbd1) (just log needed)
@param rewrite_mode - shows if warm-up is needed and if the read tests are included
	1 - mode is on (enables warm-up and read tests)
	0 - mode is off
@param conc_mode - enables the conccurent performance evaluation 
docs
run_iops_tests() {
    local device=$1 rewrite_mode=$2 mode=$3 rw_mix log_file fs_flag extra_args iodepth conc_mode 
	local -a iops_bs_list
    fs_flag=$([[ $device == "nullb0" ]] && echo "FS=nullb0" || echo "")
	plot_flag=$([[ $device == "nullb0" ]] && echo "--raw")
	rewrite_flag=$([[ $rewrite_mode -eq 1 ]] && echo "--rewrite")
	conc_mode=$([[ $mode == "conc_mode" ]] && echo "--conc_mode")

	[[ $mode == "conc_mode" ]] && iops_bs_list=("${IOPS_CONC_BS_LIST[@]}") || iops_bs_list=("${IOPS_BS_LIST[@]}")

	echo -e "---Starting IOPS Benchmark on $device...---\n"

	for rw_mix in "${IOPS_RW_MIXES[@]}"; do
        echo -e "Running $rw_mix tests on $device\n"
		rwmix_read="${rw_mix%-*}"
		rwmix_write="${rw_mix#*-}"
	
		# only-write tests (0-100) can be performed without the warm-up
		if [ "$rewrite_mode" == "0" ] && [ "$rwmix_write" != "100" ]; then 
			continue
		fi

        for bs in "${iops_bs_list[@]}"; do
			if [ "$rewrite_mode" == "1" ]; then 
				# running warm-up for all the operations in case of rewrite_mode==1 
                workload_independent_preconditioning "$bs"
            fi

			if [ "$mode" == "conc_mode" ]; then
				for nj in "${IOPS_CONC_NJ_LIST[@]}"; do 
					iodepth=$(echo "$nj * 4" | bc)

					for i in $(seq 1 $RUNS); do
						echo "Run $i of $RUNS..."
						echo -e "Running with bs = $bs, iodepth = $iodepth and nj = $nj..."
						log_file="$LOGS_PATH/fio_${rw_mix:0:1}_run_${i}.log"
						extra_args=$([[ $rwmix_read == "100" ]] && echo "RBS=$bs" || echo "")
					
						make fio_perf_mix "$fs_flag" RWMIX_READ="$rwmix_read" RWMIX_WRITE="$rwmix_write" BS="$bs" ID="$iodepth" NJ="$nj" > "$log_file"

						extract_iops_metrics "$log_file" "$i" "$bs" "$rw_mix" "$iodepth" "$nj"

						reinit_lsvbd
					done
				done
			else 
				for i in $(seq 1 $RUNS); do
					echo "Run $i of $RUNS..."
					echo -e "Running with bs = $bs, iodepth = $IO_DEPTH and nj = $JOBS_NUM..."
					log_file="$LOGS_PATH/fio_${rw_mix:0:1}_run_${i}.log"
					extra_args=$([[ $rwmix_read == "100" ]] && echo "RBS=$bs" || echo "")
				
					make fio_perf_mix "$fs_flag" RWMIX_READ="$rwmix_read" RWMIX_WRITE="$rwmix_write" BS="$bs" ID="$IO_DEPTH" NJ="$JOBS_NUM" > "$log_file"

					extract_iops_metrics "$log_file" "$i" "$bs" "$rw_mix" "$IO_DEPTH" "$JOBS_NUM"

					reinit_lsvbd
				done
			fi

			
        done

        echo "Data collected in $RESULTS_FILE"
        python3 "$AVG_PLOTS_SCRIPT" $plot_flag $rewrite_flag $conc_mode
        python3 "$HISTOGRAM_PLOTS_SCRIPT" $plot_flag $rewrite_flag
        make clean_logs > /dev/null
    done
}

<<docs
Runs latency tests based on SNIA specification.

@param device - target device (f.e. /dev/lsvbd1) (just log needed)
@param is_raw - shows if the test is aimed for raw(nullb0)/not raw(lsvbd1) device
	is needed for plot scripts and their legends
@param rewrite_mode - shows if warm-up is needed and if the read tests are included
	1 - mode is on (enables warm-up and read tests)
	0 - mode is off
@param conc_mode - enables the conccurent performance evaluation 
docs
run_latency_tests() {
    local device=$1 is_raw=$2 rewrite_mode=$2 mode=$3 bs rw_mix log_file
	plot_flag=$([[ $device == "nullb0" ]] && echo "--raw")
	rewrite_flag=$([[ $rewrite_mode -eq 1 ]] && echo "--rewrite")
	conc_mode=$([[ $mode == "conc_mode" ]] && echo "--conc_mode")
    fs_flag=$([[ $device == "nullb0" ]] && echo "FS=nullb0" || echo "")

    echo -e "---Starting SNIA-complied Latency Benchmark on $device...---\n"
    for rw_mix in "${LAT_RW_MIXES[@]}"; do
		rwmix_read="${rw_mix%-*}"
		rwmix_write="${rw_mix#*-}"
		
		# only-write tests (0-100) can be performed without the warm-up
		if [ "$rewrite_mode" == "0" ] && [ "$rw_mix" != "0-100" ]; then 
			continue
		fi

        for bs in "${LAT_BS_LIST[@]}"; do
			if [ "$rewrite_mode" == "1" ]; then 
	            echo -e "Performing a block device warm-up..."
				# running warm-up for all the operations in case of rewrite_mode==1
				workload_independent_preconditioning "$bs";
			fi
			if [ "$mode" == "conc_mode" ]; then
				for nj in "${IOPS_CONC_NJ_LIST[@]}"; do 
					iodepth=$(echo "$nj * 4" | bc)

					for i in $(seq 1 $RUNS); do
						echo "Run $i of $RUNS..."
						log_file="$LOGS_PATH/latency_${bs}_${rw_mix}"
						make fio_lat_mix "$fs_flag" RWMIX_READ="$rwmix_read" RWMIX_WRITE="$rwmix_write" BS="$bs" ID="$iodepth" NJ="$nj" LOG_FILE_PATH="$log_file" > /dev/null
						extract_latency_metrics "$i" "$log_file" "$bs" "$rw_mix" "$nj" "$iodepth"

						reinit_lsvbd
					done
				done
			else
				for i in $(seq 1 $RUNS); do
					echo "Run $i of $RUNS..."
					log_file="$LOGS_PATH/latency_${bs}_${rw_mix}"
					make fio_lat_mix "$fs_flag" RWMIX_READ="$rwmix_read" RWMIX_WRITE="$rwmix_write" BS="$bs" ID="1" NJ="1" LOG_FILE_PATH="$log_file" > /dev/null
					extract_latency_metrics "$i" "$log_file" "$bs" "$rw_mix"

					reinit_lsvbd
				done
			fi
        done
    done

	python3 "$LATENCY_PLOTS_SCRIPT" $plot_flag $rewrite_flag $conc_mode 
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


### BASIC TEST-SUITES FOR PERFORMANCE EVALUATION ### 

### Non-rewrite mode (only write operations are tested)

# Run tests for LSVBD
run_tp_tests "lsvbd1" 0
run_iops_tests "lsvbd1" 0
run_latency_tests "lsvbd1" 0

# Run tests for NULLDISK (raw mode)
run_tp_tests "nullb0" 0
run_iops_tests "nullb0" 0
run_latency_tests "nullb0" 0

### Rewrite mode test (warm-up included)

run_tp_tests "lsvbd1" 1
run_iops_tests "lsvbd1" 1
run_latency_tests "lsvbd1" 1

# Run tests for NULLDISK (raw mode)
run_tp_tests "nullb0" 1 
run_iops_tests "nullb0" 1
run_latency_tests "nullb0" 1

### Running paralllelism tests (rewrite mode)

run_iops_tests "nullb0" 1 "conc_mode" 
run_iops_tests "nullb0" 0 "conc_mode" 
run_iops_tests "lsvbd1" 1 "conc_mode"
run_iops_tests "lsvbd1" 0 "conc_mode"

run_latency_tests "lsvbd1" 1 "conc_mode"
run_latency_tests "lsvbd1" 0 "conc_mode"
run_latency_tests "nullb0" 1 "conc_mode"
run_latency_tests "nullb0" 0 "conc_mode"

echo "Histograms, AVG plots, and statistics saved in $PLOTS_PATH"

echo -e "\nCleaning the logs directory"
make clean > /dev/null
