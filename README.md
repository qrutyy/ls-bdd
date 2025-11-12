# LS-BDD

**LS-BDD** is a testing framework for block devices that implement log-structured (LS) storage.
The log-structured approach is designed to accelerate certain read workflows by transforming random requests into sequential ones.
Although the log-structured concept is well known, the details of its implementation remain insufficiently studied.
This framework explores the efficiency and behavior of various data structures used in LS-based systems.

The framework provides a modifiable block device driver where the LS's underlying data structures can be added or modified.
The current version of the driver includes several already implemented data structures in both **lock-free** and **synchronous** versions: **B+-tree**, **RB-tree**, **Skiplist**, and **Hashtable**.

The driver is based on BIO request management and supports BIO splitting (i.e., different block sizes for operations, such as 4 KB writes and 16 KB reads).
A multithreaded lock-free implementation is currently under development.

For more information, see:

* [ru-presentation 2025](https://github.com/qrutyy/ls-bdd/blob/main/docs/4-semester/%28ru-presentation-conf%29%20Research%20on%20the%20implementation%20of%20log-structured%20block%20devicesd%20in%20Linux%20kernel.pdf)
* [eng-presentation (LS implementation)](https://github.com/qrutyy/ls-bdd/blob/main/docs/3-semester/%28eng-presentation%29%20Implementation%20of%20log-structured%20block%20device%20in%20Linux%20kernel.pdf)

***Compatible with Linux Kernel 6.15.7***


## Block Device Driver

It is highly recommended to test the driver in a virtual machine to prevent data corruption.


### Adding Your Data Structure

Here are the steps to add a new data structure as the underlying mechanism for storing the LBA–PBA mapping in LS:

1. Place your implementation in the **`utils/<mode>`** directory, where **`<mode>`** denotes the parallelism model of your data structure (*sync* or *lock-free*).
   **Note:** The method implementations must comply with the standard described in [this file](https://github.com/qrutyy/ls-bdd/blob/main/src/README.md).

2. Append the **`lsbdd-objs`** list in **`Kbuild`** with the path to the `.o` file of your data structure.

3. Update the **`ds_control`** system (both header and source files) with calls to your data structure’s API.

4. Assign a name to your data structure and add this identifier to the **`available_ds`** array in `main.h`.

5. Initialize the module by passing your identifier via the **`ds_name`** option.


### Initialization

```bash
make init DS="ds_name" TY="io_type" BD="bd_name"
```

* **`ds_name`** – one of the available data structures used for mapping storage (`bt`, `ht`, `sl`, `rb`, or your custom one)
* **`io_type`** – block device mode (`lf` – lock-free, `sy` – synchronous)
* **`bd_name`** – target block device (e.g., `ram0`, `vdb`, `sdc`, etc.)


### Sending Requests

#### Writing

```bash
dd if=/dev/urandom of=/dev/lsvbd1 oflag=direct bs=2K count=10
```

#### Reading

```bash
dd of=test2.txt if=/dev/lsvbd1 iflag=direct bs=4K count=10
```

### Testing

You can use the provided FIO tests (or write your own) that measure execution time and perform pattern verification:

```bash
make fio_verify IO=io_uring FS=lsvbd1 WBS=8 RBS=8 NJ=8 ID=8 SIZE=1000
```

A description of all options is provided in the `Makefile`.

If you need more customizable FIO testing, check the `test/fio/` directory for predefined configurations.

## Performance Evaluation

For performance measurements, you can use `test/main.sh`.
A detailed overview of the testing system is provided [here](https://github.com/qrutyy/ls-bdd/blob/main/test/README.md).

## License

Distributed under the [GPL-2.0 License](https://github.com/qrutyy/ls-bdd/blob/main/LICENSE).
