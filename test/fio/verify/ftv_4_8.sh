#!/bin/bash

# Write test
fio --name=fff --ioengine=io_uring --iodepth=64 --rw=randwrite --size=500M --verify_state_save=1 --bssplit=8k/100 --direct=1 --filename=/dev/lsvbd1 --numjobs=4 --verify=pattern --verify_pattern=0xAA --do_verify=0 --verify_fatal=0 --verify_only=0 --offset=16k

# Read and verify test
fio --name=fff --ioengine=io_uring --iodepth=64 --rw=randread --size=500M --verify_state_save=1 --bssplit=8k/100 --direct=1 --filename=/dev/lsvbd1 --numjobs=4 --verify=pattern --verify_pattern=0xAA --do_verify=0 --verify_fatal=0 --verify_only=1 --offset=16k
