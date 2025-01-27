# LS-BDD
LS-BDD is a block device driver that implements log-structured storage based on B+-tree, RB-tree, Skiplist and Hashtable data structures.
Driver is based on BIO request management and supports BIO split.

For more info - see [presentation v1](https://github.com/qrutyy/ls-bdd/blob/main/docs/LogStructuredStoringBasedOnB+Tree.pdf)

***Compatable with Linux Kernel 6.8***

## Usage
Highly recommended to test/use the driver using a VM, to prevent data coruption.

### Initialisation:
```bash
make
insmod lsbdd.ko
echo "ds_name" > /sys/module/lsbdd/parameters/set_data_structure
echo "index path" > /sys/module/lsbdd/parameters/set_redirect_bd
```
**ds_name** - one of available data structures to store the mapping ("bt", "ht", "sl", "rb")
**index** - postfix for a 'device in the middle' (prefix is 'lsvbd'), **path** - to which block device to redirect

*All this steps can be reduced to `make init`*

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

## License

Distributed under the [GPL-2.0 License](https://github.com/qrutyy/ls-bdd/blob/main/LICENSE). 
