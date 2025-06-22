# LS-BDD
LS-BDD is a block device driver that implements log-structured storage based on sync and lock-free versions of B+-tree, RB-tree, Skiplist, and Hashtable data structures. The log-structured approach is designed to speed up reading from block devices by transforming random requests into sequential ones. The efficiency and behavior of different data structures are being examined.

The driver is based on BIO request management and supports BIO splitting (i.e. different operation block sizes, e.g. 4KB write, 16 KB read). At the moment, a multithreaded lock-free implementation is in development.

For more info - see [ru-presentation v1](https://github.com/qrutyy/ls-bdd/blob/main/docs/3-semester/(ru-presentation)%20Implementation%20of%20log-structured%20block%20device%20in%20Linux%20kernel.pdf) [eng-presentation v1](https://github.com/qrutyy/ls-bdd/blob/main/docs/3-semester/(eng-presentation)%20Implementation%20of%20log-structured%20block%20device%20in%20Linux%20kernel.pdf)

***Compatable with Linux Kernel 6.8***

## Usage
Highly recommended to test/use the driver using a VM, to prevent data coruption.

### Initialisation:
```bash
make init DS="ds_name" TY="io_type" BD="bd_name" 
```
- **ds_name** - one of available data structures to store the mapping ("bt", "ht", "sl", "rb")
- **io_type** - block device mode ("lf" - lock-free, "sy" - sync)
- **bd_name** - terminal block device (f.e. "ram0" "vdb", "sdc", ...)

### Sending requests: 

**Initialisation example:**
```bash
...
echo "1 /dev/vdb" > /sys/module/lsbdd/parameters/set_redirect_bd
cat /sys/module/lsbdd/parameters/get_bd_names // to get the links
```
#### Writing
```
dd if=/dev/urandom of=/dev/lsvbd1 oflag=direct bs=2K count=10;
```
#### Reading
```
dd of=test2.txt if=/dev/lsvbd1 iflag=direct bs=4K count=10; 
```

## Testing
You can use the provided fio tests (or write your own), that time the execution and use pattern-verify process.
```
make fio_verify IO=libaio WO=randwrite RO=randread FS=1000 WBS=8 RBS=8
```
Option description is provided in `Makefile`.

Although, if you need more customizable fio testing - you can check `test/fio/` for more predefined configs. 

### Performance evaluation

In case of performance measuring `test/main.sh` can be used. For example:
```
make nulld
make init DS=sl TY=lf BD=nullb0
cd ../test && ./main.sh -s -c --io_depth 16 --jobs_num 4 --bd_name nullb0
```
It is able to run fio tests with pattern verification, plot generation, flamegraph generation and system-side optimisation. For more information about mode usage - see source code or run `./test/main.sh -h`. 

More detailed overview of test-system is presented [here](https://github.com/qrutyy/ls-bdd/blob/main/test/README.md)

## License

Distributed under the [GPL-2.0 License](https://github.com/qrutyy/ls-bdd/blob/main/LICENSE).
