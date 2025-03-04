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

all:
	clean

fio_perf:
	# Write test
	fio --name=test_verify --ioengine=$(IO) --iodepth=$(ID) --rw=$(WO) --size=500M  --bssplit=$(WBS)k/100 --direct=1 --filename=/dev/lsvbd1 --numjobs=$(NJ) --serialize_overlap=1 

	# Read and verify test 
	fio --name=test_verify --ioengine=$(IO) --iodepth=$(ID) --rw=$(RO) --size=500M --bssplit=$(RBS)k/100 --direct=1 --filename=/dev/lsvbd1 --numjobs=$(NJ) --serialize_overlap=1

fio_perf_r:
	fio --name=test_verify --ioengine=$(IO) --iodepth=$(ID) --rw=$(WO) --size=500M --bssplit=$(WBS)k/100 --direct=1 --filename=/dev/lsvbd1 --numjobs=$(NJ) --serialize_overlap=1 

fio_perf_w:
	fio --name=test_verify --ioengine=$(IO) --iodepth=$(ID) --rw=$(RO) --size=500M --bssplit=$(RBS)k/100 --direct=1 --filename=/dev/lsvbd1 --numjobs=$(NJ) --serialize_overlap=1


clean:
	rm -rf *.svg *.data *.folded *.old
