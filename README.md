# LS-BDD

**LS-BDD** is a testing framework for block devices that implement log-structured (LS) storage.
The log-structured approach is designed to accelerate certain read workflows by transforming random requests into sequential ones.
Although the log-structured concept is well known, the details of its implementation remain insufficiently studied.
This framework explores the efficiency and behavior of various data structures used in LS-based systems.

The framework provides a modifiable block device driver in which the LS underlying data structures can be added or modified.
The current version of the driver includes several already implemented data structures in both **lock-free** and **synchronous** versions: **B+-tree**, **RB-tree**, **Skiplist**, and **Hashtable**.

The driver is based on BIO request management and supports BIO splitting (i.e., different block sizes for operations, such as 4 KB writes and 16 KB reads).
A multithreaded lock-free implementation is currently under development.

For more information, see:

* [ru-presentation 2025](https://github.com/qrutyy/ls-bdd/blob/main/docs/4-semester/%28ru-presentation-conf%29%20Research%20on%20the%20implementation%20of%20log-structured%20block%20devicesd%20in%20Linux%20kernel.pdf)
* [eng-presentation (LS implementation)](https://github.com/qrutyy/ls-bdd/blob/main/docs/3-semester/%28eng-presentation%29%20Implementation%20of%20log-structured%20block%20device%20in%20Linux%20kernel.pdf)

***Compatible with Linux Kernel 7.2.9***

## Block Device Driver

It is highly recommended to test the driver in a virtual machine to prevent data corruption.

### Adding Your Data Structure

The LBA-to-PBA mapping is kept in an index chosen per device. Every index is a
`struct lsv_ds_ops` in the registry of `src/utils/ds_control.c`; configfs, the
mapping layer, dedup and the KUnit suites find it there by name.

1. Place your implementation in **`src/utils/<mode>`**, where **`<mode>`** is the
   parallelism model of your data structure (*sync* or *lock-free*), and add its
   object to **`lsv-objs`** in **`src/Kbuild`**.
2. Fill a `struct lsv_ds_ops` with a name of at most two letters, your functions and
   the `caps` that describe them, and append it to `lsv_ds_registry[]`. The interface
   is described in [src/README.md](src/README.md).
3. Build with KUnit (`make kunit`): the `lsv_ds` suite runs the index contract against
   it and `lsv_ds_bench` reports its ns/op next to the others.
4. Create a device with `index_ds` set to your name.

### Initialization

```bash
make init DS="ds_name" TY="io_type" BD="bd_name" [NAME="lsv0"] [CS=4096] [DEDUP=0]
```

* **`ds_name`** – one of the available data structures used for mapping (`bt`, `ht`, `sl`, `rb`, or your custom one)
* **`io_type`** – block device mode (`lf` – lock-free, `sy` – synchronous)
* **`bd_name`** – backing block device (e.g., `ram0`, `vdb`, `sdc`)
* **`NAME`** – name of the created device, it appears as `/dev/NAME`
* **`CS`** – mapping cell size in bytes, **`DEDUP`** – inline deduplication

Devices are configured through configfs, `make init` is a shortcut for:

```bash
insmod src/lsv.ko
mkdir /sys/kernel/config/lsv/lsv0
echo /dev/ram0 > /sys/kernel/config/lsv/lsv0/backing_path
echo ht        > /sys/kernel/config/lsv/lsv0/index_ds
echo 4096      > /sys/kernel/config/lsv/lsv0/cell_size
echo 1         > /sys/kernel/config/lsv/lsv0/create_new
```

`make destroy NAME=lsv0` removes one device, `make exit` removes all of them and unloads the module.

### Sending Requests

#### Writing

```bash
dd if=/dev/urandom of=/dev/lsv0 oflag=direct bs=2K count=10
```

#### Reading

```bash
dd of=test2.txt if=/dev/lsv0 iflag=direct bs=4K count=10
```

### Testing

You can use the provided FIO tests (or write your own) to measure execution time and perform pattern verification:

```bash
make fio_verify IO=io_uring FS=lsv0 WBS=8 RBS=8 NJ=8 ID=8 SIZE=1000
```

A description of all options is provided in the `Makefile`.

For more customizable FIO testing, check the `test/fio/` directory for predefined configurations.

## Driver Stats and Tracing

The driver measures its own stages, the part of the latency fio cannot see.
Every device gets a debugfs directory:

```bash
cat /sys/kernel/debug/lsv/lsv0/stats   # counters, gauges, per-stage latency summary
cat /sys/kernel/debug/lsv/lsv0/hist    # raw latency histograms: stage floor_ns count
echo 1 > /sys/kernel/debug/lsv/lsv0/reset
```

Stages are `bio` (whole request, submission to completion), `submit` (CPU time of
`->submit_bio`), `remap_read`/`remap_write` (one cell-sized chunk), `idx_lookup`/`idx_insert`
(the mapping index), `dedup_digest` and `dedup_verify`. Histograms are log-linear with at most
6% bucket width. Counters cover splits, unmapped reads, overwrites, physical cell allocation,
`-ENOSPC` and dedup outcomes.

Stats can be switched off to measure their cost: `make build stats=0` compiles every probe out,
`insmod lsv.ko stats_enabled=0` (or `echo 0 > /sys/kernel/debug/lsv/enabled`) turns them into
patched-out branches.

Per-request tracepoints (`lsv_bio_submit`, `lsv_bio_remap`, `lsv_bio_complete`, `lsv_dedup`)
cost nothing until enabled:

```bash
echo 1 > /sys/kernel/tracing/events/lsv/enable
bpftrace -e 'tracepoint:lsv:lsv_bio_complete { @[args->op] = hist(args->lat_ns); }'
```

## Performance Evaluation

For performance measurements, you can use `test/main.sh`.
A detailed overview of the testing system is provided [here](https://github.com/qrutyy/ls-bdd/blob/main/test/README.md).

## License

Distributed under the [GPL-2.0 License](https://github.com/qrutyy/ls-bdd/blob/main/LICENSE).
