RBS?=8
# Write operation block size in KB(2, 4, 8...)
WBS?=8
# Read operation type (read/randread)
RO?=randread
# Write operation type (write/randwrite)
WO?=randwrite
# File size (in MB)
FS?=400000
# IO engine to use in FIO (libaio\io_uring)
IO?=io_uring
# Number of fio jobs to run
NJ?=8
# IO depth num
ID?=32
# INDEX
IN?=""
# Fio targets filename
FS="lsvbd1"
# Ratio of read opertations in the mix workflow
RWMIX_READ?=50
# Ratio of write operations in the mix workflow
RWMIX_WRITE?=50
# Log file path (pretty intuitive)
LOG_FILE_PATH?=logs/latency_idk

RUNTIME=5

RAMPTIME=3

RW_TYPE="randrw"


fio_perf_template:
	fio --name=test --ioengine=$(IO) --iodepth=$(ID) --rw=$(MODE) --bssplit=$(BS)k/100 --direct=1 --filename=/dev/$(FS) --numjobs=$(NJ) --write_bw_log=logs/write$(IN) --write_lat_log=logs/write$(IN) --write_iops_log=logs/write$(IN) --per_job_logs=0 $(EXTRA_OPTS) --offset=16k

fio_perf_r_clean:
	$(MAKE) fio_perf_template MODE=randread BS=$(WBS) 

fio_perf_w_clean:
	$(MAKE) fio_perf_template MODE=randwrite BS=$(WBS) 

fio_perf:
	# Write test
	make fio_perf_w_clean

	# Read and test 
	make fio_perf_r_clean

fio_perf_w_opt:
	$(MAKE) fio_perf_template MODE=randwrite BS=$(WBS) EXTRA_OPTS="--time_based --runtime=10 --cmdprio_percentage=100 --registerfiles=1 --fixedbufs=1 --hipri=0  --cpus_allowed_policy=shared --prio=0 "

fio_perf_r_opt:
	$(MAKE) fio_perf_template MODE=randread BS=$(RBS) EXTRA_OPTS="--time_based --runtime=10 --cmdprio_percentage=100 --registerfiles=1 --fixedbufs=1 --hipri=0  --cpus_allowed_policy=shared --prio=0"

fio_perf_mix:
	fio --name=mix_test \
		--rw=$(RW_TYPE) \
		--rwmixread=$(RWMIX_READ) \
		--rwmixwrite=$(RWMIX_WRITE) \
		--bs=$(BS)k \
		--numjobs=$(NJ) \
		--iodepth=$(ID) \
		--time_based \
		--runtime=$(RUNTIME) \
		--ramp_time=$(RAMPTIME) \
		--direct=1 \
		--ioengine=$(IO) \
		--registerfiles=1 \
		--hipri=0 \
		--fixedbufs=1 \
		--prio=0 \
		--norandommap=0 \
		--cpus_allowed_policy=shared \
		--filename=/dev/$(FS) \
		--cpus_allowed=0-7 \
		--offset=16k

fio_lat_mix:
	fio --name=latency_test \
	--rw=$(RW_TYPE) \
	--rwmixread=$(RWMIX_READ) \
	--rwmixwrite=$(RWMIX_WRITE) \
	--bs=$(BS)k \
	--numjobs=1 \ 
	--iodepth=$(ID) \
	--time_based \
	--runtime=$(RUNTIME) \
	--ramp_time=$(RAMPTIME) \
	--direct=1 \						
	--write_lat_log=$(LOG_FILE_PATH) \
	--ioengine=$(IO) \
	--registerfiles=1 \
	--hipri=0 \
	--prio=0 \
	--cpus_allowed=0 \
	--fixedbufs=1 
	--filename=/dev/$(FS) \
	--norandommap=0 \
	--offset=16k

fio_verify:
	fio --name=verify \
	--ioengine=$(IO) \
	--iodepth=$(ID) \
	--rw=randwrite \
	--size=$(SIZE) \
	--verify_state_save=1 \
	--bssplit=$(WBS)/100 \
	--direct=1 \
	--filename=/dev/$(FS) \
	--numjobs=$(NJ) \
	--verify=pattern \
	--verify_pattern=0xAA \
	--do_verify=0 \
	--verify_fatal=0 \
	--verify_only=0 \
	--offset=16k 
	
	fio --name=verify \
	--ioengine=$(IO) \
	--iodepth=$(ID) \
	--rw=randread \
	--size=$(SIZE) \
	--verify_state_save=1 \
	--bssplit=$(RBS)/100 \
	--direct=1 \
	--filename=/dev/$(FS) \
	--numjobs=$(NJ) \
	--verify=pattern \
	--verify_pattern=0xAA \
	--do_verify=0 \
	--verify_fatal=0 \
	--verify_only=1 \
	--offset=16k 

clean:
	rm -rf *.svg *.data *.folded *.old *.state *.log *.dat
	make clean_logs

clean_logs:
	rm -rf logs/*_log logs/*.log  logs/*.png logs/logs logs/plots logs/*.dat

clean_plots:
	rm -rf plots/* 

.PHONY: all clean test

