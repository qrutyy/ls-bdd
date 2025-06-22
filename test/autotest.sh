#!/bin/bash

JOBS_NUM=8
IO_DEPTH=32
NBD_SIZE=120
DAST="sl"
TYPE="lf"
VBDEVICE="lsvbd1"
BS_LIST=("4K" "8K" "16K" "32K" "64K" "128K") #
JOB_SIZE="100M"

usage() {
    echo -e "Automatic fio-test script.\nIncludes suites with different block sizes and is customizable. \n\nSHOULDN'T BE USED WITH NULL-DISK. THE VERIFICATION WILL MAKE NO SENSE.\n\nUsage: $0 [--io_depth number] [--jobs_num number]"
    exit 1
}

# Parse options
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --io_depth) IO_DEPTH="$2"; shift ;;
        --jobs_num) JOBS_NUM="$2"; shift ;;
        --brd_size) NBD_SIZE="$2"; shift ;;
		--vb_device) VBDEVICE="$2"; shift ;;
        -h|--help) echo "Usage: $0 [--io_depth number] [--jobs_num number]"; exit 1 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
    shift
done

# Reinits the lsbdd and null_blk modules
reinit_lsvbd() {
	make -C ../src exit DBI=1 > /dev/null

	sync; echo 3 | sudo tee /proc/sys/vm/drop_caches

	modprobe -r null_blk
	modprobe null_blk queue_mode=0 gb="$NBD_SIZE" bs=512 irqmode=0 nr_devices=1
	make -C ../src init_no_recompile DS=${DAST} TY=${TYPE} > /dev/null
}

for rbs in "${BS_LIST[@]}"; do
	for wbs in "${BS_LIST[@]}"; do
		make fio_verify WBS="$wbs" RBS="$rbs" SIZE="$JOB_SIZE" NJ="$JOBS_NUM" ID="$IO_DEPTH" FS="$VBDEVICE" IO="io_uring"
		reinit_lsvbd
	done 
done 


