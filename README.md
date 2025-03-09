# LS-BDD
LS-BDD is a block device driver that implements log-structured storage based on B+-tree, RB-tree, Skiplist and Hashtable data structures.
Driver is based on BIO request management and supports BIO split.

For more info - see [ru-presentation v1](https://github.com/qrutyy/ls-bdd/blob/main/docs/3-semester/(ru-presentation)%20Implementation%20of%20log-structured%20block%20device%20in%20Linux%20kernel.pdf) [eng-presentation v1](https://github.com/qrutyy/ls-bdd/blob/main/docs/3-semester/(eng-presentation)%20Implementation%20of%20log-structured%20block%20device%20in%20Linux%20kernel.pdf)

***Compatable with Linux Kernel 6.8***

## Usage
Highly recommended to test/use the driver using a VM, to prevent data coruption.

### Initialisation:
```bash
make init DS="ds_name" TY="io_type" BD="bd_name" 
```
**ds_name** - one of available data structures to store the mapping ("bt", "ht", "sl", "rb")
**io_type** - block device mode ("lf" - lock-free, "sy" - sync)
**bd_name** - terminal block device (f.e. "vdb", "sdc", ...)

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

### Testing
After making some changes you can check a lot of obvious cases using auto-tests:
```
python3 ../test/autotest.py -vbd="lsvbd1" -n=5 -fs=-1 -bs=0 -m=seq
```
In addition you can use the provided fio tests, that time the execution and use pattern-verify process.
```
make fio_verify IO=libaio WO=randwrite RO=randread FS=1000 WBS=8 RBS=8
```
Options description is provided in `Makefile`.

Although, if you need more customizable fio testing - you can check `test/fio/` for more predefined configs. Run them by:
```
fio test/fio/_
```
*Basic test - "ftv_4_8/ftv_8_4"*
Also including the *.sh* versions (better use them for this moment)

### Advanced testing
In case of performance measuring `main.sh` can be used. For example:
```
./test/main.sh -s -c --io_depth 16 --jobs_num 4 --bd_name vbd
```
It is able to run fio tests with pattern verification, plot generation (fio2gnuplot), flamegraph generation and system-side optimisation. For more information about modes usage - see source code or run `./test/main.sh -h`. 

## License

Distributed under the [GPL-2.0 License](https://github.com/qrutyy/ls-bdd/blob/main/LICENSE). 
