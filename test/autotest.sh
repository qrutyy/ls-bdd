#!/bin/bash

source ./configurable_params.sh

usage() {
	echo -e "Automatic fio-test script.\nIncludes suites with different block sizes. \n\nSHOULDN'T BE USED WITH NULL-DISK. THE VERIFICATION WILL MAKE NO SENSE.\n\nUsage: $0 "
    exit 1
}

# Reinits the lsbdd and null_blk modules
reinit_lsvbd() {
	make -C ../src exit DBI=1 > /dev/null

	sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
	# paste the reinition of the module
	modprobe brd rd_nr=1 rd_size=$(("$NBD_SIZE" * 1048576))
	make -C ../src init_no_recompile DS="${BD_DS}" TY="${BD_TYPE}" > /dev/null
}

if [ "$BS_MIX_MODE" -ne 1 ]; then 
	for bs in "${BS_LIST[@]}"; do
		make fio_verify WBS="$bs" RBS="$bs" SIZE="$JOB_SIZE" NJ="$JOBS_NUM" ID="$IO_DEPTH" FS="$VBD_NAME" IO="io_uring"
		reinit_lsvbd
	done 
else 
	for rbs in "${BS_LIST[@]}"; do
		for wbs in "${BS_LIST[@]}"; do
			make fio_verify WBS="$wbs" RBS="$rbs" SIZE="$JOB_SIZE" NJ="$JOBS_NUM" ID="$IO_DEPTH" FS="$VBD_NAME" IO="io_uring"
			reinit_lsvbd
		done 
	done 
fi


