# Test System Documentation

The test framework is designed to benchmark, verify, and analyze the performance characteristics of log-structured block devices. It includes automated test orchestration, performance measurement, verification testing, and visualization capabilities.

**Limitations:**
* The performance measurement system does not support mixed workflows.
* Sadly, the code itself kinda lacks documentation. Feel free to contact me for details if youre stuck with something.

## lsv-bench

`lsvtest` is the Python harness of the driver: device control through the
`lsbdd` module parameters, declarative experiments and their statistical
report. It is backported to this version of the driver, the one the ISP RAS
2026 paper was measured on. The results of the paper were obtained with the
shell pipeline below, which is kept unchanged; `experiments/isp2026.toml`
(IOPS) and `experiments/isp2026_lat.toml` (latency) are the same experiment,
with the parameters of table 1 of the paper, written in the experiment
language.

```bash
cd test
python3 -m venv venv && . venv/bin/activate && pip install -r ../requirements.txt
make -C ../src type=lf                                 # or type=sy

sudo python3 -m lsvtest run experiments/smoke.toml     # results/smoke-<time>/
python3 -m lsvtest report results/smoke-<time>         # .../report/{summary.csv,*.png}
```

`run --dry-run` prints every device and fio command without touching the machine,
`run --resume DIR` continues an interrupted experiment, `run --ko PATH` points at
the module (default `../src/lsbdd.ko`). `load`, `create lsvbd<N>`, `list`,
`destroy` and `unload` drive the module by hand.

### Experiments

An experiment is a TOML file (see `experiments/`). `[matrix]` lists the values of
the device (`ds`: `bt`, `ht`, `sl`, `rb`) and workload (`rw`, `bs`, `numjobs`,
`iodepth`) parameters, and every combination becomes a point. A key may instead
be given as a scalar in `[device]` or `[fio]`. `rw` takes a read share for mixed
workloads: `randrw:70`. When `numjobs` and `iodepth` should vary together,
`concurrency = [[1, 4], [8, 32]]` gives the pairs instead of a cross product.

`[fio]` sets `ioengine` and `runtime`; `extra` passes any other fio option, and
its string values may refer to `{numjobs}`, `{iodepth}` and `{last_cpu}` of the
point, e.g. `cpus_allowed = "0-{last_cpu}"`.

`[run]` sets the methodology:

| Key            | Default | Meaning                                                            |
| -------------- | ------- | ------------------------------------------------------------------ |
| `repeats`      | 3       | runs per point; repeats are the outer loop, so drift spreads evenly |
| `baseline`     | false   | also run every workload on the bare backing device                  |
| `precondition` | auto    | write the region once before the run: `auto` (if it reads), `always`, `never` |
| `region`       | 0.25    | share of the device the workload touches                            |

`[tags]` are copied into every row, e.g. the lf/sy build of the module, so that
results of several builds can be reported together.

Limitations of this driver version, checked when the experiment is loaded:

* the lf/sy mode is a build option (`make -C ../src type=...`), not a point of
  the matrix: run once per build and tell the runs apart by `[tags]`;
* there are no cells, segments or deduplication, so `cell_size`,
  `segment_size` and `dedup` are refused;
* in the lock-free build only `sl` and `ht` are complete (see `src/README.md`).

Every run gets a fresh device. The head of the log is global to the module and
only reset when it is loaded, so the runner reloads `lsbdd` before every run.
lsbdd has no garbage collection and fails writes past the end of the backing
device, so runs that write are bounded by `io_size` (the free space after
preconditioning) and `runtime` caps them; read-only runs are time based. The
baseline does the same IO as its lsbdd twin.

### Results

```
results/<name>-<time>/
  experiment.toml    the spec as it was run
  meta.json          kernel, CPU, governors, block queue settings, git commit, fio version
  runs.csv           one row per run and IO direction
  failures.jsonl     runs fio failed on
  fio/<run_id>.json  raw fio output
  report/            summary.csv and plots
```

`runs.csv` holds the point parameters, the tags and fio's own metrics: IOPS,
bandwidth, CPU usage and mean/max/p50/p90/p99/p99.9/p99.99 of submission,
completion and total latency (in microseconds). `summary.csv` gives, per point,
the median over repeats with a bootstrap 95% confidence interval and the
coefficient of variation of IOPS.

### Unit tests

`pytest` in `test/` checks the harness itself against a fake `/sys` and canned
fio output; it needs neither root nor the module.

## Legacy shell pipeline (used for the paper)

The primary interface is `main.sh`, which orchestrates the entire test system workflow.

### Command Line Options

| Action                   | Option            | Description                                           |
| ------------------------ | ----------------- | ----------------------------------------------------- |
| **Run Test Scripts**     | `-a` / `--auto`   | Execute the complete automated test suite             |
| **Environment Setup**    | `-s` / `--setup`  | Initialize and configure the experimental environment |
| **Performance Analysis** | `-p` / `--perf`   | Generate flamegraphs for call stack analysis          |
| **Call Stack Analysis**  | `-c` / `--cplots` | Produce performance plots from automated test results |

### Configuration Parameters

Configuration parameters are defined in `configurable_params.sh`.
1. Edit this file as needed, then run `main.sh` with the desired mode.
2. Edit the `reinit_lsvbd` functions by removing the ramdisk initialisation if you are using some non-virtual device as an underlying one.

Then you can run the main script(e.g.):
```bash
./main.sh --setup --cplots
```

Plot titles can also be modified in `configurable_params.sh`.
Before calling plot generation - add your ds to the ds_colors.
The `plots.sh` script provides extensive customization options:

- **Test Types**
  * **IOPS Testing**: Evaluates input/output operations per second
  * **Latency Testing**: Analyzes response time characteristics

- **Concurrency Analysis**
  * **Parameter Sweep**: Tests various combinations of `iodepth` and `numjobs`
  * **Comparison Mode**: Compares raw block device performance with `lsvbd1`

- **Workload Parameters**
  * **I/O Workflow**: Configures read/write operation types
  * **Block Size Variations**: Tests different transfer sizes
  * **Access Patterns**: Sequential vs. random I/O workloads
  * **Workflow Duration**: Adjustable to simulate different scenarios


### Plots examples
P.S. The generated plots in this repository are in English.
| ![Figure 1](https://github.com/qrutyy/ls-bdd/blob/main/test/plots/iops/IOPS_general_hist_nj8_id32.png) | ![Figure 2](https://github.com/qrutyy/ls-bdd/blob/main/test/plots/iops/AVG_IOPS_bars_idnj_0-100_8_randrw.png) |
|----------------------------|----------------------------|
| *Figure 1: AVG IOPS for different operations with specified NJ/ID, BS*    | *Figure 2: AVG IOPS for different NJ/ID with specified BS*     |

| ![Figure 3](https://github.com/qrutyy/ls-bdd/blob/main/test/plots/lat/P99_LAT_general_hist_nj8_id32.png) |
|----------------------------|
| *Figure 3: P99 LAT for different operations with specified NJ/ID, BS*    |


## Manual Testing Tools

### FIO Workflow Directory

The `test/fio/` directory contains specialized FIO configurations for targeted testing:

* **Verification Workflows**: Validate data integrity and correctness
* **Performance Measurement**: Benchmark throughput, IOPS, and latency

Each workflow is provided in two forms: an FIO configuration file and a corresponding `.sh` script.

## Output and Results

### Directory Structure

```
test/
├── plots/          # Generated performance visualizations
├── logs/           # Test execution logs and debug output
├── fio/            # Manual FIO test configurations
└── manualsrc/      # Text files for manual testing
```

### Performance Data Files

Performance data files store extracted results from automated test suites for later dataframe processing and plot generation.

| File                  | Content                                                     |
| --------------------- | ----------------------------------------------------------- |
| `fio_results.dat`     | Primary FIO performance metrics collected during evaluation or detailed latency statistics and percentile data |
| `fio_{rw-mix}_run_id.log` | FIO performance for the specific run. (are cleared time to time) |
| `id_(s|c)lat.log` | Detailed FIO data about specific latency type for specific run. |
| `preconditioning.log` | Auxiliary log for preconditioning. Can be useful for debug/some inaccurate performance comparisons. |


#### Latency results

Its important to mention that `fio_results.dat` consists not only from default AVG latency measurements, but also P99 and MAX for each SLAT (submission latency), CLAT(completion latency), and LAT(general latency).

More detailed structure of the `fio_results` is represented below:

|Index | **LAT** |  **IOPS** |
| ---- | --------------------- | ----------------------------------------------------------- |
| 1    | RunID | RunID |
| 2 | Data structure | Data structure |
| 3 | Block size | Block size |
| 4 | Read-Write mix (0-100/100-0) | Read-Write mix (0-100/100-0) |
| 5 | Read-Write type (sequential/random) | _ |
| 6 | "LAT" string | IOPS median value |
| 7 | Average SLAT | "IOPS" string |
| 8 | Average CLAT | Read-Write type (sequential/random) |
| 9 | Average LAT | IO depth |
| 10 | Max SLAT | Jobs number |
| 11 | Max CLAT | |
| 12 | Max LAT | |
| 13 | P99 SLAT | |
| 14 | P99 CLAT | |
| 15 | P99 LAT | |
| 16 | IO depth | |
| 17 | Jobs number | |

Implementation of parsers are presented in `plots.sh` inside  `extract_lat_metrics` and `extract_iops_metrics` function respectively.

P.S. by AVG i mean median

### Workflow Templates (`Makefile`)

The `Makefile` provides customizable workflow templates for:

* Standard performance benchmarks
* Verification test procedures
* Custom test parameter combinations
