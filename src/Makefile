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
FS?=1000

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
	echo -n "1 /dev/vdb" > /sys/module/$(name)/parameters/set_redirect_bd

lint:
	find . -name "*.c" -o -name "*.h" | xargs ./checkpatch.pl -f --no-tree

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions *.symvers *.mod *.order *.o.d
	rm -rf include/*.o include/.*.cmd include/.*.o.d

exit:
	echo "$(DBI)" > /sys/module/lsbdd/parameters/delete_bd
	rmmod lsbdd.ko

init:
	make
	make ins
	echo "$(DS)" > /sys/module/lsbdd/parameters/set_data_structure
	make set

init_no_recompile:
	make ins
	echo "$(DS)" > /sys/module/lsbdd/parameters/set_data_structure
	make set

fio_verify:
	# Write and verify test
	fio --name=test_verify --ioengine=libaio --iodepth=16 --rw=$(WO) --size=$(FS)M --verify_state_save=1 --bssplit=$(WBS)k/100 --direct=1 --filename=/dev/lsvbd1 --numjobs=1 --verify=pattern --verify_pattern=0xAA --do_verify=0 --verify_fatal=0 --verify_only=0

	# Read and verify test
	fio --name=test_verify --ioengine=libaio --iodepth=16 --rw=$(RO) --size=$(FS)M --verify_state_save=1 --bssplit=$(RBS)k/100 --direct=1 --filename=/dev/lsvbd1 --numjobs=1 --verify=pattern --verify_pattern=0xAA --do_verify=0 --verify_fatal=0 --verify_only=1

.PHONY: modules modules_install clean

endif

