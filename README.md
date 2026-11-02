# LS-BDD

**LS-BDD** is a generic software-defined-storage block device driver made for research.
Although many concepts ins SDS are well known -- its further research can be usefull. In particulary bc of narrow targeted development.

It implements such approaches as:

- log-structured storage;
- deduplication;
- garbage collection (linear/segmented).

The current version of the driver includes several already implemented data structures in both **lock-free** and **synchronous** versions: **B+-tree**, **RB-tree**, **Skiplist**, and **Hashtable**.
The driver is based on bio request management.

For related works, see [docs](https://github.com/qrutyy/ls-bdd/blob/main/docs):

**_Compatible with Linux Kernel 7.2.9_**

### Initialization

```bash
make init
```

Devices are configured through configfs:

```bash
mkdir /sys/kernel/config/lsv/lsv0
echo /dev/ram0 > /sys/kernel/config/lsv/lsv0/backing_path
echo ht        > /sys/kernel/config/lsv/lsv0/index_ds
echo 4096      > /sys/kernel/config/lsv/lsv0/cell_size
echo "linear"  > /sys/kernel/config/lsv/lsv0/space_layout
echo 8192      > /sys/kernel/config/lsv/lsv0/segment_size
echo 1         > /sys/kernel/config/lsv/lsv0/dedup
echo 1         > /sys/kernel/config/lsv/lsv0/create_new
```

`make destroy NAME=lsv0` removes one device, `make exit` removes all of them and unloads the module.

### Testing

You can use the provided FIO tests (or write your own) to measure execution time and perform pattern verification:

```bash
make fio_verify IO=io_uring FS=lsv0 WBS=8 RBS=8 NJ=8 ID=8 SIZE=1000
```

A description of all options is provided in the `Makefile`.

For more customizable FIO testing, check the `test/fio/` directory for predefined configurations.

## Performance Evaluation

For performance measurements, you can use `test/main.sh`.
A detailed overview of the testing system is provided [here](https://github.com/qrutyy/ls-bdd/blob/main/test/README.md).

## License

Distributed under the [GPL-2.0 License](https://github.com/qrutyy/ls-bdd/blob/main/LICENSE).
