CONFIG_MODULE_SIG=n
name = lsbdd

# Delete Block device Index
DBI?=1
#Data Structure name (bt, ht, sl, rb. For more info - see README)
DS?=bt
# Read operation block size in KB(2, 4, 8...)
RBS?=4
# Write operation block size in KB(2, 4, 8...)
WBS?=4
# Read operation type (read/randread)
RO?=read
# Write operation type (write/randwrite)
WO?=write
# File size (in MB)
FS?=5000
# IO engine to use in FIO (libaio\io_uring)
IO?=libaio 
# Number of fio jobs to run
NJ?=1
# IO depth num
ID?=1
# Type of data-structures to be used in the driver (lf/sy) 
TY?=lf
# End block device name
BD?=ram0
# RamDisk sizes
RD_SIZE_GB?=5
RD_SIZE_B := $(shell echo "$(RD_SIZE_GB) * 1048576" | bc)

# To build modules outside of the kernel tree, we run "make"
# in the kernel source tree; the Makefile these then includes this
# Makefile once again.
# This conditional selects whether we are being included from the
# kernel Makefile or not.

ifeq ($(KERNELRELEASE),)

    # Assume the source tree is where the running kernel was built
    # You should set KERNELDIR in the environment if it's elsewhere
    KERNELDIR ?= /lib/modules/$(shell uname -r)/build
    # The current directory is passed to sub-makes as argument
    PWD := $(shell pwd)

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

ins:
	insmod $(name).ko

set:
	echo -n "1 /dev/$(BD)" > /sys/module/$(name)/parameters/set_redirect_bd
#	**index** - postfix for a 'device in the middle' (prefix is 'lsvbd'), **bd** - to which block device to redirect

ramd:
	modprobe brd rd_nr=1 rd_size=$(RD_SIZE_B)

lint:
	find . -name "*.c" -o -name "*.h" | xargs ./checkpatch.pl -f --no-tree

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions *.symvers *.mod *.order *.o.d *.state
	rm -rf utils/lock-free/*.o utils/lock-free/.*.cmd utils/lock-free/.*.o.d
	rm -rf utils/sync/*.o utils/sync/.*.cmd utils/sync/.*.o.d
	rm -rf *.out *.folded *.perf *.old *.svg *.data *.dat *.dump
	make clean -C ../test/

exit:
	echo "$(DBI)" > /sys/module/lsbdd/parameters/delete_bd
	rmmod lsbdd.ko

init:
	make type="$(TY)"
	make ins
	echo "$(DS)" > /sys/module/lsbdd/parameters/set_data_structure
	make set

init_no_recompile:
	make ins
	echo "$(DS)" > /sys/module/lsbdd/parameters/set_data_structure
	make set

fio_verify:
	# Write and verify test
	fio --name=test_verify --ioengine=$(IO) --iodepth=$(ID) --rw=$(WO) --size=$(FS)M --verify_state_save=1 --bssplit=$(WBS)k/100 --direct=1 --filename=/dev/lsvbd1  --verify=pattern --verify_pattern=0xAA --do_verify=0 --verify_fatal=0 --verify_only=0 --numjobs=$(NJ) --serialize_overlap=1 --offset=5%

	# Read and verify test 
	fio --name=test_verify --ioengine=$(IO) --iodepth=$(ID) --rw=$(RO) --size=$(FS)M --verify_state_save=1 --bssplit=$(RBS)k/100 --direct=1 --filename=/dev/lsvbd1  --verify=pattern --verify_pattern=0xAA --do_verify=0 --verify_fatal=0 --verify_only=1 --numjobs=$(NJ) --serialize_overlap=1 --offset=5%

.PHONY: modules modules_install clean

endif

