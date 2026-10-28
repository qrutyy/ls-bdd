CONFIG_MODULE_SIG=n
name=lsv

# Name of the lsv device, also its configfs directory (/dev/$(NAME))
NAME?=lsv0
# Mapping cell size in bytes (power of two, 512..1M)
CS?=4096
# Inline deduplication (0/1)
DEDUP?=0
# configfs directory of the module
CFS?=/sys/kernel/config/lsv
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
FS?=10000
# IO engine to use in FIO (libaio\io_uring)
IO?=libaio 
# Number of fio jobs to run
NJ?=1
# IO depth num
ID?=1
# Type of data-structures to be used in the driver (lf/sy) 
TY?=lf
# End block device name
BD?=nullb0
# NULL Disk sizes
ND_SIZE_GB?=400

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

build:
	make modules

# Runs the KUnit suites built into lsv.ko. KUnit is a module in Fedora
# (kernel-modules-internal) and disabled by default, hence enable=1.
kunit:
	make build type="$(TY)"
	modprobe kunit enable=1
	-rmmod $(name) 2>/dev/null
	insmod $(name).ko
	cat /sys/kernel/debug/kunit/*/results

modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

# Devices are described in configfs, so it has to be mounted before insmod.
ins:
	mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config
	insmod $(name).ko

# Creates /dev/$(NAME) on top of /dev/$(BD), see ctl/configfs.c for the protocol.
create:
	mkdir $(CFS)/$(NAME)
	echo /dev/$(BD) > $(CFS)/$(NAME)/backing_path
	echo $(DS) > $(CFS)/$(NAME)/index_ds
	echo $(CS) > $(CFS)/$(NAME)/cell_size && echo $(DEDUP) > $(CFS)/$(NAME)/dedup
	echo 1 > $(CFS)/$(NAME)/create_new

destroy:
	rmdir $(CFS)/$(NAME)

nulld:
	modprobe null_blk gb=$(ND_SIZE_GB) bs=512 irqmode=0 nr_devices=1

lint:
	find . -name "*.c" -o -name "*.h" | xargs ./checkpatch.pl -f --no-tree

clean:
	rm -rf core/*.o core/bio/*.o core/dedup/*.o core/lmap/*.o
	rm -rf utils/*.o utils/.*.cmd utils/.*.o.d utils/*/*.o utils/*/.*.cmd utils/*/.*.o.d
	find utils -name "*.o" -o -name ".*.cmd" -o -name ".*.o.d" | xargs rm -f
	rm -rf *.out *.folded *.perf *.old *.svg *.data *.dat *.dump
	make clean -C ../test/

# Every device holds a reference on the module, so they go first.
exit:
	find $(CFS) -mindepth 1 -maxdepth 1 -type d -exec rmdir {} +
	rmmod $(name)

init:
	make build type="$(TY)"
	make ins
	make create

init_no_recompile:
	make ins
	make create

.PHONY: all modules modules_install build clean test nulld kunit ins create destroy exit init init_no_recompile

endif

