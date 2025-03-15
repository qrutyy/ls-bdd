RBS?=8
# Write operation block size in KB(2, 4, 8...)
WBS?=8
# Read operation type (read/randread)
RO?=randread
# Write operation type (write/randwrite)
WO?=randwrite
# File size (in MB)
FS?=1000
# IO engine to use in FIO (libaio\io_uring)
IO?=io_uring 
# Number of fio jobs to run
NJ?=1
# IO depth num
ID?=1
# INDEX
IN?=""

fio_perf_template:
	fio --name=test_verify --ioengine=$(IO) --iodepth=$(ID) --rw=$(MODE) --bssplit=$(BS)k/100 --direct=1 --filename=/dev/lsvbd1 --numjobs=$(NJ) --write_bw_log=logs/write$(IN) --write_lat_log=logs/write$(IN) --write_iops_log=logs/write$(IN) --per_job_logs=0 $(EXTRA_OPTS)

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
	$(MAKE) fio_perf_template MODE=randwrite BS=$(WBS) EXTRA_OPTS="--time_based --runtime=2 --cmdprio_percentage=100 --registerfiles=1 --fixedbufs=0 --hipri=0  --cpus_allowed_policy=shared --prio=0"

fio_perf_r_opt:
	$(MAKE) fio_perf_template MODE=randread BS=$(RBS) EXTRA_OPTS="--size=$(FS)M --cmdprio_percentage=100 --registerfiles=1 --fixedbufs=0 --hipri=0  --cpus_allowed_policy=shared --prio=0"

fio_perf_wr_opt:
	$(MAKE) fio_perf_template MODE=randwrite BS=$(WBS) EXTRA_OPTS="--size=$(FS)M --cmdprio_percentage=100 --registerfiles=1 --hipri=0"
	
	$(MAKE) fio_perf_template MODE=randread BS=$(RBS) EXTRA_OPTS="--size=$(FS)M --cmdprio_percentage=100 --registerfiles=1 --hipri=0"

clean:
	rm -rf *.svg *.data *.folded *.old *.state *.log *.dat
	make clean_logs
	make clean_plots

clean_logs:
	rm -rf logs/*_log logs/*.log  logs/*.png logs/logs logs/plots logs/*.dat

clean_plots:
	rm -rf plots/*/*.png plots/*.png plots/*.txt plots/bw plots/lat plots/iops plots/clat plots/slat plots/bw_iops

